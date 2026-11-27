/*
 * bootstate.c — Open-source replacement for Broadcom /bin/bcm_bootstate +
 *               pureUBI metadata1/2 and bcmFlash_getIdent from libbcm_flashutil.so
 *
 * Verified against /opt/bcm/libexec/bcm_bootstate, /opt/bcm/lib/libbcm_flashutil.so,
 * and live HG6371F backups/metadata1.bin:
 *   - metadata1/2: 1280 B UBI static volumes (/dev/ubi0_1, /dev/ubi0_2)
 *     +0x00 u32 = 0x100
 *     +0x04 u32 = 0x100 (size <= 0x500)
 *     +0x08 u32 = standard IEEE CRC32 (poly 0xEDB88320) over [0x0c, 0x0c + size - 4)
 *     +0x0c NUL-separated ASCII KV lines: COMMITTED=%d, VALID=%d,%d, SEQ=%d,%d
 *   - Direct UBI write via UBI_IOCVOLUP (0x40084f00) BEFORE write(fd, buf, 1280)
 *   - Booted partition from /proc/device-tree/chosen/active_image (BE u32)
 *     with fallback to /proc/cmdline (ubiblock0_4 -> 1, ubiblock0_6 -> 2)
 *   - ONCE flag via /dev/brcmboard ioctl 0xc018423e + /proc/bootstate/reset_reason
 *   - Image tag via bcm_flash_get_ident() scanning bootfs1 (/dev/ubi0_3) / bootfs2 (/dev/ubi0_5)
 */
#include "bcmopen.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __linux__
#define BCM_FOPEN_R  "re"
#define BCM_FOPEN_RB "rbe"
#define BCM_FOPEN_W  "we"
#else
#define BCM_FOPEN_R  "r"
#define BCM_FOPEN_RB "rb"
#define BCM_FOPEN_W  "w"
#endif

static pthread_mutex_t g_bootstate_mu = PTHREAD_MUTEX_INITIALIZER;

#ifndef BCM_UBI_META_FMT
#define BCM_UBI_META_FMT      "/dev/ubi0_%d"
#endif
#ifndef BCM_DT_ACTIVE_IMAGE
#define BCM_DT_ACTIVE_IMAGE   "/proc/device-tree/chosen/active_image"
#endif
#ifndef BCM_RESET_REASON_PATH
#define BCM_RESET_REASON_PATH "/proc/bootstate/reset_reason"
#endif
#ifndef BCM_BRCMBOARD_DEV
#define BCM_BRCMBOARD_DEV     "/dev/brcmboard"
#endif

/* Linux UBI_IOCVOLUP = _IOW('O', 0, int64_t) = 0x40084f00 (UBI_VOL_IOC_MAGIC='O') */
#define BCM_UBI_IOCVOLUP      0x40084f00UL
/* Linux MTD MEMGETINFO = _IOR('M', 1, struct mtd_info_user) = 0x80204d01 */
#define BCM_MTD_MEMGETINFO    0x80204d01UL

static int find_ubi_vol_by_name(const char *want, int fallback_idx, char *out, size_t out_sz) {
    DIR *d = opendir("/sys/class/ubi");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strncmp(ent->d_name, "ubi0_", 5) != 0) continue;
            char npath[128];
            snprintf(npath, sizeof(npath), "/sys/class/ubi/%s/name", ent->d_name);
            FILE *fp = fopen(npath, BCM_FOPEN_R);
            if (!fp) continue;
            char name[64] = {0};
            if (fgets(name, sizeof(name), fp)) {
                char *nl = strpbrk(name, "\r\n");
                if (nl) *nl = '\0';
                if (strcmp(name, want) == 0) {
                    fclose(fp);
                    closedir(d);
                    snprintf(out, out_sz, "/dev/%s", ent->d_name);
                    return 0;
                }
            }
            fclose(fp);
        }
        closedir(d);
    }
    snprintf(out, out_sz, BCM_UBI_META_FMT, fallback_idx);
    return 0;
}

static size_t query_mtd_erasesize(void) {
    char mtd_dev[32] = "/dev/mtd2";
    FILE *mfp = fopen("/proc/mtd", BCM_FOPEN_R);
    if (mfp) {
        char line[256];
        while (fgets(line, sizeof(line), mfp)) {
            if (strstr(line, "\"image\"")) {
                char devname[16] = {0};
                if (sscanf(line, "%15[^:]:", devname) == 1 && strncmp(devname, "mtd", 3) == 0) {
                    snprintf(mtd_dev, sizeof(mtd_dev), "/dev/%s", devname);
                    break;
                }
            }
        }
        fclose(mfp);
    }
    int fd = open(mtd_dev, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        uint8_t mi[32];
        memset(mi, 0, sizeof(mi));
        if (ioctl(fd, BCM_MTD_MEMGETINFO, mi) == 0) {
            uint32_t esz = (uint32_t)mi[12] | ((uint32_t)mi[13] << 8) |
                           ((uint32_t)mi[14] << 16) | ((uint32_t)mi[15] << 24);
            close(fd);
            if (esz >= 4096 && esz <= 4 * 1024 * 1024) return (size_t)esz;
            return 128 * 1024;
        }
        close(fd);
    }
    return 128 * 1024;
}

static inline uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

uint32_t bcm_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

int bcm_meta_parse(const uint8_t *buf, size_t len, bcm_boot_meta_t *out) {
    if (!buf || !out || len < BCM_META_SIZE_FIELD) return -1;
    uint32_t sz = rd_le32(buf + 0x04);
    if (sz <= 4 || sz > BCM_META_VOL_BYTES || (size_t)(BCM_META_HDR_BYTES + sz - 4) > len)
        return -1;
    uint32_t stored = rd_le32(buf + 0x08);
    uint32_t calc = bcm_crc32(buf + BCM_META_HDR_BYTES, sz - 4);
    if (stored != calc) return -1;

    bcm_boot_meta_t m = { 0, {0, 0}, {-1, -1} };
    const char *p = (const char *)(buf + BCM_META_HDR_BYTES);
    const char *end = p + (sz - 4);
    int seen_comm = 0, seen_valid = 0, seen_seq = 0;

    while (p < end && *p != '\0') {
        size_t slen = strnlen(p, (size_t)(end - p));
        if (slen == 0) break;
        if (p + slen >= end) return -1;
        int a = 0, b = 0;
        if (sscanf(p, "COMMITTED=%d", &a) == 1) {
            m.committed = a;
            seen_comm = 1;
        } else if (sscanf(p, "VALID=%d,%d", &a, &b) == 2) {
            if (a < 0 || a > 2 || b < 0 || b > 2) return -1;
            if (a == 1 || a == 2) m.valid[a - 1] = a;
            if (b == 1 || b == 2) m.valid[b - 1] = b;
            seen_valid = 1;
        } else if (sscanf(p, "VALID=%d", &a) == 1) {
            if (a < 0 || a > 2) return -1;
            if (a == 1 || a == 2) m.valid[a - 1] = a;
            seen_valid = 1;
        } else if (sscanf(p, "SEQ=%d,%d", &a, &b) == 2) {
            m.seq[0] = a;
            m.seq[1] = b;
            seen_seq = 1;
        }
        p += slen + 1;
    }
    if (!seen_comm || !seen_valid) return -1;
    if (!seen_seq) {
        /* Synthesize default seq from committed/valid (flashutil_meta.asm:0x9c80..0x9d84) */
        if (m.committed == 1) {
            m.seq[0] = (m.valid[0] == 1) ? 1 : -1;
            m.seq[1] = (m.valid[1] == 2) ? 0 : -1;
        } else if (m.committed == 2) {
            m.seq[0] = (m.valid[0] == 1) ? 0 : -1;
            m.seq[1] = (m.valid[1] == 2) ? 1 : -1;
        } else {
            m.seq[0] = (m.valid[0] == 1) ? 0 : -1;
            m.seq[1] = (m.valid[1] == 2) ? 0 : -1;
        }
    }
    *out = m;
    return 0;
}

int bcm_meta_pack(const bcm_boot_meta_t *m, uint8_t *buf, size_t len) {
    if (!m || !buf || len < BCM_META_VOL_BYTES) return -1;
    /* Fill unused tail with 0xFF matching NAND/UBI erased pattern, zero string area */
    memset(buf, 0xff, BCM_META_VOL_BYTES);
    memset(buf, 0, BCM_META_HDR_BYTES + BCM_META_CRC_LEN);

    wr_le32(buf + 0x00, BCM_META_SIZE_FIELD);
    wr_le32(buf + 0x04, BCM_META_SIZE_FIELD);

    char *p = (char *)(buf + BCM_META_HDR_BYTES);
    p += sprintf(p, "COMMITTED=%d", m->committed) + 1;
    /* Stock flashutil_meta.asm:0x9e70..0x9e8c ALWAYS emits VALID=%d,%d */
    p += sprintf(p, "VALID=%d,%d", m->valid[0], m->valid[1]) + 1;
    p += sprintf(p, "SEQ=%d,%d", m->seq[0], m->seq[1]) + 1;
    *p = '\0';

    uint32_t crc = bcm_crc32(buf + BCM_META_HDR_BYTES, BCM_META_CRC_LEN);
    wr_le32(buf + 0x08, crc);
    return 0;
}

static int read_meta_vol(int idx, bcm_boot_meta_t *out, int verbose) {
    char path[64];
    find_ubi_vol_by_name((idx == 1) ? "metadata1" : "metadata2", idx, path, sizeof(path));
    FILE *fp = fopen(path, BCM_FOPEN_RB);
    if (!fp) return -1;
    uint8_t buf[BCM_META_VOL_BYTES];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < BCM_META_SIZE_FIELD) return -1;
    if (bcm_meta_parse(buf, n, out) != 0) return -1;
    if (verbose) {
        printf("rd_metadata: committed %d valid %d,%d seq %d,%d\n",
               out->committed, out->valid[0], out->valid[1],
               out->seq[0], out->seq[1]);
    }
    return 0;
}

static int read_meta_any(bcm_boot_meta_t *out, int verbose) {
    if (read_meta_vol(1, out, verbose) == 0) return 0;
    return read_meta_vol(2, out, verbose);
}

static int write_meta_vol(int idx, const uint8_t *buf, int verbose) {
    char path[64];
    find_ubi_vol_by_name((idx == 1) ? "metadata1" : "metadata2", idx, path, sizeof(path));
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (verbose) fprintf(stderr, "bcm_bootstate: open %s failed (errno=%d)\n", path, errno);
        return -1;
    }

    /* CRITICAL: UBI_IOCVOLUP MUST be called BEFORE write() on UBI volumes */
    int64_t vol_bytes = BCM_META_VOL_BYTES;
    if (ioctl(fd, BCM_UBI_IOCVOLUP, &vol_bytes) < 0) {
        int err = errno;
        close(fd);
        if (verbose) fprintf(stderr, "bcm_bootstate: UBI_IOCVOLUP %s failed (errno=%d)\n", path, err);
        return -1;
    }
    size_t off = 0;
    while (off < BCM_META_VOL_BYTES) {
        ssize_t w = write(fd, buf + off, BCM_META_VOL_BYTES - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            int err = errno;
            close(fd);
            if (verbose) fprintf(stderr, "bcm_bootstate: write %s failed (errno=%d)\n", path, err);
            return -1;
        }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

static int write_meta_both(const bcm_boot_meta_t *m, int verbose) {
    uint8_t buf[BCM_META_VOL_BYTES];
    if (bcm_meta_pack(m, buf, sizeof(buf)) != 0) return -1;
    if (verbose) {
        printf("wr_metadata: committed %d valid %d,%d seq %d,%d\n",
               m->committed, m->valid[0], m->valid[1], m->seq[0], m->seq[1]);
    }
    int r1 = write_meta_vol(1, buf, verbose);
    int r2 = write_meta_vol(2, buf, verbose);
    return (r1 == 0 && r2 == 0) ? 0 : -1;
}

static int read_booted_partition(void) {
    FILE *fp = fopen(BCM_DT_ACTIVE_IMAGE, BCM_FOPEN_RB);
    if (fp) {
        uint8_t b[4] = {0};
        size_t n = fread(b, 1, 4, fp);
        fclose(fp);
        if (n == 4) {
            uint32_t v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                         ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
            if (v == 1 || v == 2) return (int)v;
        }
    }
    fp = fopen("/proc/cmdline", BCM_FOPEN_R);
    if (fp) {
        char line[1024];
        if (fgets(line, sizeof(line), fp)) {
            fclose(fp);
            if (strstr(line, "ubiblock0_6") || strstr(line, "img_index=2")) return 2;
            if (strstr(line, "ubiblock0_4") || strstr(line, "img_index=1")) return 1;
            return 1;
        }
        fclose(fp);
    }
    return 1;
}

static int read_boot_reason(void) {
    FILE *fp = fopen(BCM_RESET_REASON_PATH, BCM_FOPEN_R);
    if (!fp) return 0;
    char buf[64] = {0};
    int v = 0;
    if (fgets(buf, sizeof(buf), fp)) {
        v = (int)strtoul(buf, NULL, 16);
    }
    fclose(fp);
    return v;
}

static int write_boot_reason(int v) {
    FILE *fp = fopen(BCM_RESET_REASON_PATH, BCM_FOPEN_W);
    if (!fp) return -1;
    int err = (fprintf(fp, "0x%x\n", (unsigned)v) < 0) || (fflush(fp) != 0);
    if (fclose(fp) != 0) err = 1;
    return err ? -1 : 0;
}

/*
 * Scan bootfs1 (/dev/ubi0_3) or bootfs2 (/dev/ubi0_5) for "$<key>... $"
 * Matches libbcm_flashutil.so bcmFlash_getIdent (0xb030..0xb520).
 */
int bcm_flash_get_ident(int partition, int *start_blk, int *end_blk,
                        const char *key, char *out, int out_sz) {
    if (!start_blk || !end_blk || !out || out_sz <= 0) return -1;
    int booted = read_booted_partition();
    int part = 1;
    if (partition == 1) part = 1;
    else if (partition == 2) part = 2;
    else if (partition == 3) part = booted;
    else if (partition == 4) part = (booted == 1) ? 2 : 1;
    else return -1;

    char dev[64];
    find_ubi_vol_by_name((part == 1) ? "bootfs1" : "bootfs2",
                         (part == 1) ? 3 : 5, dev, sizeof(dev));
    FILE *fp = fopen(dev, BCM_FOPEN_RB);
    if (!fp) {
        out[0] = '\0';
        return -1;
    }

    const size_t blk_sz = query_mtd_erasesize();
    uint8_t *buf = malloc(blk_sz);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    char needle[64];
    if (key && *key) snprintf(needle, sizeof(needle), "$%s", key);
    else strcpy(needle, "$");
    size_t nlen = strlen(needle);

    if (*start_blk < 0) *start_blk = 0;
    if (*start_blk > 0 && fseeko(fp, (off_t)(*start_blk) * (off_t)blk_sz, SEEK_SET) != 0) {
        free(buf);
        fclose(fp);
        return -1;
    }

    int written = 0;
    out[0] = '\0';
    for (;;) {
        if (*end_blk >= 0 && *start_blk > *end_blk) break;
        size_t rd = fread(buf, 1, blk_sz, fp);
        if (rd == 0) break;

        for (size_t i = 0; i + nlen + 2 < rd; i++) {
            if (memcmp(buf + i, needle, nlen) != 0) continue;
            size_t j = nlen;
            if (!key || !*key) {
                while (i + j < rd && (isalnum(buf[i + j]) || buf[i + j] == '_')) j++;
            }
            if (i + j >= rd || buf[i + j] != ':') continue;
            j++;
            int matched = 0;
            while (i + j + 1 < rd) {
                uint8_t c = buf[i + j];
                if (c < 0x20 || c > 0x7e) break;
                if (buf[i + j] == ' ' && buf[i + j + 1] == '$') {
                    j += 2;
                    matched = 1;
                    break;
                }
                j++;
            }
            if (!matched) continue;
            if ((int)j < out_sz - written) {
                memcpy(out + written, buf + i, j);
                written += (int)j;
                out[written] = '\0';
            }
            if (key && *key) {
                free(buf);
                fclose(fp);
                return written;
            }
            i += j - 1;
        }
        (*start_blk)++;
    }

    free(buf);
    fclose(fp);
    return written;
}

static void derive_state(const bcm_boot_meta_t *m, int booted, int reason,
                         int *out_state, int *out_reboot) {
    int st = (booted == 1) ? '5' : '6';
    if (m->committed == 1)      st = (reason == 1) ? '8' : '5';
    else if (m->committed == 2) st = (reason == 1) ? '7' : '6';
    *out_state = st;
    *out_reboot = (st == '5' || st == '7') ? 1 : 2;
}

static int resolve_state_char(int ch, int seq1, int seq2) {
    /* 0/999 wraparound rule verified at libbcm_flashutil.so 0xbf14..0xbf38 */
    if (seq1 == 0 && seq2 == 999) seq1 = 1000;
    else if (seq2 == 0 && seq1 == 999) seq2 = 1000;

    switch (ch) {
    case '0': return (seq1 > seq2) ? '5' : '6'; /* NEW */
    case '1': return (seq1 < seq2) ? '5' : '6'; /* OLD */
    case '2': return (seq1 > seq2) ? '7' : '8'; /* NEW_ONCE */
    case 'A': return (seq1 < seq2) ? '7' : '8'; /* OLD_ONCE */
    default:  return ch;
    }
}

int bcm_bootstate_read(bcm_bootstate_t *out) {
    if (!out) return -1;
    pthread_mutex_lock(&g_bootstate_mu);
    memset(out, 0, sizeof(*out));
    out->booted_part = read_booted_partition();
    out->once_pending = (read_boot_reason() == 1) ? 1 : 0;
    if (read_meta_any(&out->meta, 0) != 0) {
        pthread_mutex_unlock(&g_bootstate_mu);
        return -1;
    }
    derive_state(&out->meta, out->booted_part, out->once_pending,
                 &out->state_char, &out->reboot_part);
    for (int p = 1; p <= 2; p++) {
        int sb = -1, eb = -1;
        bcm_flash_get_ident(p, &sb, &eb, "imageversion",
                            out->img_tag[p - 1], (int)sizeof(out->img_tag[0]));
    }
    pthread_mutex_unlock(&g_bootstate_mu);
    return 0;
}

static int bootstate_commit_ex(int part, int flag, int verbose) {
    if (part != 1 && part != 2) return -1;
    pthread_mutex_lock(&g_bootstate_mu);
    bcm_boot_meta_t m;
    if (read_meta_any(&m, verbose) != 0) {
        pthread_mutex_unlock(&g_bootstate_mu);
        return -1;
    }
    if (flag) {
        m.committed = part;
        m.valid[part - 1] = part;
    } else {
        if (m.committed == part) m.committed = 0;
    }
    int rc = write_meta_both(&m, verbose);
    pthread_mutex_unlock(&g_bootstate_mu);
    return rc;
}

int bcm_bootstate_commit(int part, int flag) {
    return bootstate_commit_ex(part, flag, 0);
}

static int bootstate_set_code_ex(int code, int verbose) {
    /* Intentional 6->'7', 7->'6' crossover verified at bcm_bootstate 0x10b00..0x10c90 */
    int ch = 0;
    switch (code) {
    case 1: ch = '0'; break;
    case 2: ch = '1'; break;
    case 3: ch = '2'; break;
    case 4: ch = 'A'; break;
    case 5: ch = '5'; break;
    case 6: ch = '7'; break;
    case 7: ch = '6'; break;
    case 8: ch = '8'; break;
    default: return -1;
    }

    pthread_mutex_lock(&g_bootstate_mu);
    bcm_boot_meta_t m;
    if (read_meta_any(&m, verbose) != 0) {
        pthread_mutex_unlock(&g_bootstate_mu);
        return -1;
    }
    int booted = read_booted_partition();
    int cur_state = 0, cur_reboot = 0;
    derive_state(&m, booted, read_boot_reason(), &cur_state, &cur_reboot);

    int target = resolve_state_char(ch, m.seq[0], m.seq[1]);
    if (target == cur_state) {
        pthread_mutex_unlock(&g_bootstate_mu);
        return 0;
    }

    int rc = 0;
    if (target == '5') {
        if (m.valid[0] != 1) {
            rc = -1;
        } else {
            if (m.committed != 1) {
                m.committed = 1;
                rc = write_meta_both(&m, verbose);
            }
            if (rc == 0 && write_boot_reason(0) != 0) rc = -1;
        }
    } else if (target == '6') {
        if (m.valid[1] != 2) {
            rc = -1;
        } else {
            if (m.committed != 2) {
                m.committed = 2;
                rc = write_meta_both(&m, verbose);
            }
            if (rc == 0 && write_boot_reason(0) != 0) rc = -1;
        }
    } else if (target == '7') {
        /* PART1_ONCE (pureUBI): committed must stay on 2, set /proc/bootstate/reset_reason */
        if (m.valid[0] != 1 || m.valid[1] != 2) {
            rc = -1;
        } else {
            if (m.committed != 2) {
                m.committed = 2;
                rc = write_meta_both(&m, verbose);
            }
            if (rc == 0 && write_boot_reason(1) != 0) rc = -1;
        }
    } else if (target == '8') {
        /* PART2_ONCE (pureUBI): committed must stay on 1, set /proc/bootstate/reset_reason */
        if (m.valid[0] != 1 || m.valid[1] != 2) {
            rc = -1;
        } else {
            if (m.committed != 1) {
                m.committed = 1;
                rc = write_meta_both(&m, verbose);
            }
            if (rc == 0 && write_boot_reason(1) != 0) rc = -1;
        }
    }
    pthread_mutex_unlock(&g_bootstate_mu);
    return rc;
}

int bcm_bootstate_set_code(int code) {
    return bootstate_set_code_ex(code, 0);
}

static const char *state_char_name(int ch) {
    switch (ch) {
    case '0': return "BOOT_SET_NEW_IMAGE";
    case '1': return "BOOT_SET_OLD_IMAGE";
    case '2': return "BOOT_SET_NEW_IMAGE_ONCE";
    case 'A': return "BOOT_SET_OLD_IMAGE_ONCE";
    case '5': return "BOOT_SET_PART1_IMAGE";
    case '7': return "BOOT_SET_PART1_IMAGE_ONCE";
    case '6': return "BOOT_SET_PART2_IMAGE";
    case '8': return "BOOT_SET_PART2_IMAGE_ONCE";
    default:  return "UNKNOWN (boot latest image)";
    }
}

static void bootstate_print_usage(const char *prog) {
    printf("\nTo set boot state usage: %s [state], where your state options are:\n", prog);
    puts("  1 or BOOT_SET_NEW_IMAGE");
    puts("  2 or BOOT_SET_OLD_IMAGE");
    puts("  3 or BOOT_SET_NEW_IMAGE_ONCE");
    puts("  4 or BOOT_SET_OLD_IMAGE_ONCE");
    puts("  5 or BOOT_SET_PART1_IMAGE");
    puts("  6 or BOOT_SET_PART1_IMAGE_ONCE");
    puts("  7 or BOOT_SET_PART2_IMAGE");
    puts("  8 or BOOT_SET_PART2_IMAGE_ONCE");
    puts("  -------- test options --------");
    puts("  +1 ------ commit    partition 1");
    puts("  -1 ------ uncommit  partition 1");
    puts("  +2 ------ commit    partition 2");
    puts("  -2 ------ uncommit  partition 2");
}

static int parse_state_arg(const char *s) {
    if (strcmp(s, "1") == 0 || strcmp(s, "BOOT_SET_NEW_IMAGE") == 0) return 1;
    if (strcmp(s, "2") == 0 || strcmp(s, "BOOT_SET_OLD_IMAGE") == 0) return 2;
    if (strcmp(s, "3") == 0 || strcmp(s, "BOOT_SET_NEW_IMAGE_ONCE") == 0) return 3;
    if (strcmp(s, "4") == 0 || strcmp(s, "BOOT_SET_OLD_IMAGE_ONCE") == 0) return 4;
    if (strcmp(s, "5") == 0 || strcmp(s, "BOOT_SET_PART1_IMAGE") == 0) return 5;
    if (strcmp(s, "6") == 0 || strcmp(s, "BOOT_SET_PART1_IMAGE_ONCE") == 0) return 6;
    if (strcmp(s, "7") == 0 || strcmp(s, "BOOT_SET_PART2_IMAGE") == 0) return 7;
    if (strcmp(s, "8") == 0 || strcmp(s, "BOOT_SET_PART2_IMAGE_ONCE") == 0) return 8;
    return -1;
}

int bcm_cli_bootstate(int argc, char **argv) {
    if (argc == 2) {
        const char *arg = argv[1];
        if (strcmp(arg, "+1") == 0) return bootstate_commit_ex(1, 1, 1) == 0 ? 0 : 255;
        if (strcmp(arg, "-1") == 0) return bootstate_commit_ex(1, 0, 1) == 0 ? 0 : 255;
        if (strcmp(arg, "+2") == 0) return bootstate_commit_ex(2, 1, 1) == 0 ? 0 : 255;
        if (strcmp(arg, "-2") == 0) return bootstate_commit_ex(2, 0, 1) == 0 ? 0 : 255;

        int code = parse_state_arg(arg);
        if (code < 0) {
            fprintf(stderr, "%s is an unknown boot state to set\n", arg);
            return 255;
        }
        return bootstate_set_code_ex(code, 1) == 0 ? 0 : 255;
    }

    bcm_bootstate_t st;
    if (bcm_bootstate_read(&st) != 0) {
        fprintf(stderr, "bcm_bootstate: failed to read metadata\n");
        return 255;
    }

    printf("rd_metadata: committed %d valid %d,%d seq %d,%d\n",
           st.meta.committed, st.meta.valid[0], st.meta.valid[1],
           st.meta.seq[0], st.meta.seq[1]);
    printf("\n  Boot image state: %s\n", state_char_name(st.state_char));
    printf("  Booted Partition: %s\n", st.booted_part == 1 ? "First" : "Second");
    printf("  Reboot Partition: %s\n\n", st.reboot_part == 1 ? "First" : "Second");

    for (int p = 1; p <= 2; p++) {
        char b = (st.booted_part == p) ? 'B' : ' ';
        char r = (st.reboot_part == p) ? '>' : ' ';
        const char *label = (p == 1) ? "First " : "Second";
        int seq = st.meta.seq[p - 1];
        const char *type = (seq < 0) ? "NONE" : "pureUBI/GPT";
        const char *cflag = (st.meta.committed == p) ? "1" : "0";
        printf("%c%c%s partition type           : %s\n", b, r, label, type);
        printf("%c%c%s partition sequence number: %d\n", b, r, label, seq);
        printf("%c%c%s partition image tag      : %s\n", b, r, label, st.img_tag[p - 1]);
        printf("%c%c%s partition commit flag    : %s\n", b, r, label, cflag);
    }

    bootstate_print_usage(argv[0]);
    return 0;
}
