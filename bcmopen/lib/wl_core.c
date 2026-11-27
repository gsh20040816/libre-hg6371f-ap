/*
 * wl_core.c — Open-source Broadcom wl core engine (SIOCDEVPRIVATE = 0x89f0)
 *
 * Implements:
 *   - Thread-safe public C API (bcm_wl_*) for static linking / Go CGO
 *   - D11REV >= 40 chanspec encoder/decoder
 *   - All 10 generic handlers for WL_GEN_TABLE[] (wl_table.h)
 *   - Universal `wl dump <subsys>`, raw `wl iovar`/`wl ioctl` explorer,
 *     `wl cmds`/`wl help`, and generic int32 IOVAR fallback
 *   - CLI dispatcher `bcm_cli_wl()`
 */
#include "wl_internal.h"
#include "wl_table.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef BCMOPEN_HOST_TEST
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* =========================================================================
 * Low-level SIOCDEVPRIVATE transport (thread-safe, zero global mutable buf)
 * ========================================================================= */
int bcm_wl_ioctl(const char *ifname, int cmd, void *buf, size_t len, int set) {
    if (!ifname || !ifname[0]) return -EINVAL;
    if (strlen(ifname) >= 16) return -ENAMETOOLONG; /* IFNAMSIZ == 16 on Linux */
    if (len > UINT32_MAX) return -EINVAL;
    if (len > 0 && !buf) return -EINVAL;
#ifdef BCMOPEN_HOST_TEST
    (void)cmd; (void)buf; (void)len; (void)set;
    return -ENOSYS;
#else
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    wl_ioctl_t ioc;
    memset(&ioc, 0, sizeof(ioc));
    ioc.cmd = (uint32_t)cmd;
    ioc.buf = buf;
    ioc.len = (uint32_t)len;
    ioc.set = set ? 1 : 0;

    ifr.ifr_data = (char *)&ioc;

    int rc = ioctl(fd, SIOCDEVPRIVATE, &ifr);
    int saved = errno;
    close(fd);
    if (rc < 0) return saved ? -saved : -EIO;
    return 0;
#endif
}

int bcm_wl_find_iface(char *out_ifname, size_t out_sz) {
    if (!out_ifname || out_sz < 4) return -EINVAL;
#ifdef BCMOPEN_HOST_TEST
    snprintf(out_ifname, out_sz, "wl0");
    return 0;
#else
    FILE *fp = fopen("/proc/net/dev", "re");
    if (!fp) {
        /* Fallback probe wl0 / wl1 directly */
        const char *fallbacks[] = { "wl0", "wl1", "eth1", "eth2" };
        for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
            uint32_t magic = 0;
            if (bcm_wl_ioctl(fallbacks[i], WLC_GET_MAGIC, &magic, sizeof(magic), 0) == 0 &&
                magic == BCM_WL_MAGIC) {
                int n = snprintf(out_ifname, out_sz, "%s", fallbacks[i]);
                return (n < 0 || (size_t)n >= out_sz) ? -ENOBUFS : 0;
            }
        }
        return -ENODEV;
    }

    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (++lineno <= 2) continue;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *colon = strchr(p, ':');
        if (!colon) continue;
        *colon = '\0';
        uint32_t magic = 0;
        if (bcm_wl_ioctl(p, WLC_GET_MAGIC, &magic, sizeof(magic), 0) == 0 &&
            magic == BCM_WL_MAGIC) {
            fclose(fp);
            int n = snprintf(out_ifname, out_sz, "%s", p);
            return (n < 0 || (size_t)n >= out_sz) ? -ENOBUFS : 0;
        }
    }
    fclose(fp);
    return -ENODEV;
#endif
}

/* =========================================================================
 * IOVAR & BSSCFG IOVAR helpers
 * ========================================================================= */
static int wl_add_size(size_t a, size_t b, size_t *out) {
    if (b > SIZE_MAX - a) return -EOVERFLOW;
    *out = a + b;
    return 0;
}

static int wl_iovar_pack(char *dst, size_t dst_sz,
                         int bsscfg_idx, const char *name,
                         const void *p1, size_t p1_len,
                         const void *p2, size_t p2_len,
                         size_t *out_used) {
    size_t nlen = strlen(name) + 1;
    size_t total = (bsscfg_idx >= 0) ? 11u : 0u; /* "bsscfg:" (7) + idx (4) */
    if (wl_add_size(total, nlen, &total) < 0 ||
        wl_add_size(total, p1_len, &total) < 0 ||
        wl_add_size(total, p2_len, &total) < 0) {
        return -EOVERFLOW;
    }
    if (total > dst_sz) return -ENOBUFS;

    size_t off = 0;
    if (bsscfg_idx >= 0) {
        memcpy(dst + off, "bsscfg:", 7);
        off += 7;
    }
    memcpy(dst + off, name, nlen);
    off += nlen;
    if (bsscfg_idx >= 0) {
        int32_t idx32 = (int32_t)bsscfg_idx;
        memcpy(dst + off, &idx32, 4);
        off += 4;
    }
    if (p1 && p1_len > 0) {
        memcpy(dst + off, p1, p1_len);
        off += p1_len;
    }
    if (p2 && p2_len > 0) {
        memcpy(dst + off, p2, p2_len);
        off += p2_len;
    }
    if (out_used) *out_used = off;
    return 0;
}

static int wl_ranges_overlap(const void *a, size_t alen, const void *b, size_t blen) {
    if (!a || !b || alen == 0 || blen == 0) return 0;
    uintptr_t a0 = (uintptr_t)a, b0 = (uintptr_t)b;
    return (a0 < b0) ? ((b0 - a0) < alen) : ((a0 - b0) < blen);
}

static int wl_iovar_getbuf_ex(const char *ifname, int bsscfg_idx, const char *name,
                              const void *param, size_t param_len,
                              void *out, size_t out_len) {
    if (!ifname || !name || !name[0] || !out || out_len == 0) return -EINVAL;
    if (param_len > 0 && !param) return -EINVAL;
    size_t nlen = strlen(name) + 1u;
    size_t hdr_len = (bsscfg_idx >= 0) ? 11u : 0u;
    if (wl_add_size(hdr_len, nlen, &hdr_len) < 0 ||
        wl_add_size(hdr_len, param_len, &hdr_len) < 0) {
        return -EOVERFLOW;
    }
    size_t ioctl_len = (out_len >= hdr_len) ? out_len : hdr_len;
    if (ioctl_len > UINT32_MAX) return -EINVAL;

    uint8_t stack_buf[WL_IOCTL_MEDLEN];
    uint8_t *work = NULL;
    int heap = 0;
    int overlaps = wl_ranges_overlap(param, param_len, out, out_len) ||
                   wl_ranges_overlap(name, nlen, out, out_len) ||
                   wl_ranges_overlap(ifname, strlen(ifname) + 1u, out, out_len);

    if (out_len >= hdr_len && !overlaps) {
        /* Safe to pack directly into caller's output buffer */
        work = (uint8_t *)out;
    } else if (ioctl_len <= sizeof(stack_buf)) {
        work = stack_buf;
    } else {
        work = (uint8_t *)calloc(1, ioctl_len);
        if (!work) return -ENOMEM;
        heap = 1;
    }

    size_t packed = 0;
    int rc = wl_iovar_pack((char *)work, ioctl_len, bsscfg_idx, name,
                           param, param_len, NULL, 0, &packed);
    if (rc < 0) {
        if (heap) free(work);
        return rc;
    }
    if (ioctl_len > packed) {
        memset(work + packed, 0, ioctl_len - packed);
    }

    rc = bcm_wl_ioctl(ifname, WLC_GET_VAR, work, ioctl_len, 0);
    if (rc == 0 && work != (uint8_t *)out) {
        memcpy(out, work, out_len);
    }
    if (heap) free(work);
    return rc;
}

static int wl_iovar_setbuf_ex(const char *ifname, int bsscfg_idx, const char *name,
                              const void *param, size_t param_len,
                              const void *val, size_t val_len) {
    if (!ifname || !name || !name[0]) return -EINVAL;
    if ((param_len > 0 && !param) || (val_len > 0 && !val)) return -EINVAL;
    size_t total = (bsscfg_idx >= 0) ? 11u : 0u;
    if (wl_add_size(total, strlen(name) + 1u, &total) < 0 ||
        wl_add_size(total, param_len, &total) < 0 ||
        wl_add_size(total, val_len, &total) < 0) {
        return -EOVERFLOW;
    }
    if (total > UINT32_MAX) return -EINVAL;

    uint8_t stack_buf[WL_IOCTL_MEDLEN];
    uint8_t *work = stack_buf;
    int heap = 0;
    if (total > sizeof(stack_buf)) {
        work = (uint8_t *)malloc(total);
        if (!work) return -ENOMEM;
        heap = 1;
    }

    size_t packed = 0;
    int rc = wl_iovar_pack((char *)work, total, bsscfg_idx, name,
                           param, param_len, val, val_len, &packed);
    if (rc == 0) {
        rc = bcm_wl_ioctl(ifname, WLC_SET_VAR, work, packed, 1);
    }
    if (heap) free(work);
    return rc;
}

int bcm_wl_iovar_get(const char *ifname, const char *name, void *out, size_t out_len) {
    return wl_iovar_getbuf_ex(ifname, -1, name, NULL, 0, out, out_len);
}

int bcm_wl_iovar_set(const char *ifname, const char *name, const void *val, size_t val_len) {
    return wl_iovar_setbuf_ex(ifname, -1, name, val, val_len, NULL, 0);
}

int bcm_wl_iovar_getbuf(const char *ifname, const char *name,
                        const void *param, size_t param_len,
                        void *out, size_t out_len) {
    return wl_iovar_getbuf_ex(ifname, -1, name, param, param_len, out, out_len);
}

int bcm_wl_iovar_setbuf(const char *ifname, const char *name,
                        const void *param, size_t param_len,
                        const void *val, size_t val_len) {
    return wl_iovar_setbuf_ex(ifname, -1, name, param, param_len, val, val_len);
}

int bcm_wl_iovar_getint(const char *ifname, const char *name, int32_t *out) {
    if (!out) return -EINVAL;
    int32_t v = 0;
    int rc = bcm_wl_iovar_get(ifname, name, &v, sizeof(v));
    if (rc == 0) *out = v;
    return rc;
}

int bcm_wl_iovar_setint(const char *ifname, const char *name, int32_t val) {
    return bcm_wl_iovar_set(ifname, name, &val, sizeof(val));
}

int bcm_wl_bsscfg_iovar_get(const char *ifname, int bsscfg_idx,
                            const char *name, void *out, size_t out_len) {
    return wl_iovar_getbuf_ex(ifname, bsscfg_idx, name, NULL, 0, out, out_len);
}

int bcm_wl_bsscfg_iovar_set(const char *ifname, int bsscfg_idx,
                            const char *name, const void *val, size_t val_len) {
    return wl_iovar_setbuf_ex(ifname, bsscfg_idx, name, val, val_len, NULL, 0);
}

int bcm_wl_bsscfg_iovar_getint(const char *ifname, int bsscfg_idx,
                               const char *name, int32_t *out) {
    if (!out) return -EINVAL;
    int32_t v = 0;
    int rc = bcm_wl_bsscfg_iovar_get(ifname, bsscfg_idx, name, &v, sizeof(v));
    if (rc == 0) *out = v;
    return rc;
}

int bcm_wl_bsscfg_iovar_setint(const char *ifname, int bsscfg_idx,
                               const char *name, int32_t val) {
    return bcm_wl_bsscfg_iovar_set(ifname, bsscfg_idx, name, &val, sizeof(val));
}

/* Context-aware wrappers */
int wl_ctx_ioctl(const wl_ctx_t *ctx, int cmd, void *buf, size_t len, int set) {
    return bcm_wl_ioctl(ctx->ifname, cmd, buf, len, set);
}

int wl_ctx_iovar_get(const wl_ctx_t *ctx, const char *name, void *out, size_t out_len) {
    return wl_iovar_getbuf_ex(ctx->ifname, ctx->bsscfg_idx, name, NULL, 0, out, out_len);
}

int wl_ctx_iovar_set(const wl_ctx_t *ctx, const char *name, const void *val, size_t val_len) {
    return wl_iovar_setbuf_ex(ctx->ifname, ctx->bsscfg_idx, name, val, val_len, NULL, 0);
}

int wl_ctx_iovar_getbuf(const wl_ctx_t *ctx, const char *name,
                        const void *param, size_t param_len,
                        void *out, size_t out_len) {
    return wl_iovar_getbuf_ex(ctx->ifname, ctx->bsscfg_idx, name, param, param_len, out, out_len);
}

int wl_ctx_iovar_setbuf(const wl_ctx_t *ctx, const char *name,
                        const void *param, size_t param_len,
                        const void *val, size_t val_len) {
    return wl_iovar_setbuf_ex(ctx->ifname, ctx->bsscfg_idx, name, param, param_len, val, val_len);
}

int wl_ctx_iovar_getint(const wl_ctx_t *ctx, const char *name, int32_t *out) {
    if (!out) return -EINVAL;
    int32_t v = 0;
    int rc = wl_ctx_iovar_get(ctx, name, &v, sizeof(v));
    if (rc == 0) *out = v;
    return rc;
}

int wl_ctx_iovar_setint(const wl_ctx_t *ctx, const char *name, int32_t val) {
    return wl_ctx_iovar_set(ctx, name, &val, sizeof(val));
}

/* =========================================================================
 * D11REV >= 40 Chanspec encode / decode
 * ========================================================================= */
int wl_chspec_bw_mhz(uint16_t chspec) {
    switch (chspec & WL_CHANSPEC_BW_MASK) {
    case WL_CHANSPEC_BW_5:    return 5;
    case WL_CHANSPEC_BW_10:   return 10;
    case WL_CHANSPEC_BW_20:   return 20;
    case WL_CHANSPEC_BW_40:   return 40;
    case WL_CHANSPEC_BW_80:   return 80;
    case WL_CHANSPEC_BW_160:  return 160;
    case WL_CHANSPEC_BW_8080: return 160;
    default:                  return 0;
    }
}

const char *wl_chspec_band_str(uint16_t chspec) {
    switch (chspec & WL_CHANSPEC_BAND_MASK) {
    case WL_CHANSPEC_BAND_2G: return "2.4GHz";
    case WL_CHANSPEC_BAND_5G: return "5GHz";
    case WL_CHANSPEC_BAND_6G: return "6GHz";
    default:                  return "Unknown";
    }
}

int wl_chspec_primary_chan(uint16_t chspec) {
    int center = (int)(chspec & WL_CHANSPEC_CHAN_MASK);
    int sb = (int)((chspec & WL_CHANSPEC_CTL_SB_MASK) >> WL_CHANSPEC_CTL_SB_SHIFT);
    switch (chspec & WL_CHANSPEC_BW_MASK) {
    case WL_CHANSPEC_BW_5:
    case WL_CHANSPEC_BW_10:
    case WL_CHANSPEC_BW_20:
        return center;
    case WL_CHANSPEC_BW_40:
        return center - 2 + (sb & 1) * 4;
    case WL_CHANSPEC_BW_80:
        return center - 6 + (sb & 3) * 4;
    case WL_CHANSPEC_BW_160:
        return center - 14 + (sb & 7) * 4;
    default:
        return center;
    }
}

int bcm_wl_chspec_to_str(uint16_t chspec, char *out, size_t out_sz) {
    if (!out || out_sz < 8) return -EINVAL;
    uint16_t band = chspec & WL_CHANSPEC_BAND_MASK;
    uint16_t bw   = chspec & WL_CHANSPEC_BW_MASK;
    int sb        = (int)((chspec & WL_CHANSPEC_CTL_SB_MASK) >> WL_CHANSPEC_CTL_SB_SHIFT);
    int prim      = wl_chspec_primary_chan(chspec);

    const char *bprefix = "";
    if (band == WL_CHANSPEC_BAND_6G) {
        bprefix = "6g";
    } else if (band == WL_CHANSPEC_BAND_2G && prim > 14) {
        bprefix = "2g";
    } else if (band == WL_CHANSPEC_BAND_5G && prim <= 14) {
        bprefix = "5g";
    }

    int n = 0;
    switch (bw) {
    case WL_CHANSPEC_BW_5:
        n = snprintf(out, out_sz, "%s%d/5", bprefix, prim);
        break;
    case WL_CHANSPEC_BW_10:
        n = snprintf(out, out_sz, "%s%d/10", bprefix, prim);
        break;
    case WL_CHANSPEC_BW_20:
        n = snprintf(out, out_sz, "%s%d", bprefix, prim);
        break;
    case WL_CHANSPEC_BW_40:
        n = snprintf(out, out_sz, "%s%d%c", bprefix, prim, (sb & 1) ? 'u' : 'l');
        break;
    case WL_CHANSPEC_BW_80:
        n = snprintf(out, out_sz, "%s%d/80", bprefix, prim);
        break;
    case WL_CHANSPEC_BW_160:
        n = snprintf(out, out_sz, "%s%d/160", bprefix, prim);
        break;
    default:
        n = snprintf(out, out_sz, "0x%04x", (unsigned)chspec);
        break;
    }
    return (n < 0 || (size_t)n >= out_sz) ? -ENOBUFS : 0;
}

static int find_center_5g(int prim, int bw, int *out_center, int *out_sb) {
    static const int c40[]  = { 38, 46, 54, 62, 102, 110, 118, 126, 134, 142, 151, 159, 167, 175 };
    static const int c80[]  = { 42, 58, 106, 122, 138, 155, 171 };
    static const int c160[] = { 50, 114, 163 };
    const int *tbl = NULL;
    size_t cnt = 0;
    int half = 0;

    if (bw == 40)       { tbl = c40;  cnt = sizeof(c40) / sizeof(c40[0]);   half = 2;  }
    else if (bw == 80)  { tbl = c80;  cnt = sizeof(c80) / sizeof(c80[0]);   half = 6;  }
    else if (bw == 160) { tbl = c160; cnt = sizeof(c160) / sizeof(c160[0]); half = 14; }
    else return -EINVAL;

    for (size_t i = 0; i < cnt; i++) {
        int low = tbl[i] - half;
        int high = tbl[i] + half;
        if (prim >= low && prim <= high && ((prim - low) % 4) == 0) {
            *out_center = tbl[i];
            *out_sb = (prim - low) / 4;
            return 0;
        }
    }
    return -EINVAL;
}

int bcm_wl_chspec_from_str(const char *str, uint16_t *out) {
    if (!str || !str[0] || !out) return -EINVAL;

    /* Raw hex: 0x.... */
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        char *end = NULL;
        unsigned long v = strtoul(str, &end, 16);
        if (!end || *end != '\0' || v > 0xffffUL) return -EINVAL;
        *out = (uint16_t)v;
        return 0;
    }

    const char *p = str;
    int explicit_band = -1;
    if ((p[0] == '2' || p[0] == '5' || p[0] == '6') && (p[1] == 'g' || p[1] == 'G')) {
        if (p[0] == '2') explicit_band = WL_CHANSPEC_BAND_2G;
        else if (p[0] == '5') explicit_band = WL_CHANSPEC_BAND_5G;
        else explicit_band = WL_CHANSPEC_BAND_6G;
        p += 2;
    }

    if (!isdigit((unsigned char)*p)) return -EINVAL;
    char *end = NULL;
    long chan = strtol(p, &end, 10);
    if (end == p || chan < 1 || chan > 233) return -EINVAL;

    uint16_t band = (explicit_band >= 0) ? (uint16_t)explicit_band
                    : ((chan <= 14) ? WL_CHANSPEC_BAND_2G : WL_CHANSPEC_BAND_5G);

    uint16_t bw_bits = WL_CHANSPEC_BW_20;
    int center = (int)chan;
    int sb = 0;

    if (*end == '\0') {
        bw_bits = WL_CHANSPEC_BW_20;
    } else if ((*end == 'l' || *end == 'L' || *end == 'u' || *end == 'U') && end[1] == '\0') {
        int is_upper = (*end == 'u' || *end == 'U');
        bw_bits = WL_CHANSPEC_BW_40;
        if (band == WL_CHANSPEC_BAND_2G) {
            if (is_upper) {
                if (chan < 5 || chan > 13) return -EINVAL;
                center = (int)chan - 2;
                sb = 1;
            } else {
                if (chan < 1 || chan > 9) return -EINVAL;
                center = (int)chan + 2;
                sb = 0;
            }
        } else {
            int c = 0, s = 0;
            if (find_center_5g((int)chan, 40, &c, &s) < 0 || s != is_upper) {
                return -EINVAL;
            }
            center = c;
            sb = s;
        }
    } else if (*end == '/') {
        char *bwend = NULL;
        long bw = strtol(end + 1, &bwend, 10);
        if (!bwend || *bwend != '\0') return -EINVAL;
        if (bw == 20) {
            bw_bits = WL_CHANSPEC_BW_20;
        } else if (bw == 40) {
            bw_bits = WL_CHANSPEC_BW_40;
            if (band == WL_CHANSPEC_BAND_2G) {
                if (chan < 1 || chan > 13) return -EINVAL;
                if (chan <= 7) { center = (int)chan + 2; sb = 0; }
                else           { center = (int)chan - 2; sb = 1; }
            } else {
                if (find_center_5g((int)chan, 40, &center, &sb) < 0) return -EINVAL;
            }
        } else if (bw == 80) {
            bw_bits = WL_CHANSPEC_BW_80;
            if (find_center_5g((int)chan, 80, &center, &sb) < 0) return -EINVAL;
        } else if (bw == 160) {
            bw_bits = WL_CHANSPEC_BW_160;
            if (find_center_5g((int)chan, 160, &center, &sb) < 0) return -EINVAL;
        } else if (bw == 10) {
            bw_bits = WL_CHANSPEC_BW_10;
        } else if (bw == 5) {
            bw_bits = WL_CHANSPEC_BW_5;
        } else {
            return -EINVAL;
        }
    } else {
        return -EINVAL;
    }

    *out = (uint16_t)(band | bw_bits | ((uint16_t)(sb & 7) << WL_CHANSPEC_CTL_SB_SHIFT) | (uint16_t)(center & 0xff));
    return 0;
}

/* =========================================================================
 * MAC & integer parsing / formatting helpers
 * ========================================================================= */
int bcm_wl_parse_mac(const char *str, uint8_t mac[6]) {
    if (!str || !mac || strlen(str) != 17) return -EINVAL;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)str[i * 3]) || !isxdigit((unsigned char)str[i * 3 + 1])) {
            return -EINVAL;
        }
        if (i < 5 && str[i * 3 + 2] != ':') return -EINVAL;
    }
    unsigned int b[6];
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return -EINVAL;
    }
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    return 0;
}

void bcm_wl_format_mac(const uint8_t mac[6], char out[18], int lower) {
    snprintf(out, 18, lower ? "%02x:%02x:%02x:%02x:%02x:%02x"
                            : "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int wl_parse_i32(const char *s, int32_t *out) {
    if (!s || !s[0] || !out) return -EINVAL;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 0);
    if (errno != 0 || !end || *end != '\0') return -EINVAL;
    if (v < INT32_MIN || v > UINT32_MAX) return -ERANGE;
    *out = (int32_t)v;
    return 0;
}

int wl_parse_u32(const char *s, uint32_t *out) {
    if (!s || !s[0] || !out) return -EINVAL;
    if (s[0] == '-' || s[0] == '+' || isspace((unsigned char)s[0])) return -EINVAL;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || !end || *end != '\0' || v > UINT32_MAX) return -EINVAL;
    *out = (uint32_t)v;
    return 0;
}

int wl_parse_onoff_i32(const char *s, int32_t *out) {
    if (!s || !out) return -EINVAL;
    if (strcasecmp(s, "on") == 0)  { *out = 1; return 0; }
    if (strcasecmp(s, "off") == 0) { *out = 0; return 0; }
    return wl_parse_i32(s, out);
}

/* Matches stock wl_varint (0x423f8): > 9 prints "%d (0x%x)\n", else "%d\n" */
void wl_print_varint(int32_t val) {
    if (val > 9) {
        printf("%d (0x%x)\n", val, (unsigned int)val);
    } else {
        printf("%d\n", val);
    }
}

void wl_format_ssid_escaped(const uint8_t *ssid, uint32_t len, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (len > 32) len = 32;
    size_t pos = 0;
    for (uint32_t i = 0; i < len && pos + 5 < out_sz; i++) {
        uint8_t c = ssid[i];
        if (c == '\\' || c == '"') {
            out[pos++] = '\\';
            out[pos++] = (char)c;
        } else if (c >= 0x20 && c < 0x7f) {
            out[pos++] = (char)c;
        } else {
            pos += (size_t)snprintf(out + pos, out_sz - pos, "\\x%02x", c);
        }
    }
    out[pos] = '\0';
}

void wl_print_err(const wl_ctx_t *ctx, int rc) {
    const char *prog = (ctx && ctx->prog) ? ctx->prog : "wl";
    int err = (rc < 0) ? -rc : rc;
    switch (err) {
    case EOPNOTSUPP: fprintf(stderr, "%s: Unsupported\n", prog); break;
    case ENODEV:     fprintf(stderr, "%s: wl driver adapter not found\n", prog); break;
    case EINVAL:     fprintf(stderr, "%s: Bad Argument\n", prog); break;
    case EBUSY:      fprintf(stderr, "%s: Busy\n", prog); break;
    case EPERM:
    case EACCES:     fprintf(stderr, "%s: Permission denied\n", prog); break;
    default: {
        char ebuf[128] = "Unknown error";
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
        const char *msg = strerror_r(err, ebuf, sizeof(ebuf));
        fprintf(stderr, "%s: %s\n", prog, msg ? msg : ebuf);
#else
        if (strerror_r(err, ebuf, sizeof(ebuf)) != 0) {
            snprintf(ebuf, sizeof(ebuf), "Error %d", err);
        }
        fprintf(stderr, "%s: %s\n", prog, ebuf);
#endif
        break;
    }
    }
}

/* =========================================================================
 * All 10 Generic Handlers for WL_GEN_TABLE[] (wl_table.h)
 * ========================================================================= */
static int run_generic_entry(const wl_ctx_t *ctx, const wl_gen_entry_t *e, int argc, char **argv) {
    int rc = 0;
    switch (e->kind) {
    case WL_K_VARINT: {
        if (argc == 1) {
            if (e->get_cmd < 0) {
                fprintf(stderr, "%s: %s is set-only\n", ctx->prog, e->name);
                return -EINVAL;
            }
            int32_t val = 0;
            rc = wl_ctx_iovar_getint(ctx, e->name, &val);
            if (rc == 0) wl_print_varint(val);
            return rc;
        }
        if (e->set_cmd < 0) {
            fprintf(stderr, "%s: %s is read-only\n", ctx->prog, e->name);
            return -EINVAL;
        }
        int32_t val = 0;
        if (wl_parse_onoff_i32(argv[1], &val) < 0) return -EINVAL;
        return wl_ctx_iovar_setint(ctx, e->name, val);
    }

    case WL_K_INT: {
        if (argc == 1) {
            if (e->get_cmd < 0) return -EINVAL;
            int32_t val = 0;
            rc = wl_ctx_ioctl(ctx, e->get_cmd, &val, sizeof(val), 0);
            if (rc == 0) printf("%d\n", val);
            return rc;
        }
        if (e->set_cmd < 0) return -EINVAL;
        int32_t val = 0;
        if (wl_parse_onoff_i32(argv[1], &val) < 0) return -EINVAL;
        return wl_ctx_ioctl(ctx, e->set_cmd, &val, sizeof(val), 1);
    }

    case WL_K_BSSCFG_INT: {
        /* Matches wl_bsscfg_int (0x2ef14): optional "-C <idx>" prefix */
        int bss = ctx->bsscfg_idx;
        int argi = 1;
        if (argi + 1 < argc && strcmp(argv[argi], "-C") == 0) {
            int32_t idx = 0;
            if (wl_parse_i32(argv[argi + 1], &idx) < 0 || idx < 0) return -EINVAL;
            bss = idx;
            argi += 2;
        }
        if (argi >= argc) {
            if (e->get_cmd < 0) return -EINVAL;
            int32_t val = 0;
            if (bss >= 0 || e->get_cmd == WLC_GET_VAR) {
                rc = bcm_wl_bsscfg_iovar_getint(ctx->ifname, bss, e->name, &val);
            } else {
                rc = wl_ctx_ioctl(ctx, e->get_cmd, &val, sizeof(val), 0);
            }
            if (rc == 0) printf("%d\n", val);
            return rc;
        }
        if (e->set_cmd < 0) return -EINVAL;
        int32_t val = 0;
        if (wl_parse_onoff_i32(argv[argi], &val) < 0) return -EINVAL;
        if (bss >= 0 || e->set_cmd == WLC_SET_VAR) {
            return bcm_wl_bsscfg_iovar_setint(ctx->ifname, bss, e->name, val);
        }
        return wl_ctx_ioctl(ctx, e->set_cmd, &val, sizeof(val), 1);
    }

    case WL_K_VAR_VOID:
        if (e->set_cmd < 0) return -EINVAL;
        return wl_ctx_iovar_set(ctx, e->name, NULL, 0);

    case WL_K_VOID:
        if (e->set_cmd < 0) return -EINVAL;
        return wl_ctx_ioctl(ctx, e->set_cmd, NULL, 0, 1);

    case WL_K_ETHERADDR: {
        if (argc == 1) {
            uint8_t mac[6] = {0};
            rc = wl_ctx_iovar_get(ctx, e->name, mac, sizeof(mac));
            if (rc == 0) {
                char s[18];
                bcm_wl_format_mac(mac, s, 0);
                printf("%s %s\n", e->name, s);
            }
            return rc;
        }
        uint8_t mac[6];
        if (bcm_wl_parse_mac(argv[1], mac) < 0) return -EINVAL;
        return wl_ctx_iovar_set(ctx, e->name, mac, sizeof(mac));
    }

    case WL_K_MAC_IOCTL: {
        if (argc < 2 || e->set_cmd < 0) return -EINVAL;
        uint8_t mac[6];
        if (bcm_wl_parse_mac(argv[1], mac) < 0) return -EINVAL;
        return wl_ctx_ioctl(ctx, e->set_cmd, mac, sizeof(mac), 1);
    }

    case WL_K_VAR_GETSTR: {
        char buf[WL_IOCTL_MAXLEN];
        memset(buf, 0, sizeof(buf));
        rc = wl_ctx_iovar_get(ctx, e->name, buf, sizeof(buf) - 1);
        if (rc == 0) {
            buf[sizeof(buf) - 1] = '\0';
            puts(buf);
        }
        return rc;
    }

    case WL_K_VAR_STR: {
        if (argc == 1) {
            char buf[WL_IOCTL_MEDLEN];
            memset(buf, 0, sizeof(buf));
            rc = wl_ctx_iovar_get(ctx, e->name, buf, sizeof(buf) - 1);
            if (rc == 0) {
                buf[sizeof(buf) - 1] = '\0';
                puts(buf);
            }
            return rc;
        }
        if (e->set_cmd < 0) return -EINVAL;
        return wl_ctx_iovar_set(ctx, e->name, argv[1], strlen(argv[1]) + 1);
    }

    case WL_K_CHANSPECS: {
        /* Matches stock wl_dump_chanspecs (0x36de4):
         * Input param (10B packed, no pad): { u16 hint; char ccode[4]; u32 count; }
         * Output: { u32 count; u32 chanspec[count]; }
         */
        uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
        if (!buf) return -ENOMEM;

        uint16_t hint = 0;
        char ccode[4] = {0};
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-b") == 0) {
                if (i + 1 >= argc) { free(buf); return -EINVAL; }
                int32_t b = 0;
                if (wl_parse_i32(argv[++i], &b) < 0) { free(buf); return -EINVAL; }
                hint &= (uint16_t)~WL_CHANSPEC_BAND_MASK;
                if (b == 5) hint |= WL_CHANSPEC_BAND_5G;
                else if (b == 6) hint |= WL_CHANSPEC_BAND_6G;
                else if (b == 2) hint |= WL_CHANSPEC_BAND_2G;
                else { free(buf); return -EINVAL; }
            } else if (strcmp(argv[i], "-w") == 0) {
                if (i + 1 >= argc) { free(buf); return -EINVAL; }
                int32_t w = 0;
                if (wl_parse_i32(argv[++i], &w) < 0) { free(buf); return -EINVAL; }
                hint &= (uint16_t)~WL_CHANSPEC_BW_MASK;
                if (w == 20) hint |= WL_CHANSPEC_BW_20;
                else if (w == 40) hint |= WL_CHANSPEC_BW_40;
                else if (w == 80) hint |= WL_CHANSPEC_BW_80;
                else if (w == 160) hint |= WL_CHANSPEC_BW_160;
                else { free(buf); return -EINVAL; }
            } else if (strcmp(argv[i], "-c") == 0) {
                if (i + 1 >= argc || strlen(argv[i + 1]) > 3) { free(buf); return -EINVAL; }
                strncpy(ccode, argv[++i], 3);
            } else {
                free(buf);
                return -EINVAL;
            }
        }
        uint8_t req[10];
        memset(req, 0, sizeof(req));
        memcpy(req + 0, &hint, 2);
        memcpy(req + 2, ccode, 4);
        /* Stock 0x37040: start at max_cnt=110 and retry +110 only on BCME_BUFTOOSHORT (-14) */
        rc = -EINVAL;
        for (uint32_t max_cnt = 110; 4u + max_cnt * 4u <= WL_IOCTL_LARGE_LEN; max_cnt += 110) {
            memcpy(req + 6, &max_cnt, 4);
            rc = wl_ctx_iovar_getbuf(ctx, e->name, req, sizeof(req), buf, WL_IOCTL_LARGE_LEN);
            if (rc == 0) break;
            int32_t bcmerr = 0;
            if (wl_ctx_iovar_getint(ctx, "bcmerror", &bcmerr) != 0 || bcmerr != -14) break;
        }
        if (rc == 0) {
            uint32_t count = 0;
            memcpy(&count, buf, 4);
            if (count > (WL_IOCTL_LARGE_LEN - 4u) / 4u) {
                count = (WL_IOCTL_LARGE_LEN - 4u) / 4u;
            }
            for (uint32_t i = 0; i < count; i++) {
                uint32_t cs32 = 0;
                memcpy(&cs32, buf + 4u + i * 4u, 4);
                char s[32];
                bcm_wl_chspec_to_str((uint16_t)(cs32 & 0xffffu), s, sizeof(s));
                printf("%s (0x%04x)\n", s, (unsigned)(cs32 & 0xffffu));
            }
            putchar('\n');
        }
        free(buf);
        return rc;
    }
    }
    return -EINVAL;
}

/* =========================================================================
 * Built-in Exploration Commands: dump, iovar, ioctl, cmds, help
 * ========================================================================= */
static int cmd_dump(const wl_ctx_t *ctx, int argc, char **argv) {
    char *buf = (char *)calloc(1, WL_DUMP_BUF_LEN);
    if (!buf) return -ENOMEM;

    int rc;
    if (argc <= 1) {
        /* Plain `wl dump` */
        rc = wl_ctx_iovar_get(ctx, "dump", buf, WL_DUMP_BUF_LEN - 1);
    } else {
        /* Matches stock wl_dump (0x23024): join args with ' ' + trailing ' ' */
        char param[512];
        size_t off = 0;
        for (int i = 1; i < argc && off + 2 < sizeof(param); i++) {
            size_t alen = strlen(argv[i]);
            if (off + alen + 2 >= sizeof(param)) break;
            memcpy(param + off, argv[i], alen);
            off += alen;
            param[off++] = ' ';
        }
        param[off] = '\0';
        rc = wl_ctx_iovar_getbuf(ctx, "dump", param, off + 1, buf, WL_DUMP_BUF_LEN - 1);
    }

    if (rc == 0) {
        buf[WL_DUMP_BUF_LEN - 1] = '\0';
        fputs(buf, stdout);
        size_t blen = strlen(buf);
        if (blen > 0 && buf[blen - 1] != '\n') putchar('\n');
    }
    free(buf);
    return rc;
}

static int hex_decode(const char *hex, uint8_t *out, size_t max_len, size_t *out_len) {
    size_t n = 0;
    const char *p = hex;
    while (*p) {
        while (*p == ' ' || *p == ':' || *p == '-') p++;
        if (!*p) break;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return -EINVAL;
        if (n >= max_len) return -ENOBUFS;
        char pair[3] = { p[0], p[1], '\0' };
        out[n++] = (uint8_t)strtoul(pair, NULL, 16);
        p += 2;
    }
    *out_len = n;
    return 0;
}

static void hexdump(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i += 16) {
        printf("%04zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) printf("%02x ", buf[i + j]);
            else printf("   ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = buf[i + j];
            putchar((c >= 32 && c < 127) ? (char)c : '.');
        }
        printf("|\n");
    }
}

static int cmd_raw_iovar(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: wl iovar <name> [get [len] | getint | getstr | setint <n> | setstr <s> | sethex <hex>]\n");
        return -EINVAL;
    }
    const char *name = argv[1];
    const char *mode = (argc >= 3) ? argv[2] : "getint";

    if (strcmp(mode, "getint") == 0) {
        int32_t v = 0;
        int rc = wl_ctx_iovar_getint(ctx, name, &v);
        if (rc == 0) wl_print_varint(v);
        return rc;
    }
    if (strcmp(mode, "setint") == 0 && argc >= 4) {
        int32_t v = 0;
        if (wl_parse_onoff_i32(argv[3], &v) < 0) return -EINVAL;
        return wl_ctx_iovar_setint(ctx, name, v);
    }
    if (strcmp(mode, "getstr") == 0) {
        char buf[WL_IOCTL_MAXLEN];
        memset(buf, 0, sizeof(buf));
        int rc = wl_ctx_iovar_get(ctx, name, buf, sizeof(buf) - 1);
        if (rc == 0) puts(buf);
        return rc;
    }
    if (strcmp(mode, "setstr") == 0 && argc >= 4) {
        return wl_ctx_iovar_set(ctx, name, argv[3], strlen(argv[3]) + 1);
    }
    if (strcmp(mode, "get") == 0) {
        size_t len = (argc >= 4) ? (size_t)strtoul(argv[3], NULL, 0) : 64;
        if (len == 0 || len > WL_IOCTL_MAXLEN) len = 64;
        uint8_t buf[WL_IOCTL_MAXLEN];
        memset(buf, 0, len);
        int rc = wl_ctx_iovar_get(ctx, name, buf, len);
        if (rc == 0) hexdump(buf, len);
        return rc;
    }
    if (strcmp(mode, "sethex") == 0 && argc >= 4) {
        uint8_t raw[1024];
        size_t rlen = 0;
        if (hex_decode(argv[3], raw, sizeof(raw), &rlen) < 0) return -EINVAL;
        return wl_ctx_iovar_set(ctx, name, raw, rlen);
    }
    fprintf(stderr, "wl iovar: unknown mode '%s'\n", mode);
    return -EINVAL;
}

static int cmd_raw_ioctl(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: wl ioctl <num> [get [len] | getint | setint <n> | sethex <hex>]\n");
        return -EINVAL;
    }
    int32_t num = 0;
    if (wl_parse_i32(argv[1], &num) < 0) return -EINVAL;
    const char *mode = (argc >= 3) ? argv[2] : "getint";

    if (strcmp(mode, "getint") == 0) {
        int32_t v = 0;
        int rc = wl_ctx_ioctl(ctx, num, &v, sizeof(v), 0);
        if (rc == 0) wl_print_varint(v);
        return rc;
    }
    if (strcmp(mode, "setint") == 0 && argc >= 4) {
        int32_t v = 0;
        if (wl_parse_onoff_i32(argv[3], &v) < 0) return -EINVAL;
        return wl_ctx_ioctl(ctx, num, &v, sizeof(v), 1);
    }
    if (strcmp(mode, "get") == 0) {
        size_t len = (argc >= 4) ? (size_t)strtoul(argv[3], NULL, 0) : 64;
        if (len == 0 || len > WL_IOCTL_MAXLEN) len = 64;
        uint8_t buf[WL_IOCTL_MAXLEN];
        memset(buf, 0, len);
        int rc = wl_ctx_ioctl(ctx, num, buf, len, 0);
        if (rc == 0) hexdump(buf, len);
        return rc;
    }
    if (strcmp(mode, "sethex") == 0 && argc >= 4) {
        uint8_t raw[1024];
        size_t rlen = 0;
        if (hex_decode(argv[3], raw, sizeof(raw), &rlen) < 0) return -EINVAL;
        return wl_ctx_ioctl(ctx, num, raw, rlen, 1);
    }
    return -EINVAL;
}

static int cmd_help(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)ctx;
    if (argc >= 2) {
        const char *q = argv[1];
        for (size_t i = 0; i < WL_T1_CMDS_COUNT; i++) {
            if (strcmp(WL_T1_CMDS[i].name, q) == 0) {
                printf("%s [%s/%s]: %s\n", WL_T1_CMDS[i].name, WL_T1_CMDS[i].tier,
                       WL_T1_CMDS[i].group, WL_T1_CMDS[i].help);
                return 0;
            }
        }
        for (size_t i = 0; i < WL_T2_CMDS_COUNT; i++) {
            if (strcmp(WL_T2_CMDS[i].name, q) == 0) {
                printf("%s [%s/%s]: %s\n", WL_T2_CMDS[i].name, WL_T2_CMDS[i].tier,
                       WL_T2_CMDS[i].group, WL_T2_CMDS[i].help);
                return 0;
            }
        }
        size_t gen_n = sizeof(WL_GEN_TABLE) / sizeof(WL_GEN_TABLE[0]);
        for (size_t i = 0; i < gen_n; i++) {
            if (strcmp(WL_GEN_TABLE[i].name, q) == 0) {
                printf("%s [%s]: %s\n", WL_GEN_TABLE[i].name, WL_GEN_TABLE[i].tier,
                       WL_GEN_TABLE[i].help);
                return 0;
            }
        }
        printf("%s: generic integer IOVAR (WLC_GET_VAR/WLC_SET_VAR fallback)\n", q);
        return 0;
    }

    printf("Usage: wl [-i <ifname>] [-b <bsscfg>] <command> [args...]\n\n");
    printf("Built-in exploration commands:\n");
    printf("  dump [subsys...]   Universal kernel subsystem text dump (csimon, scb, wlc, ...)\n");
    printf("  iovar <name> ...   Raw IOVAR get/set/getint/setint/getstr/sethex explorer\n");
    printf("  ioctl <num> ...    Raw IOCTL get/set/getint/setint/sethex explorer\n");
    printf("  cmds / help [cmd]  List all built-in commands or show command help\n");
    printf("  <any_iovar> [val]  Generic int32 IOVAR fallback (e.g. phy_tempsense, atf, txbf)\n\n");

    printf("T1 Core AP Commands (%zu):\n  ", WL_T1_CMDS_COUNT);
    for (size_t i = 0; i < WL_T1_CMDS_COUNT; i++) {
        printf("%s%s", WL_T1_CMDS[i].name, ((i + 1) % 6 == 0) ? "\n  " : " ");
    }
    printf("\n\nT2 Extended AP Commands (%zu):\n  ", WL_T2_CMDS_COUNT);
    for (size_t i = 0; i < WL_T2_CMDS_COUNT; i++) {
        printf("%s%s", WL_T2_CMDS[i].name, ((i + 1) % 6 == 0) ? "\n  " : " ");
    }
    size_t gen_n = sizeof(WL_GEN_TABLE) / sizeof(WL_GEN_TABLE[0]);
    printf("\n\nTable-driven Generic Commands (%zu):\n  ", gen_n);
    for (size_t i = 0; i < gen_n; i++) {
        printf("%s%s", WL_GEN_TABLE[i].name, ((i + 1) % 6 == 0) ? "\n  " : " ");
    }
    printf("\n");
    return 0;
}

/* =========================================================================
 * Main CLI Entry Point: bcm_cli_wl
 * ========================================================================= */
int bcm_cli_wl(int argc, char **argv) {
    wl_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.bsscfg_idx = -1;
    ctx.prog = (argc > 0 && argv[0]) ? argv[0] : "wl";

    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "-a") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: option '%s' requires an interface name\n", ctx.prog, argv[i]);
                return 1;
            }
            snprintf(ctx.ifname, sizeof(ctx.ifname), "%s", argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "-c") == 0 ||
                   strcmp(argv[i], "-C") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: option '%s' requires a bsscfg index\n", ctx.prog, argv[i]);
                return 1;
            }
            int32_t b = 0;
            if (wl_parse_i32(argv[i + 1], &b) < 0 || b < 0) {
                fprintf(stderr, "%s: invalid bsscfg index '%s'\n", ctx.prog, argv[i + 1]);
                return 1;
            }
            ctx.bsscfg_idx = b;
            i += 2;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            return cmd_help(&ctx, 1, argv + i);
        } else {
            break;
        }
    }

    if (i >= argc) {
        return cmd_help(&ctx, 1, argv);
    }

    int cmd_argc = argc - i;
    char **cmd_argv = argv + i;
    const char *cmd = cmd_argv[0];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "cmds") == 0) {
        return cmd_help(&ctx, cmd_argc, cmd_argv);
    }

    /* Resolve interface if -i/-a was not passed */
    if (!ctx.ifname[0]) {
        int rc = bcm_wl_find_iface(ctx.ifname, sizeof(ctx.ifname));
        if (rc < 0) {
            wl_print_err(&ctx, rc);
            return 1;
        }
    }

    int rc = 0;

    /* 1. T1 custom handlers */
    for (size_t k = 0; k < WL_T1_CMDS_COUNT; k++) {
        if (strcmp(WL_T1_CMDS[k].name, cmd) == 0) {
            rc = WL_T1_CMDS[k].fn(&ctx, cmd_argc, cmd_argv);
            goto done;
        }
    }

    /* 2. T2 custom handlers */
    for (size_t k = 0; k < WL_T2_CMDS_COUNT; k++) {
        if (strcmp(WL_T2_CMDS[k].name, cmd) == 0) {
            rc = WL_T2_CMDS[k].fn(&ctx, cmd_argc, cmd_argv);
            goto done;
        }
    }

    /* 3. Built-in dump / iovar / ioctl */
    if (strcmp(cmd, "dump") == 0) {
        rc = cmd_dump(&ctx, cmd_argc, cmd_argv);
        goto done;
    }
    if (strcmp(cmd, "iovar") == 0) {
        rc = cmd_raw_iovar(&ctx, cmd_argc, cmd_argv);
        goto done;
    }
    if (strcmp(cmd, "ioctl") == 0) {
        rc = cmd_raw_ioctl(&ctx, cmd_argc, cmd_argv);
        goto done;
    }

    /* 4. Auto-extracted WL_GEN_TABLE[] (10 generic kinds across T1/T2/T3) */
    size_t gen_n = sizeof(WL_GEN_TABLE) / sizeof(WL_GEN_TABLE[0]);
    for (size_t k = 0; k < gen_n; k++) {
        if (strcmp(WL_GEN_TABLE[k].name, cmd) == 0) {
            rc = run_generic_entry(&ctx, &WL_GEN_TABLE[k], cmd_argc, cmd_argv);
            goto done;
        }
    }

    /* 5. Generic integer IOVAR fallback (matches stock wl_varint fallback) */
    if (cmd_argc == 1) {
        int32_t val = 0;
        rc = wl_ctx_iovar_getint(&ctx, cmd, &val);
        if (rc == 0) wl_print_varint(val);
    } else if (cmd_argc == 2) {
        int32_t val = 0;
        if (wl_parse_onoff_i32(cmd_argv[1], &val) < 0) {
            rc = -EINVAL;
        } else {
            rc = wl_ctx_iovar_setint(&ctx, cmd, val);
        }
    } else {
        fprintf(stderr, "%s: unknown command '%s' (try 'wl help' or 'wl iovar')\n", ctx.prog, cmd);
        return 1;
    }

done:
    if (rc < 0) {
        wl_print_err(&ctx, rc);
        return 1;
    }
    return 0;
}
