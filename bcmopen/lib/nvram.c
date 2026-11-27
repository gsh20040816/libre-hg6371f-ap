/*
 * nvram.c — Open-source replacement for Broadcom /bin/nvram + libnvram.so
 *
 * Verified against /opt/bcm/libexec/nvram and /opt/bcm/lib/libnvram.so:
 *   - Kernel NVRAM: AF_NETLINK (16), SOCK_RAW (3), proto = 31
 *     Header (0x18 bytes):
 *       +0x00 u32 nlmsg_len   = (plen + 0x18 + 3) & ~3
 *       +0x04 u16 nlmsg_type  = 5
 *       +0x06 u16 nlmsg_flags = 0
 *       +0x08 u32 nlmsg_seq   = 1
 *       +0x0c u32 nlmsg_pid   = bound pid
 *       +0x10 u16 op          (2=SET, 3=GET, 5=GETALL, 6=GETALL_DONE)
 *       +0x12 u16 plen        (payload bytes)
 *       +0x14 u32 pad         = 0
 *       +0x18 payload[]
 *     SET payload (get_namevalue_buf):
 *       +0x00 u32 name_len (= strlen(name)+1)
 *       +0x04 name\0 + pad to 4B boundary
 *       +off  u32 val_len  (= val ? strlen(val)+1 : 0)
 *       +off+4 val\0 (if val)
 *   - User NVRAM: /data/.user_nvram.setting (name=value lines, atomic rename + flock)
 */
#include "bcmopen.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/netlink.h>
#endif

#define NVRAM_NL_PROTO      31
#define NVRAM_NL_TYPE       5
#define NVRAM_OP_SET        2
#define NVRAM_OP_GET        3
#define NVRAM_OP_GETALL     5
#define NVRAM_OP_GETALL_END 6

#define USER_NVRAM_FILE     "/data/.user_nvram.setting"
#define USER_NVRAM_LOCK     "/tmp/.user_nvram.lock"
#define KERN_NVRAM_FILE     "/data/.kernel_nvram.setting"
#define DEF_NVRAM_FILE      "/data/nvramdefault.txt"
#define MFG_NVRAM_FILE      "/var/nvram/nvram.nvm"

#ifdef __linux__
static size_t align4(size_t n) {
    return (n + 3u) & ~3u;
}

typedef struct {
    int      sock;
    uint32_t pid;
} nv_nl_t;

static nv_nl_t g_nl = { -1, 0 };
static pthread_mutex_t g_nl_mu = PTHREAD_MUTEX_INITIALIZER;

static int nv_nl_init(void) {
    if (g_nl.sock >= 0) return 0;
    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NVRAM_NL_PROTO);
    if (s < 0) return -1;

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = (uint32_t)getpid();
    sa.nl_groups = 0;

    int tries = 0;
    while (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (++tries > 10) {
            close(s);
            return -1;
        }
        sa.nl_pid++;
    }
    g_nl.sock = s;
    g_nl.pid = sa.nl_pid;
    return 0;
}

static int nv_nl_send(uint16_t op, const void *data, size_t len) {
    if (len > 0xffffu) return -1;
    if (nv_nl_init() != 0) return -1;
    size_t sz = align4(len + 0x18u);
    uint8_t *buf = calloc(1, sz);
    if (!buf) return -1;

    uint32_t u32;
    uint16_t u16;
    u32 = (uint32_t)sz;      memcpy(buf + 0x00, &u32, 4);
    u16 = NVRAM_NL_TYPE;     memcpy(buf + 0x04, &u16, 2);
    u16 = 0;                 memcpy(buf + 0x06, &u16, 2);
    u32 = 1;                 memcpy(buf + 0x08, &u32, 4);
    u32 = g_nl.pid;          memcpy(buf + 0x0c, &u32, 4);
    u16 = op;                memcpy(buf + 0x10, &u16, 2);
    u16 = (uint16_t)len;     memcpy(buf + 0x12, &u16, 2);
    u32 = 0;                 memcpy(buf + 0x14, &u32, 4);
    if (len > 0 && data) memcpy(buf + 0x18, data, len);

    struct sockaddr_nl dst;
    memset(&dst, 0, sizeof(dst));
    dst.nl_family = AF_NETLINK;
    struct iovec iov = { buf, sz };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &dst;
    msg.msg_namelen = sizeof(dst);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ssize_t r = sendmsg(g_nl.sock, &msg, 0);
    free(buf);
    return (r < 0) ? -1 : 0;
}

static int nv_nl_recv(uint8_t *out, size_t out_cap) {
    if (g_nl.sock < 0) return -1;
    uint8_t ibuf[0x510];
    memset(ibuf, 0, sizeof(ibuf));

    struct sockaddr_nl src;
    memset(&src, 0, sizeof(src));
    struct iovec iov = { ibuf, sizeof(ibuf) };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &src;
    msg.msg_namelen = sizeof(src);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ssize_t r;
    do {
        r = recvmsg(g_nl.sock, &msg, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0x18 || (msg.msg_flags & MSG_TRUNC)) return -1;

    uint16_t plen = 0;
    memcpy(&plen, ibuf + 0x12, 2);
    /* Mirror libnvram.so netlink_recv_mesg (0x29dc..0x29f4): copy from +0x10, len=plen+8 */
    uint32_t pid = 0;
    memcpy(&pid, ibuf + 0x0c, 4);
    memcpy(ibuf + 0x14, &pid, 4);
    size_t avail = (size_t)r - 0x10u;
    size_t copy_len = (size_t)plen + 8u;
    if (copy_len > avail) copy_len = avail;
    if (copy_len > out_cap || 0x10u + copy_len > sizeof(ibuf)) return -1;
    memcpy(out, ibuf + 0x10, copy_len);
    return (int)copy_len;
}
#endif

#ifdef __linux__
static uint8_t *build_nv_payload(const char *name, const char *val, size_t *out_len) {
    size_t nlen = strlen(name) + 1;
    size_t nalign = align4(nlen);
    size_t vlen = val ? (strlen(val) + 1) : 0;
    size_t total = nalign + 8u + vlen;
    uint8_t *buf = calloc(1, total);
    if (!buf) return NULL;

    uint32_t u32 = (uint32_t)nlen;
    memcpy(buf, &u32, 4);
    memcpy(buf + 4, name, nlen);
    size_t voff = nalign + 4u;
    u32 = (uint32_t)vlen;
    memcpy(buf + voff, &u32, 4);
    if (val && vlen > 0) memcpy(buf + voff + 4u, val, vlen);

    if (out_len) *out_len = total;
    return buf;
}
#endif

int bcm_knvram_set(const char *key, const char *val) {
    if (!key || !*key) return -1;
#ifdef __linux__
    size_t plen = 0;
    uint8_t *p = build_nv_payload(key, val, &plen);
    if (!p) return -1;
    int rc = -1;
    pthread_mutex_lock(&g_nl_mu);
    if (nv_nl_send(NVRAM_OP_SET, p, plen) == 0) {
        uint8_t rep[0x500];
        if (nv_nl_recv(rep, sizeof(rep)) > 0) {
            uint16_t op = 0;
            memcpy(&op, rep, 2);
            if (op == NVRAM_OP_SET) rc = 0;
        }
    }
    pthread_mutex_unlock(&g_nl_mu);
    free(p);
    return rc;
#else
    (void)val;
    return -1;
#endif
}

int bcm_knvram_unset(const char *key) {
    return bcm_knvram_set(key, NULL);
}

int bcm_knvram_get(const char *key, char *out, size_t out_sz) {
    if (!key || !out || out_sz == 0) return -1;
    out[0] = '\0';
#ifdef __linux__
    pthread_mutex_lock(&g_nl_mu);
    if (nv_nl_send(NVRAM_OP_GET, key, strlen(key) + 1) != 0) {
        pthread_mutex_unlock(&g_nl_mu);
        return -1;
    }
    uint8_t rep[0x500];
    int rlen = nv_nl_recv(rep, sizeof(rep));
    pthread_mutex_unlock(&g_nl_mu);
    if (rlen < 16) return -1;
    uint16_t op = 0, plen = 0;
    memcpy(&op, rep + 0, 2);
    memcpy(&plen, rep + 2, 2);
    if (op != NVRAM_OP_GET || plen == 0) return -1;

    /* Layout verified at libnvram.so 0x203c..0x2088:
     * +0x0c: name\0, aligned up to 4B, then +12 -> u32 val_len, +4 -> val\0 */
    const char *rname = (const char *)(rep + 12);
    size_t nalign = align4(strnlen(rname, (size_t)rlen - 12u) + 1);
    size_t vlen_off = nalign + 12u;
    if (vlen_off + 4u > (size_t)rlen) return -1;
    uint32_t vlen = 0;
    memcpy(&vlen, rep + vlen_off, 4);
    if (vlen == 0) return -1;
    const char *val = (const char *)(rep + vlen_off + 4u);
    size_t slen = strnlen(val, (size_t)rlen - vlen_off - 4u);
    if (slen >= out_sz) return -1;
    memcpy(out, val, slen);
    out[slen] = '\0';
    return 0;
#else
    return -1;
#endif
}

int bcm_knvram_getall(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return -1;
    memset(out, 0, out_sz);
#ifdef __linux__
    size_t used = 0;
    uint32_t seq_idx = 0;
    uint16_t op = NVRAM_OP_GETALL;
    int bad_ops = 0;
    pthread_mutex_lock(&g_nl_mu);
    while (op != NVRAM_OP_GETALL_END) {
        uint32_t req[2] = { (uint32_t)out_sz, seq_idx };
        if (nv_nl_send(NVRAM_OP_GETALL, req, sizeof(req)) != 0) {
            pthread_mutex_unlock(&g_nl_mu);
            return -1;
        }
        uint8_t rep[0x500];
        int rlen = nv_nl_recv(rep, sizeof(rep));
        if (rlen < 8) {
            pthread_mutex_unlock(&g_nl_mu);
            return -1;
        }
        uint16_t r_op = 0, r_len = 0;
        memcpy(&r_op, rep + 0, 2);
        memcpy(&r_len, rep + 2, 2);
        if (r_len == 0) break;
        if (r_op != NVRAM_OP_GETALL && r_op != NVRAM_OP_GETALL_END) {
            if (++bad_ops > 16) {
                pthread_mutex_unlock(&g_nl_mu);
                return -1;
            }
            continue;
        }
        bad_ops = 0;
        if (8u + (size_t)r_len > (size_t)rlen || used + r_len >= out_sz) {
            pthread_mutex_unlock(&g_nl_mu);
            return -1;
        }
        memcpy(out + used, rep + 8, r_len);
        used += r_len;
        out[used] = '\0';
        seq_idx++;
        op = r_op;
    }
    pthread_mutex_unlock(&g_nl_mu);
    return 0;
#else
    return -1;
#endif
}

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

int bcm_knvram_loadfile(const char *path) {
    FILE *fp = fopen(path ? path : DEF_NVRAM_FILE, "r");
    if (!fp) return -1;
    char line[1024];
    int rc = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim_ws(line);
        if (*p == '\0' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim_ws(p);
        char *v = trim_ws(eq + 1);
        if (*k && bcm_knvram_set(k, v) != 0) rc = -1;
    }
    fclose(fp);
    return rc;
}

/* ---------------- User NVRAM file layer (/data/.user_nvram.setting) ---------------- */
typedef struct {
    char *k;
    char *v;
} nv_pair_t;

static pthread_mutex_t g_unv_mu = PTHREAD_MUTEX_INITIALIZER;
static void nv_pairs_free(nv_pair_t *arr, size_t n);

static int nv_file_read_all(const char *path, nv_pair_t **out_arr, size_t *out_n) {
    *out_arr = NULL;
    *out_n = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return (errno == ENOENT) ? 0 : -1;
    size_t cap = 64, n = 0;
    nv_pair_t *arr = calloc(cap, sizeof(*arr));
    if (!arr) { fclose(fp); return -1; }
    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (l == 0 || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        if (n == cap) {
            cap *= 2;
            nv_pair_t *tmp = realloc(arr, cap * sizeof(*arr));
            if (!tmp) {
                nv_pairs_free(arr, n);
                fclose(fp);
                return -1;
            }
            arr = tmp;
        }
        char *valp = eq + 1;
        while (*valp == ' ' || *valp == '\t') valp++;
        char *dk = strdup(line);
        char *dv = strdup(valp);
        if (!dk || !dv) {
            free(dk);
            free(dv);
            nv_pairs_free(arr, n);
            fclose(fp);
            return -1;
        }
        arr[n].k = dk;
        arr[n].v = dv;
        n++;
    }
    if (ferror(fp)) {
        nv_pairs_free(arr, n);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out_arr = arr;
    *out_n = n;
    return 0;
}

static void nv_pairs_free(nv_pair_t *arr, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(arr[i].k);
        free(arr[i].v);
    }
    free(arr);
}

static int nv_lock_open(int op) {
    int fd = open(USER_NVRAM_LOCK, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return -1;
    while (flock(fd, op) < 0) {
        if (errno == EINTR) continue;
        close(fd);
        return -1;
    }
    return fd;
}

static void nv_lock_close(int fd) {
    if (fd >= 0) {
        (void)flock(fd, LOCK_UN);
        close(fd);
    }
}

static void fsync_parent_dir(const char *path) {
    const char *sl = strrchr(path, '/');
    char dir[256];
    if (!sl) strcpy(dir, ".");
    else if (sl == path) strcpy(dir, "/");
    else {
        size_t n = (size_t)(sl - path);
        if (n >= sizeof(dir)) n = sizeof(dir) - 1;
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
}

static int nv_file_save_all(const char *path, const nv_pair_t *arr, size_t n) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    (void)fchmod(fd, 0644);
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    int io_err = 0;
    for (size_t i = 0; i < n; i++) {
        if (arr[i].k && arr[i].v) {
            if (fprintf(fp, "%s=%s\n", arr[i].k, arr[i].v) < 0) {
                io_err = 1;
                break;
            }
        }
    }
    if (ferror(fp) || fflush(fp) != 0 || fsync(fileno(fp)) != 0) io_err = 1;
    if (fclose(fp) != 0) io_err = 1;
    if (io_err || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    fsync_parent_dir(path);
    return 0;
}

int bcm_unvram_get(const char *key, char *out, size_t out_sz) {
    if (!key || !out || out_sz == 0) return -1;
    pthread_mutex_lock(&g_unv_mu);
    int lfd = nv_lock_open(LOCK_SH);
    if (lfd < 0) {
        pthread_mutex_unlock(&g_unv_mu);
        return -1;
    }
    nv_pair_t *arr = NULL;
    size_t n = 0;
    if (nv_file_read_all(USER_NVRAM_FILE, &arr, &n) != 0) {
        nv_lock_close(lfd);
        pthread_mutex_unlock(&g_unv_mu);
        return -1;
    }
    int rc = -1;
    for (size_t i = 0; i < n; i++) {
        if (arr[i].k && strcmp(arr[i].k, key) == 0) {
            snprintf(out, out_sz, "%s", arr[i].v ? arr[i].v : "");
            rc = 0;
            break;
        }
    }
    nv_pairs_free(arr, n);
    nv_lock_close(lfd);
    pthread_mutex_unlock(&g_unv_mu);
    return rc;
}

int bcm_unvram_set(const char *key, const char *val) {
    if (!key || !*key) return -1;
    pthread_mutex_lock(&g_unv_mu);
    int lfd = nv_lock_open(LOCK_EX);
    if (lfd < 0) {
        pthread_mutex_unlock(&g_unv_mu);
        return -1;
    }
    nv_pair_t *arr = NULL;
    size_t n = 0;
    if (nv_file_read_all(USER_NVRAM_FILE, &arr, &n) != 0) {
        nv_lock_close(lfd);
        pthread_mutex_unlock(&g_unv_mu);
        return -1;
    }
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (arr[i].k && strcmp(arr[i].k, key) == 0) {
            if (val) {
                char *nv = strdup(val);
                if (!nv) {
                    nv_pairs_free(arr, n);
                    nv_lock_close(lfd);
                    pthread_mutex_unlock(&g_unv_mu);
                    return -1;
                }
                free(arr[i].v);
                arr[i].v = nv;
            } else {
                free(arr[i].v);
                arr[i].v = NULL;
            }
            found = 1;
            break;
        }
    }
    if (!found && val) {
        char *nk = strdup(key);
        char *nv = strdup(val);
        if (!nk || !nv) {
            free(nk);
            free(nv);
            nv_pairs_free(arr, n);
            nv_lock_close(lfd);
            pthread_mutex_unlock(&g_unv_mu);
            return -1;
        }
        nv_pair_t *tmp = realloc(arr, (n + 1) * sizeof(*arr));
        if (!tmp) {
            free(nk);
            free(nv);
            nv_pairs_free(arr, n);
            nv_lock_close(lfd);
            pthread_mutex_unlock(&g_unv_mu);
            return -1;
        }
        arr = tmp;
        arr[n].k = nk;
        arr[n].v = nv;
        n++;
    }
    int rc = nv_file_save_all(USER_NVRAM_FILE, arr, n);
    nv_pairs_free(arr, n);
    nv_lock_close(lfd);
    pthread_mutex_unlock(&g_unv_mu);
    return rc;
}

int bcm_unvram_unset(const char *key) {
    return bcm_unvram_set(key, NULL);
}

static int nv_kcommit_file(void) {
    char *buf = calloc(1, 0x20000);
    if (!buf) return -1;
    int rc = -1;
    pthread_mutex_lock(&g_unv_mu);
    int lfd = nv_lock_open(LOCK_EX);
    if (lfd >= 0 && bcm_knvram_getall(buf, 0x20000) == 0) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", KERN_NVRAM_FILE);
        int tfd = mkstemp(tmp);
        if (tfd >= 0) {
            (void)fchmod(tfd, 0644);
            FILE *fp = fdopen(tfd, "w");
            if (!fp) {
                close(tfd);
                unlink(tmp);
            } else {
                int io_err = 0;
                const char *p = buf;
                while (*p) {
                    if (fprintf(fp, "%s\n", p) < 0) {
                        io_err = 1;
                        break;
                    }
                    p += strlen(p) + 1;
                    if ((size_t)(p - buf) >= 0x20000) break;
                }
                if (ferror(fp) || fflush(fp) != 0 || fsync(fileno(fp)) != 0) io_err = 1;
                if (fclose(fp) != 0) io_err = 1;
                if (!io_err && rename(tmp, KERN_NVRAM_FILE) == 0) {
                    fsync_parent_dir(KERN_NVRAM_FILE);
                    rc = 0;
                } else {
                    unlink(tmp);
                }
            }
        }
    }
    nv_lock_close(lfd);
    pthread_mutex_unlock(&g_unv_mu);
    free(buf);
    return rc;
}

static void nvram_usage(void) {
    puts("Usage: nvram [-v lognum] <cmd> [args]");
    puts("  set name=value            set name with value");
    puts("  setflag name bit=val      set bit value");
    puts("  unset name                remove nvram entry");
    puts("  get name                  get nvram value with name");
    puts("  getflag name bit          get bit value");
    puts("  show|dump|getall          show all nvrams");
    puts("  kshow|kdump|kgetall       show all kernel nvrams");
    puts("  loadfile [file]           populate kernel nvram value from files");
    puts("  uloadfile [file]          populate userspace nvram value from file");
    puts("  usavefile [file]          save all userspace nvram value to file");
    puts("  restore_mfg <file>        restore mfg nvrams");
    puts("  kernelset <file>          populate kernel nvram from kernel config");
    puts("  kset name=value           set kernel nvram into runtime storage");
    puts("  kunset name               remove kernel nvram from runtime storage");
    puts("  kget name                 get kernel nvram from runtime storage");
    puts("  kcommit                   only save kernel nvram");
    puts("  commit [restart]          save nvram");
    puts("  restart                   restart wlan");
    puts("  godefault                 reset wireless nvram to default");
}

int bcm_cli_nvram(int argc, char **argv) {
    int idx = 1;
    if (idx + 1 < argc && strcmp(argv[idx], "-v") == 0) {
        idx += 2;
    }
    if (idx >= argc) {
        nvram_usage();
        return 255;
    }
    const char *cmd = argv[idx];
    int rem = argc - idx - 1;
    char **a = argv + idx + 1;

    if (strcmp(cmd, "kget") == 0) {
        if (rem < 1) { nvram_usage(); return 255; }
        char val[1024];
        if (bcm_knvram_get(a[0], val, sizeof(val)) == 0) puts(val);
        return 0;
    }
    if (strcmp(cmd, "kset") == 0) {
        if (rem < 1) { nvram_usage(); return 255; }
        char *buf = strdup(a[0]);
        if (!buf) return 255;
        char *eq = strchr(buf, '=');
        int rc;
        if (eq) { *eq = '\0'; rc = bcm_knvram_set(buf, eq + 1); }
        else    { rc = bcm_knvram_unset(buf); }
        free(buf);
        return rc == 0 ? 0 : 255;
    }
    if (strcmp(cmd, "kunset") == 0) {
        if (rem < 1) { nvram_usage(); return 255; }
        return bcm_knvram_unset(a[0]) == 0 ? 0 : 255;
    }
    if (strcmp(cmd, "kshow") == 0 || strcmp(cmd, "kdump") == 0 || strcmp(cmd, "kgetall") == 0) {
        puts("\n/---------------------------------/");
        puts("/|      KernelSpace NVRAMs       |/");
        puts("/---------------------------------/");
        char *buf = calloc(1, 0x20000);
        if (!buf) return 255;
        if (bcm_knvram_getall(buf, 0x20000) == 0 && buf[0] != '\0') {
            /* Kernel returns NUL-separated entries */
            const char *p = buf;
            while (*p) {
                puts(p);
                p += strlen(p) + 1;
                if ((size_t)(p - buf) >= 0x20000) break;
            }
        }
        free(buf);
        return 0;
    }
    if (strcmp(cmd, "kernelset") == 0) {
        if (rem < 1) { nvram_usage(); return 255; }
        if (bcm_knvram_loadfile(a[0]) != 0) return 255;
        puts("restore done!");
        return 0;
    }
    if (strcmp(cmd, "loadfile") == 0) {
        const char *f = (rem >= 1) ? a[0] : DEF_NVRAM_FILE;
        return bcm_knvram_loadfile(f) == 0 ? 0 : 255;
    }
    if (strcmp(cmd, "kcommit") == 0) {
        return nv_kcommit_file() == 0 ? 0 : 255;
    }
    if (strcmp(cmd, "get") == 0) {
        if (rem < 1) { nvram_usage(); return 255; }
        char val[1024];
        if (bcm_unvram_get(a[0], val, sizeof(val)) == 0 ||
            bcm_knvram_get(a[0], val, sizeof(val)) == 0) {
            puts(val);
        }
        return 0;
    }
    if (strcmp(cmd, "set") == 0) {
        if (rem < 1) { nvram_usage(); return 255; }
        char *buf = strdup(a[0]);
        if (!buf) return 255;
        char *eq = strchr(buf, '=');
        int rc;
        if (eq) { *eq = '\0'; rc = bcm_unvram_set(buf, eq + 1); }
        else    { rc = bcm_unvram_unset(buf); }
        free(buf);
        return rc == 0 ? 0 : 255;
    }
    if (strcmp(cmd, "unset") == 0) {
        if (rem < 1) { nvram_usage(); return 255; }
        return bcm_unvram_unset(a[0]) == 0 ? 0 : 255;
    }
    if (strcmp(cmd, "getflag") == 0) {
        if (rem < 2) { nvram_usage(); return 255; }
        char val[128];
        int bit = atoi(a[1]);
        if (bit < 0 || bit > 31) return 255;
        if (bcm_unvram_get(a[0], val, sizeof(val)) == 0 ||
            bcm_knvram_get(a[0], val, sizeof(val)) == 0) {
            unsigned long v = strtoul(val, NULL, 16);
            printf("%lu\n", (v >> bit) & 1UL);
        }
        return 0;
    }
    if (strcmp(cmd, "setflag") == 0) {
        if (rem < 2) { nvram_usage(); return 255; }
        char *eq = strchr(a[1], '=');
        if (!eq) { nvram_usage(); return 255; }
        int bit = atoi(a[1]);
        if (bit < 0 || bit > 31) return 255;
        int bval = atoi(eq + 1);
        char cur[128] = {0};
        unsigned long v = 0;
        if (bcm_unvram_get(a[0], cur, sizeof(cur)) == 0 ||
            bcm_knvram_get(a[0], cur, sizeof(cur)) == 0) {
            printf("value:%s->", cur);
            v = strtoul(cur, NULL, 16);
            if (bval) v |= (1UL << bit); else v &= ~(1UL << bit);
        } else {
            /* Mirror libnvram.asm:22d4..2344: missing key writes "0" */
            printf("value:NULL->");
            v = 0;
        }
        char nbuf[64];
        snprintf(nbuf, sizeof(nbuf), "%lx", v);
        if (bcm_unvram_set(a[0], nbuf) != 0) return 255;
        puts(nbuf);
        return 0;
    }
    if (strcmp(cmd, "show") == 0 || strcmp(cmd, "dump") == 0 || strcmp(cmd, "getall") == 0) {
        /* Mirror libnvram.asm:4030..41b8: UserSpace banner (no leading blank line) + KernelSpace banner */
        puts("/---------------------------------/");
        puts("/|       UserSpace NVRAMs        |/");
        puts("/---------------------------------/");
        pthread_mutex_lock(&g_unv_mu);
        int lfd = nv_lock_open(LOCK_SH);
        nv_pair_t *arr = NULL;
        size_t n = 0;
        if (lfd >= 0 && nv_file_read_all(USER_NVRAM_FILE, &arr, &n) == 0) {
            for (size_t i = 0; i < n; i++) printf("%s=%s\n", arr[i].k, arr[i].v);
            nv_pairs_free(arr, n);
        }
        nv_lock_close(lfd);
        pthread_mutex_unlock(&g_unv_mu);

        puts("\n/---------------------------------/");
        puts("/|      KernelSpace NVRAMs       |/");
        puts("/---------------------------------/");
        char *buf = calloc(1, 0x20000);
        if (buf) {
            if (bcm_knvram_getall(buf, 0x20000) == 0 && buf[0] != '\0') {
                const char *p = buf;
                while (*p) {
                    puts(p);
                    p += strlen(p) + 1;
                    if ((size_t)(p - buf) >= 0x20000) break;
                }
            }
            free(buf);
        }
        return 0;
    }
    if (strcmp(cmd, "uloadfile") == 0) {
        const char *f = (rem >= 1) ? a[0] : DEF_NVRAM_FILE;
        FILE *fp = fopen(f, "r");
        if (!fp) return 255;
        char line[1024];
        int rc = 0;
        while (fgets(line, sizeof(line), fp)) {
            char *p = trim_ws(line);
            if (*p == '\0' || *p == '#') continue;
            char *eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = trim_ws(p);
            if (*k && bcm_unvram_set(k, trim_ws(eq + 1)) != 0) rc = -1;
        }
        fclose(fp);
        if (rc != 0) return 255;
        printf("populate nvram from %s done!\n", f);
        return 0;
    }
    if (strcmp(cmd, "usavefile") == 0) {
        const char *f = (rem >= 1) ? a[0] : DEF_NVRAM_FILE;
        pthread_mutex_lock(&g_unv_mu);
        int lfd = nv_lock_open(LOCK_SH);
        nv_pair_t *arr = NULL;
        size_t n = 0;
        if (lfd < 0 || nv_file_read_all(USER_NVRAM_FILE, &arr, &n) != 0) {
            nv_lock_close(lfd);
            pthread_mutex_unlock(&g_unv_mu);
            return 255;
        }
        nv_lock_close(lfd);
        pthread_mutex_unlock(&g_unv_mu);
        int rc = nv_file_save_all(f, arr, n);
        nv_pairs_free(arr, n);
        if (rc == 0) printf("save nvram to %s done!\n", f);
        return rc == 0 ? 0 : 255;
    }
    if (strcmp(cmd, "restore_mfg") == 0) {
        if (rem < 1) { nvram_usage(); return 255; }
        printf("Restoring NVRAM to manufacturing default ... ");
        FILE *in = fopen(MFG_NVRAM_FILE, "rb");
        if (!in) { puts("fail."); return 255; }
        FILE *out = fopen(a[0], "a");
        if (!out) { fclose(in); puts("fail."); return 255; }
        char rec[1025];
        size_t n = 0;
        int ch;
        while ((ch = fgetc(in)) != EOF) {
            if (ch == 0) {
                if (n == 0) break; /* Broadcom NVRAM double-NUL end sentinel */
                rec[n] = '\0';
                fprintf(out, "%s\n", rec);
                n = 0;
            } else if (n < 1024) {
                rec[n++] = (char)ch;
            }
        }
        if (n > 0) {
            rec[n] = '\0';
            fprintf(out, "%s\n", rec);
        }
        fclose(out);
        fclose(in);
        puts("done.");
        return 0;
    }
    if (strcmp(cmd, "commit") == 0) {
        /* Persist kernel NVRAM if bcm_knvram is active; user NVRAM is already persisted atomically. */
        (void)nv_kcommit_file();
        sync();
        if (rem >= 1 && strncmp(a[0], "restart", 7) == 0) {
            if (system("killall -SIGUSR2 wlssk 2>/dev/null") == 0) return 0;
            if (system("killall -SIGUSR2 bcmmap 2>/dev/null") == 0) return 0;
            return system("killall -SIGUSR2 easymeshagent 2>/dev/null") == 0 ? 0 : 255;
        }
        return 0;
    }
    if (strcmp(cmd, "restart") == 0) {
        if (system("killall -SIGUSR2 wlssk 2>/dev/null") == 0) return 0;
        if (system("killall -SIGUSR2 bcmmap 2>/dev/null") == 0) return 0;
        return system("killall -SIGUSR2 easymeshagent 2>/dev/null") == 0 ? 0 : 255;
    }
    if (strcmp(cmd, "godefault") == 0) {
        unlink(KERN_NVRAM_FILE);
        unlink(USER_NVRAM_FILE);
        sync();
        return 0;
    }

    printf("\n=====\nInput cmd:%s is not supported \n======\n", cmd);
    nvram_usage();
    return 255;
}
