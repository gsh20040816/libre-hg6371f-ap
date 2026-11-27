/*
 * vm_mock_runner.c — Local QEMU ARMv7 Linux /init + ptrace Syscall-Mock Diff Harness
 *
 * Boots inside qemu-system-arm (-M virt -cpu cortex-a7) with Alpine armv7 kernel
 * and minimal initramfs containing:
 *   - Stock Broadcom ARM32 binaries + glibc 2.32 softfp libs (/opt/bcm/...)
 *   - Open-source static ARM32 musl binary (/open/bcmctl + symlinks)
 *   - Real metadata1/2 + synthetic bootfs fixtures (/fixtures/...)
 *
 * Uses ARM32 ptrace(PTRACE_SYSCALL + PTRACE_SET_SYSCALL(-1)) to intercept and mock
 * ALL Broadcom kernel interfaces (fcache, watchdog, bdmf_shell, nvram netlink 31,
 * ethswctl SIOCETHSWCTLOPS/SIOCIFREQ_EXT/SIOCSWANPORT/SIOCGWANPORT, UBI metadata
 * & bootfs volumes, /proc/bootstate/reset_reason, /proc/mtd, /proc/device-tree).
 *
 * Compares BOTH:
 *   1. Exact kernel-facing ABI trace (ioctl numbers, struct payloads, netlink ops,
 *      UBI_IOCVOLUP ordering + 1280B written metadata, reset_reason writes)
 *   2. Exact stdout, stderr, and exit code
 * between stock Broadcom binaries and our open-source bcmctl across read AND write ops!
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/ptrace.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* ARM32 EABI ptrace constants */
#ifndef PTRACE_GETREGS
#define PTRACE_GETREGS     12
#endif
#ifndef PTRACE_SETREGS
#define PTRACE_SETREGS     13
#endif
#ifndef PTRACE_SET_SYSCALL
#define PTRACE_SET_SYSCALL 23
#endif

struct arm_regs {
    uint32_t uregs[18]; /* r0..r12, sp, lr, pc, cpsr, orig_r0 */
};

/* ARM32 Linux EABI syscall numbers */
enum {
    ARM_SYS_read       = 3,
    ARM_SYS_write      = 4,
    ARM_SYS_open       = 5,
    ARM_SYS_close      = 6,
    ARM_SYS_lseek      = 19,
    ARM_SYS_ioctl      = 54,
    ARM_SYS__llseek    = 140,
    ARM_SYS_readv      = 145,
    ARM_SYS_writev     = 146,
    ARM_SYS_fstat64    = 197,
    ARM_SYS_socket     = 281,
    ARM_SYS_bind       = 282,
    ARM_SYS_sendto     = 290,
    ARM_SYS_recvfrom   = 292,
    ARM_SYS_sendmsg    = 296,
    ARM_SYS_recvmsg    = 297,
    ARM_SYS_openat     = 322,
};

/* Virtual FD assignments */
enum {
    VFD_BASE         = 1000,
    VFD_FCACHE       = 1001,
    VFD_WATCHDOG     = 1002,
    VFD_BDMF         = 1003,
    VFD_NETLINK_NV   = 1004,
    VFD_ETHSW_SOCK   = 1005,
    VFD_UBI_META1    = 1011,
    VFD_UBI_META2    = 1012,
    VFD_UBI_BOOTFS1  = 1013,
    VFD_UBI_BOOTFS2  = 1015,
    VFD_DT_ACTIVE    = 1020,
    VFD_RESET_REASON = 1021,
    VFD_PROC_MTD     = 1022,
    VFD_WDT_TIMELEFT = 1023,
    VFD_DEV_MTD2     = 1024,
    VFD_UBI_NAME_1   = 1031,
    VFD_UBI_NAME_2   = 1032,
    VFD_UBI_NAME_3   = 1033,
    VFD_UBI_NAME_5   = 1035,
};

typedef struct {
    char   data[32768];
    size_t len;
} strbuf_t;

static void sb_appendf(strbuf_t *sb, const char *fmt, ...) {
    if (sb->len >= sizeof(sb->data) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->data + sb->len, sizeof(sb->data) - sb->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t add = (size_t)n;
        if (sb->len + add >= sizeof(sb->data)) sb->len = sizeof(sb->data) - 1;
        else sb->len += add;
    }
}

static void sb_hex(strbuf_t *sb, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) sb_appendf(sb, "%02x", p[i]);
}

/* Read memory from tracee */
static int peek_bytes(pid_t pid, uint32_t addr, void *out, size_t len) {
    uint8_t *dst = (uint8_t *)out;
    size_t i = 0;
    while (i < len) {
        uint32_t aligned = (addr + (uint32_t)i) & ~3u;
        uint32_t off = (addr + (uint32_t)i) & 3u;
        errno = 0;
        long w = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)aligned, 0);
        if (w == -1 && errno != 0) return -1;
        uint8_t tmp[4];
        memcpy(tmp, &w, 4);
        size_t take = 4 - off;
        if (take > len - i) take = len - i;
        memcpy(dst + i, tmp + off, take);
        i += take;
    }
    return 0;
}

static int peek_cstr(pid_t pid, uint32_t addr, char *out, size_t max_len) {
    size_t i = 0;
    while (i + 1 < max_len) {
        uint8_t c = 0;
        if (peek_bytes(pid, addr + (uint32_t)i, &c, 1) < 0) {
            out[i] = '\0';
            return -1;
        }
        out[i++] = (char)c;
        if (c == '\0') return 0;
    }
    out[max_len - 1] = '\0';
    return 0;
}

static int poke_bytes(pid_t pid, uint32_t addr, const void *src_p, size_t len) {
    const uint8_t *src = (const uint8_t *)src_p;
    size_t i = 0;
    while (i < len) {
        uint32_t aligned = (addr + (uint32_t)i) & ~3u;
        uint32_t off = (addr + (uint32_t)i) & 3u;
        size_t take = 4 - off;
        if (take > len - i) take = len - i;
        long w = 0;
        if (off != 0 || take != 4) {
            errno = 0;
            w = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)aligned, 0);
            if (w == -1 && errno != 0) return -1;
        }
        uint8_t tmp[4];
        memcpy(tmp, &w, 4);
        memcpy(tmp + off, src + i, take);
        memcpy(&w, tmp, 4);
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)aligned, (void *)w) < 0) return -1;
        i += take;
    }
    return 0;
}

/* Fixture buffers loaded once at VM boot */
static uint8_t g_meta1_init[1280];
static uint8_t g_meta2_init[1280];
static uint8_t g_bootfs1[131072];
static uint8_t g_bootfs2[131072];

static void load_fixtures(void) {
    FILE *fp = fopen("/fixtures/metadata1.bin", "rb");
    if (fp) { (void)fread(g_meta1_init, 1, sizeof(g_meta1_init), fp); fclose(fp); }
    fp = fopen("/fixtures/metadata2.bin", "rb");
    if (fp) { (void)fread(g_meta2_init, 1, sizeof(g_meta2_init), fp); fclose(fp); }
    memset(g_bootfs1, 0xff, sizeof(g_bootfs1));
    memset(g_bootfs2, 0xff, sizeof(g_bootfs2));
    const char *t1 = "$imageversion: 5044p1GWOV_CT1331113 $";
    const char *t2 = "$imageversion: 5044p1GWOV_CT1331113 $";
    memcpy(g_bootfs1 + 0x1000, t1, strlen(t1));
    memcpy(g_bootfs2 + 0x1000, t2, strlen(t2));
}

/* Per-run mock kernel state */
typedef struct {
    uint8_t  meta1[1280];
    uint8_t  meta2[1280];
    size_t   pos[64];
    int      volup_armed[2];
    int64_t  volup_bytes[2];
    char     reset_reason[32];
    /* Netlink pending reply queue */
    uint8_t  nl_reply[2][512];
    size_t   nl_rlen[2];
    int      nl_rcount;
    int      nl_ridx;
    strbuf_t trace;
} mock_state_t;

static void mock_reset(mock_state_t *ms) {
    memset(ms, 0, sizeof(*ms));
    memcpy(ms->meta1, g_meta1_init, sizeof(ms->meta1));
    memcpy(ms->meta2, g_meta2_init, sizeof(ms->meta2));
    strcpy(ms->reset_reason, "0\n");
}

static int vfd_slot(int fd) {
    if (fd >= VFD_BASE && fd < VFD_BASE + 64) return fd - VFD_BASE;
    return -1;
}

static int map_special_path(const char *path) {
    if (strcmp(path, "/dev/fcache") == 0) return VFD_FCACHE;
    if (strcmp(path, "/dev/watchdog") == 0) return VFD_WATCHDOG;
    if (strcmp(path, "/dev/bdmf_shell") == 0) return VFD_BDMF;
    if (strcmp(path, "/dev/ubi0_1") == 0) return VFD_UBI_META1;
    if (strcmp(path, "/dev/ubi0_2") == 0) return VFD_UBI_META2;
    if (strcmp(path, "/dev/ubi0_3") == 0) return VFD_UBI_BOOTFS1;
    if (strcmp(path, "/dev/ubi0_5") == 0) return VFD_UBI_BOOTFS2;
    if (strcmp(path, "/proc/device-tree/chosen/active_image") == 0) return VFD_DT_ACTIVE;
    if (strcmp(path, "/proc/bootstate/reset_reason") == 0) return VFD_RESET_REASON;
    if (strcmp(path, "/proc/mtd") == 0) return VFD_PROC_MTD;
    if (strcmp(path, "/dev/mtd2") == 0) return VFD_DEV_MTD2;
    if (strcmp(path, "/sys/class/watchdog/watchdog0/timeleft") == 0) return VFD_WDT_TIMELEFT;
    if (strcmp(path, "/sys/class/ubi/ubi0_1/name") == 0) return VFD_UBI_NAME_1;
    if (strcmp(path, "/sys/class/ubi/ubi0_2/name") == 0) return VFD_UBI_NAME_2;
    if (strcmp(path, "/sys/class/ubi/ubi0_3/name") == 0) return VFD_UBI_NAME_3;
    if (strcmp(path, "/sys/class/ubi/ubi0_5/name") == 0) return VFD_UBI_NAME_5;
    return -1;
}

static const uint8_t *vfd_ro_buf(const mock_state_t *ms, int fd, size_t *out_len) {
    static const uint8_t dt_active[4] = { 0, 0, 0, 2 };
    static const char proc_mtd[] =
        "dev:    size   erasesize  name\n"
        "mtd0: 00100000 00020000 \"nvram\"\n"
        "mtd1: 00300000 00020000 \"loader\"\n"
        "mtd2: 07c00000 00020000 \"image\"\n";
    static const char wdt_tl[] = "72\n";
    switch (fd) {
    case VFD_UBI_META1:    *out_len = sizeof(ms->meta1); return ms->meta1;
    case VFD_UBI_META2:    *out_len = sizeof(ms->meta2); return ms->meta2;
    case VFD_UBI_BOOTFS1:  *out_len = sizeof(g_bootfs1); return g_bootfs1;
    case VFD_UBI_BOOTFS2:  *out_len = sizeof(g_bootfs2); return g_bootfs2;
    case VFD_DT_ACTIVE:    *out_len = 4; return dt_active;
    case VFD_RESET_REASON: *out_len = strlen(ms->reset_reason); return (const uint8_t *)ms->reset_reason;
    case VFD_PROC_MTD:     *out_len = strlen(proc_mtd); return (const uint8_t *)proc_mtd;
    case VFD_WDT_TIMELEFT: *out_len = strlen(wdt_tl); return (const uint8_t *)wdt_tl;
    case VFD_UBI_NAME_1:   *out_len = 10; return (const uint8_t *)"metadata1\n";
    case VFD_UBI_NAME_2:   *out_len = 10; return (const uint8_t *)"metadata2\n";
    case VFD_UBI_NAME_3:   *out_len = 8;  return (const uint8_t *)"bootfs1\n";
    case VFD_UBI_NAME_5:   *out_len = 8;  return (const uint8_t *)"bootfs2\n";
    default:               *out_len = 0;  return NULL;
    }
}

static void handle_nvram_nl_tx(mock_state_t *ms, const uint8_t *pkt, size_t len) {
    if (len < 0x18) return;
    uint16_t nltype_in = 0, nlflags_in = 0, op = 0, plen = 0;
    uint32_t nlseq_in = 0, pid_v = 0;
    memcpy(&nltype_in, pkt + 0x04, 2);
    memcpy(&nlflags_in, pkt + 0x06, 2);
    memcpy(&nlseq_in, pkt + 0x08, 4);
    memcpy(&pid_v, pkt + 0x0c, 4);
    memcpy(&op, pkt + 0x10, 2);
    memcpy(&plen, pkt + 0x12, 2);
    size_t pay_avail = (len > 0x18) ? len - 0x18 : 0;
    if (plen < pay_avail) pay_avail = plen;

    sb_appendf(&ms->trace, "NL_NV type=%u flags=%u seq=%u op=%u plen=%u payload=",
               nltype_in, nlflags_in, nlseq_in, op, plen);
    sb_hex(&ms->trace, pkt + 0x18, pay_avail > 64 ? 64 : pay_avail);
    sb_appendf(&ms->trace, "\n");

    ms->nl_rcount = 0;
    ms->nl_ridx = 0;

    if (op == 3) { /* GET */
        char key[64] = {0};
        size_t klen = pay_avail < 63 ? pay_avail : 63;
        memcpy(key, pkt + 0x18, klen);
        const char *val = (strcmp(key, "boardid") == 0) ? "HG6371F" : "";
        uint8_t *r = ms->nl_reply[0];
        if (val[0] != '\0') {
            /* Exact Broadcom namevalue_buf layout:
             * +0x00 u32 nlen, +0x04 name\0 + pad4, +off u32 vlen, +off+4 val\0 */
            uint32_t nlen = (uint32_t)(strlen(key) + 1);
            size_t nalign = (nlen + 3u) & ~3u;
            uint32_t vlen = (uint32_t)(strlen(val) + 1);
            uint16_t rplen = (uint16_t)(4u + nalign + 4u + vlen);
            uint32_t total = (uint32_t)((0x18u + rplen + 3u) & ~3u);
            memset(r, 0, total);
            memcpy(r + 0x00, &total, 4);
            uint16_t nltype = 5;
            memcpy(r + 0x04, &nltype, 2);
            uint32_t seq = 1;
            memcpy(r + 0x08, &seq, 4);
            memcpy(r + 0x0c, &pid_v, 4);
            memcpy(r + 0x10, &op, 2);
            memcpy(r + 0x12, &rplen, 2);
            memcpy(r + 0x18, &nlen, 4);
            memcpy(r + 0x1c, key, nlen);
            memcpy(r + 0x1c + nalign, &vlen, 4);
            memcpy(r + 0x20 + nalign, val, vlen);
            ms->nl_rlen[0] = total;
        } else {
            uint32_t total = 0x18;
            memset(r, 0, total);
            memcpy(r + 0x00, &total, 4);
            uint16_t nltype = 5;
            memcpy(r + 0x04, &nltype, 2);
            uint32_t seq = 1;
            memcpy(r + 0x08, &seq, 4);
            memcpy(r + 0x0c, &pid_v, 4);
            memcpy(r + 0x10, &op, 2);
            ms->nl_rlen[0] = total;
        }
        ms->nl_rcount = 1;
    } else if (op == 2) { /* SET */
        uint32_t total = 0x18;
        uint8_t *r = ms->nl_reply[0];
        memset(r, 0, total);
        memcpy(r + 0x00, &total, 4);
        uint16_t nltype = 5;
        memcpy(r + 0x04, &nltype, 2);
        memcpy(r + 0x0c, &pid_v, 4);
        memcpy(r + 0x10, &op, 2);
        ms->nl_rlen[0] = total;
        ms->nl_rcount = 1;
    } else if (op == 5) { /* GETALL */
        uint32_t out_sz = 0, seq_idx = 0;
        if (pay_avail >= 8) {
            memcpy(&out_sz, pkt + 0x18, 4);
            memcpy(&seq_idx, pkt + 0x1c, 4);
        }
        if (out_sz == 0) return;
        const char data[] = "boardid=HG6371F\0mac=aa:bb:cc:dd:ee:ff\0";
        uint16_t dlen = (uint16_t)(sizeof(data) - 1);
        uint16_t end_op = 6;
        uint32_t total = (uint32_t)(0x18 + dlen);
        uint8_t *r = ms->nl_reply[0];
        memset(r, 0, total);
        memcpy(r + 0x00, &total, 4);
        uint16_t nltype = 5;
        memcpy(r + 0x04, &nltype, 2);
        memcpy(r + 0x0c, &pid_v, 4);
        memcpy(r + 0x10, &end_op, 2);
        memcpy(r + 0x12, &dlen, 2);
        memcpy(r + 0x18, data, dlen);
        ms->nl_rlen[0] = total;
        ms->nl_rcount = 1;
    }
}

static void handle_wl_ioctl(pid_t pid, mock_state_t *ms, uint32_t ifreq_addr,
                            uint8_t *poke_buf, uint32_t *poke_addr, size_t *poke_len,
                            int32_t *mock_ret) {
    char ifn[16] = {0};
    peek_bytes(pid, ifreq_addr, ifn, 16);
    ifn[15] = '\0';

    uint32_t ioc_ptr = 0;
    peek_bytes(pid, ifreq_addr + 16, &ioc_ptr, 4);
    if (!ioc_ptr) { *mock_ret = -EFAULT; return; }

    uint32_t ioc[6] = {0}; /* cmd, buf_ptr, len, set|pad, used, needed */
    peek_bytes(pid, ioc_ptr, ioc, sizeof(ioc));
    uint32_t wcmd    = ioc[0];
    uint32_t buf_ptr = ioc[1];
    uint32_t blen    = ioc[2];
    uint8_t  set     = (uint8_t)(ioc[3] & 0xffu);

    /* Silent startup capability probes */
    if (wcmd == 0) { /* WLC_GET_MAGIC */
        uint32_t magic = 0x14e46c77u;
        if (buf_ptr && blen >= 4) {
            memcpy(poke_buf, &magic, 4);
            *poke_addr = buf_ptr;
            *poke_len = 4;
        }
        *mock_ret = 0;
        return;
    }
    if (wcmd == 1) { /* WLC_GET_VERSION */
        uint32_t ver = 2;
        if (buf_ptr && blen >= 4) {
            memcpy(poke_buf, &ver, 4);
            *poke_addr = buf_ptr;
            *poke_len = 4;
        }
        *mock_ret = 0;
        return;
    }

    uint8_t inbuf[512];
    memset(inbuf, 0, sizeof(inbuf));
    size_t peek_n = blen < sizeof(inbuf) ? blen : sizeof(inbuf);
    if (buf_ptr && peek_n > 0) peek_bytes(pid, buf_ptr, inbuf, peek_n);

    uint8_t out[4096];
    memset(out, 0, sizeof(out));
    size_t out_n = 0;
    *mock_ret = 0;

    if (wcmd == 262 || wcmd == 263) { /* WLC_GET_VAR / WLC_SET_VAR */
        char name[64] = {0};
        size_t nlen = 0;
        while (nlen < 63 && nlen < peek_n && inbuf[nlen] != '\0') {
            name[nlen] = (char)inbuf[nlen];
            nlen++;
        }
        name[nlen] = '\0';
        size_t poff = nlen + 1;
        const uint8_t *p = (poff < peek_n) ? (inbuf + poff) : NULL;
        size_t plen = (poff < blen) ? (blen - poff) : 0;

        if (wcmd == 263) { /* SET_VAR */
            size_t show = plen < 64 ? plen : 64;
            sb_appendf(&ms->trace, "WL_SET_VAR if=%s name=%s len=%zu val=", ifn, name, plen);
            if (p && show > 0) sb_hex(&ms->trace, p, show);
            sb_appendf(&ms->trace, "\n");
            return;
        }

        /* GET_VAR: log name + meaningful input params */
        if (strcmp(name, "bw_cap") == 0 && p && plen >= 4) {
            uint32_t band = 0; memcpy(&band, p, 4);
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s band=%u\n", ifn, name, band);
            uint32_t cap = 0xf; memcpy(out, &cap, 4); out_n = 4;
        } else if ((strcmp(name, "he") == 0 || strcmp(name, "twt") == 0) && p && plen >= 4) {
            uint16_t id = 0, xl = 0; memcpy(&id, p, 2); memcpy(&xl, p + 2, 2);
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s id=%u xlen=%u\n", ifn, name, id, xl);
            uint32_t val = 1; memcpy(out, &val, 4); out_n = 4;
        } else if (strcmp(name, "sta_info") == 0 && p && plen >= 6) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s mac=", ifn, name);
            sb_hex(&ms->trace, p, 6);
            sb_appendf(&ms->trace, "\n");
            uint16_t ver = 8, slen = 588, aid = 0xc001, lsn = 10;
            uint32_t flags = 0x10185b, idle = 2, in_s = 3600, tx_tot = 1234, rx_tot = 5678;
            uint32_t tx_rate = 1200900, rx_rate = 864700;
            uint64_t tx_b = 987654ULL, rx_b = 432100ULL;
            memcpy(out + 0, &ver, 2);
            memcpy(out + 2, &slen, 2);
            memcpy(out + 10, &aid, 2);
            memcpy(out + 12, &flags, 4);
            memcpy(out + 16, &idle, 4);
            memcpy(out + 20, &in_s, 4);
            memcpy(out + 38, &lsn, 2);
            memcpy(out + 44, &tx_tot, 4);
            memcpy(out + 60, &rx_tot, 4);
            memcpy(out + 92, &tx_rate, 4);
            memcpy(out + 96, &rx_rate, 4);
            memcpy(out + 128, &tx_b, 8);
            memcpy(out + 144, &rx_b, 8);
            int32_t rssi_arr[4] = { -51, -53, 0, 0 };
            memcpy(out + 228, rssi_arr, 16);
            out[324] = (uint8_t)(int8_t)-52;
            out_n = 588;
        } else if (strcmp(name, "sta_monitor") == 0 && p && plen >= 12) {
            uint32_t scmd = 0; memcpy(&scmd, p, 4);
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s scmd=%u\n", ifn, name, scmd);
            if (scmd == 9) { /* dump */
                uint16_t ver = 1, rlen = 0x30;
                uint32_t enabled = 1, cnt = 1;
                uint32_t dcnt = 59779, mcnt = 154, ccnt = 39;
                memcpy(out + 0x00, &ver, 2);
                memcpy(out + 0x02, &rlen, 2);
                memcpy(out + 0x04, &enabled, 4);
                memcpy(out + 0x08, &dcnt, 4);
                memcpy(out + 0x0c, &mcnt, 4);
                memcpy(out + 0x10, &ccnt, 4);
                memcpy(out + 0x14, &cnt, 4);
                uint8_t ea[6] = { 0xce, 0x68, 0x6d, 0xce, 0xf4, 0xca };
                int32_t rssi = -50, amt = -1;
                uint16_t cs = 0xeb32;
                uint32_t offms = 0;
                memcpy(out + 0x18 + 0, ea, 6);
                memcpy(out + 0x18 + 8, &rssi, 4);
                memcpy(out + 0x18 + 12, &amt, 4);
                memcpy(out + 0x18 + 16, &cs, 2);
                memcpy(out + 0x18 + 20, &offms, 4);
                out_n = 0x30;
            } else {
                uint32_t v = 1; memcpy(out, &v, 4); out_n = 4;
            }
        } else if (strcmp(name, "dump") == 0 && p) {
            char sub[64] = {0};
            size_t sl = 0;
            while (sl < 63 && poff + sl < peek_n && p[sl] != '\0') { sub[sl] = (char)p[sl]; sl++; }
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=dump sub=[%s]\n", ifn, sub);
            const char *txt = "CSI Monitor: 0\nNumber of clients: 0\n";
            out_n = strlen(txt) + 1;
            memcpy(out, txt, out_n);
        } else if (strcmp(name, "chanim_stats") == 0) {
            uint32_t req_blen = 0, req_cnt = 0;
            if (p && plen >= 12) {
                memcpy(&req_blen, p + 0, 4);
                memcpy(&req_cnt, p + 8, 4);
            }
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s req_blen=%u req_cnt=%u\n",
                       ifn, name, req_blen, req_cnt);
            uint32_t buflen = 92, ver = 4, count = 1;
            memcpy(out + 0, &buflen, 4);
            memcpy(out + 4, &ver, 4);
            memcpy(out + 8, &count, 4);
            uint8_t *e = out + 12;
            uint32_t glitch = 9, badplcp = 12;
            uint16_t cs = 0xeb32;
            uint32_t ts = 6979328;
            memcpy(e + 0x00, &glitch, 4);
            memcpy(e + 0x04, &badplcp, 4);
            e[0x11] = (uint8_t)(int8_t)-84;
            memcpy(e + 0x12, &cs, 2);
            memcpy(e + 0x14, &ts, 4);
            e[0x20] = 89; /* chan_idle */
            uint32_t acc[9] = { 400, 600, 200, 300, 100, 0, 8600, 200, 100 };
            memcpy(e + 0x24, acc, sizeof(acc));
            uint32_t tot_tm = 1000;
            memcpy(e + 0x48, &tot_tm, 4);
            out_n = 92;
        } else if (strcmp(name, "counters") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            /* XTLV starts at offset +4 right after {u16 version, u16 datalen} */
            uint16_t ver = 30, xid = 0x100, xlen = 500, dlen = 4 + 500;
            memcpy(out + 0, &ver, 2);
            memcpy(out + 2, &dlen, 2);
            memcpy(out + 4, &xid, 2);
            memcpy(out + 6, &xlen, 2);
            uint32_t txframe = 100, txbyte = 20000, rxframe = 300, rxbyte = 60000;
            memcpy(out + 8 + 0x00, &txframe, 4);
            memcpy(out + 8 + 0x04, &txbyte, 4);
            memcpy(out + 8 + 0x3c, &rxframe, 4);
            memcpy(out + 8 + 0x40, &rxbyte, 4);
            out_n = 8 + xlen;
        } else if (strcmp(name, "scan_ver") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=scan_ver\n", ifn);
            uint16_t sv[4] = { 1, 8, 2, 0 }; /* scan_ver = 2 at +4 */
            memcpy(out, sv, sizeof(sv));
            out_n = sizeof(sv);
        } else if (strcmp(name, "chanspecs") == 0 && p && plen >= 10) {
            uint16_t hint = 0; uint32_t mcnt = 0; char cc[5] = {0};
            memcpy(&hint, p + 0, 2); memcpy(cc, p + 2, 4); memcpy(&mcnt, p + 6, 4);
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=chanspecs hint=0x%x cc=%s cnt=%u\n",
                       ifn, hint, cc, mcnt);
            uint32_t res[3] = { 2, 0xd024, 0xeb32 };
            memcpy(out, res, sizeof(res)); out_n = sizeof(res);
        } else if (strcmp(name, "mbssid") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=mbssid\n", ifn);
            out[0] = 1; out[1] = 4; out[2] = 1; out[3] = 0;
            out_n = 4;
        } else if (strcmp(name, "txpwr_target_max") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=txpwr_target_max\n", ifn);
            uint32_t ver = 0; uint16_t cs = 0xeb32;
            memcpy(out + 0, &ver, 4);
            memcpy(out + 4, &cs, 2);
            out[6] = 80; out[7] = 78; out[10] = 2; /* 20.00, 19.50, nchain=2 */
            out_n = 11;
        } else if (strcmp(name, "dfs_ap_move") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=dfs_ap_move\n", ifn);
            out[0] = 1; out[1] = (uint8_t)(int8_t)-1; /* status=-1 (IDLE) */
            uint16_t dver = 1, nsub = 0;
            memcpy(out + 4, &dver, 2); memcpy(out + 6, &nsub, 2);
            out_n = 12;
        } else if (strcmp(name, "memuse") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=memuse len=%u\n", ifn, blen);
            uint16_t ver = 1, mlen = 80;
            uint32_t tot = 1048576, mfree = 524288;
            memcpy(out + 0, &ver, 2); memcpy(out + 2, &mlen, 2);
            memcpy(out + 0x14, &tot, 4); memcpy(out + 0x18, &mfree, 4);
            out_n = 80;
        } else if (strcmp(name, "country_rev") == 0 && p && plen >= 2) {
            char cc[4] = {0}; memcpy(cc, p, plen < 4 ? plen : 3);
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=country_rev cc=%s\n", ifn, cc);
            int32_t r[3] = { 2, 947, 948 };
            memcpy(out, r, sizeof(r)); out_n = sizeof(r);
        } else if (strcmp(name, "bss") == 0 && p && plen >= 4) {
            int32_t bidx = 0; memcpy(&bidx, p, 4);
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=bss idx=%d\n", ifn, bidx);
            int32_t v = 1; memcpy(out, &v, 4); out_n = 4;
        } else if (strcmp(name, "per_chan_info") == 0 && p && plen >= 4) {
            uint32_t cs = 0; memcpy(&cs, p, 4);
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=per_chan_info cs=0x%x\n", ifn, cs);
            /* ch36 (0xd024): VALID_HW|VALID_SW|5G = 0x7; ch52 (0xd034): +RADAR|PASSIVE = 0x2f */
            uint32_t flg = ((cs & 0xffu) == 52u) ? 0x002fu : 0x0007u;
            memcpy(out, &flg, 4); out_n = 4;
        } else if (strcmp(name, "txpwr_cap") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=txpwr_cap\n", ifn);
            uint32_t raw = 0x0000504eu; /* core0=78, core1=80, core2=0, core3=0 */
            memcpy(out, &raw, 4); out_n = 4;
        } else if (strcmp(name, "cfp_dump") == 0 && p && plen >= 10) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=cfp_dump mac=", ifn);
            sb_hex(&ms->trace, p + 4, 6);
            sb_appendf(&ms->trace, "\n");
            const char *txt = "CFP_OK\n";
            out_n = strlen(txt) + 1; memcpy(out, txt, out_n);
        } else if (strcmp(name, "muinfo") == 0 && p && plen >= 1) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=muinfo verbose=%u\n", ifn, (unsigned)p[0]);
            const char *txt = "MUINFO_OK\n";
            out_n = strlen(txt) + 1; memcpy(out, txt, out_n);
        } else if (strcmp(name, "taf") == 0 && p && plen >= 20) {
            uint16_t tver = 0; memcpy(&tver, p + 6, 2);
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=taf ver=%u ea=", ifn, tver);
            sb_hex(&ms->trace, p, 6);
            sb_appendf(&ms->trace, "\n");
            static const uint8_t ff[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
            if (memcmp(p, ff, 6) != 0) {
                /* MAC-only query reply */
                memcpy(out, p, 6);
                uint32_t sch = 1, prio = 2;
                memcpy(out + 8, &sch, 4); memcpy(out + 12, &prio, 4);
                const char *sname = "ebs";
                memcpy(out + 20, sname, strlen(sname) + 1);
                out_n = 20 + strlen(sname) + 1;
            } else {
                const char *txt = "TAF_OK\n";
                memcpy(out + 20, txt, strlen(txt) + 1);
                out_n = 20 + strlen(txt) + 1;
            }
        } else if (strcmp(name, "wnm") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=wnm\n", ifn);
            uint32_t mask = 0x03u; /* BSS-Transition | Proxy-ARP */
            memcpy(out, &mask, 4); out_n = 4;
        } else if (strcmp(name, "authe_sta_list") == 0 || strcmp(name, "autho_sta_list") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            uint32_t cnt = 1;
            static const uint8_t m1[6] = { 0xce, 0x68, 0x6d, 0xce, 0xf4, 0xca };
            memcpy(out, &cnt, 4); memcpy(out + 4, m1, 6); out_n = 10;
        } else if (strcmp(name, "chanim_state") == 0 && p && plen >= 4) {
            uint32_t cs = 0; memcpy(&cs, p, 4);
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=chanim_state cs=0x%x\n", ifn, cs);
            int32_t st = 0; memcpy(out, &st, 4); out_n = 4;
        } else if (strcmp(name, "phy_rssi_ant") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=phy_rssi_ant\n", ifn);
            uint32_t ver = 1, cnt = 2;
            memcpy(out + 0, &ver, 4); memcpy(out + 4, &cnt, 4);
            out[8] = (uint8_t)(int8_t)-50; out[9] = (uint8_t)(int8_t)-52;
            out_n = 16;
        } else if (strcmp(name, "chanspec") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            uint32_t cs = 0xeb32u; memcpy(out, &cs, 4); out_n = 4;
        } else if (strcmp(name, "country") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            memcpy(out + 0, "CN\0\0", 4);
            int32_t rev = 947; memcpy(out + 4, &rev, 4);
            memcpy(out + 8, "CN\0\0", 4);
            out_n = 12;
        } else if (strcmp(name, "qtxpower") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            uint32_t v = 127; memcpy(out, &v, 4); out_n = 4;
        } else if (strcmp(name, "wme_ac_ap") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            static const uint8_t ac[16] = {
                0x02, 0x53, 0x2f, 0x00,
                0x27, 0xa4, 0x00, 0x00,
                0x41, 0x43, 0x5e, 0x00,
                0x61, 0x32, 0x2f, 0x00,
            };
            memcpy(out, ac, 16); out_n = 16;
        } else if (strcmp(name, "cur_etheraddr") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            static const uint8_t mac[6] = { 0x98, 0xee, 0xca, 0x15, 0x57, 0x48 };
            memcpy(out, mac, 6); out_n = 6;
        } else if (strcmp(name, "ver") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            const char *v = "Broadcom BCA: 17.10 RC251.3202\nwl1: May 13 2026 11:02:40 version 17.10.251.3202 (r816340)-PvB0.W6\n";
            out_n = strlen(v) + 1; memcpy(out, v, out_n);
        } else if (strcmp(name, "cap") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            const char *v = "ap sta wme 160 11ax stamon csimon ";
            out_n = strlen(v) + 1; memcpy(out, v, out_n);
        } else if (strcmp(name, "phy_tempsense") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            int32_t v = 67; memcpy(out, &v, 4); out_n = 4;
        } else if (strcmp(name, "wme_apsd") == 0 || strcmp(name, "bssmax") == 0 ||
                   strcmp(name, "chanim_mode") == 0 || strcmp(name, "csimon") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            int32_t v = (strcmp(name, "bssmax") == 0) ? 16 : 1;
            memcpy(out, &v, 4); out_n = 4;
        } else if (strcmp(name, "dfs_status") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            uint32_t st = 0, dur = 0; uint16_t cs = 0xeb32;
            memcpy(out + 0, &st, 4); memcpy(out + 4, &dur, 4); memcpy(out + 8, &cs, 2);
            out_n = 16;
        } else if (strcmp(name, "wme_counters") == 0) {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            uint16_t ver = 1, wlen = 196;
            memcpy(out + 0, &ver, 2); memcpy(out + 2, &wlen, 2);
            /* Non-zero tx_expired[0..3] at +0xa4, +0xac, +0xb4, +0xbc (8B stride!) */
            uint32_t e0 = 11, e1 = 22, e2 = 33, e3 = 44;
            memcpy(out + 0xa4, &e0, 4);
            memcpy(out + 0xac, &e1, 4);
            memcpy(out + 0xb4, &e2, 4);
            memcpy(out + 0xbc, &e3, 4);
            out_n = 196;
        } else {
            sb_appendf(&ms->trace, "WL_GET_VAR if=%s name=%s\n", ifn, name);
            int32_t v = 0; memcpy(out, &v, 4); out_n = 4;
        }
    } else {
        /* Numeric WLC ioctls */
        if (set) {
            size_t show = blen < 96 ? blen : 96;
            if (wcmd == 201 && show > 10) show = 10; /* scb_val_t has 2B uninit tail pad */
            if (wcmd == 216 && show > 4) show = 4;   /* WLC_SET_AUTOCHANNEL leaves [4..7] uninit in stock */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=%u set=1 len=%u val=", ifn, wcmd, blen);
            if (show > 0) sb_hex(&ms->trace, inbuf, show);
            sb_appendf(&ms->trace, "\n");
            return;
        }
        if (wcmd == 127 && blen >= 10) { /* WLC_GET_RSSI (scb_val_t: MAC at +4) */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=127 set=0 mac=", ifn);
            sb_hex(&ms->trace, inbuf + 4, 6);
            sb_appendf(&ms->trace, "\n");
            int32_t rssi = -52; memcpy(out, &rssi, 4); out_n = 4;
        } else if (wcmd == 69) { /* WLC_GET_MACLIST */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=69 set=0\n", ifn);
            uint32_t cnt = 1;
            static const uint8_t m1[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
            memcpy(out, &cnt, 4); memcpy(out + 4, m1, 6); out_n = 10;
        } else if (wcmd == 256) { /* WLC_CURRENT_TXPWR (93B tx_power_legacy_t) */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=256 set=0 len=%u\n", ifn, blen);
            out[0] = 84; out[0x18] = 80; /* min(84, 80) = 80 qdBm = 20.0 dBm = 100 mW */
            out_n = 93;
        } else if (wcmd == 159) { /* WLC_GET_ASSOCLIST */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=159 set=0\n", ifn);
            uint32_t cnt = 2;
            static const uint8_t m1[6] = { 0xce, 0x68, 0x6d, 0xce, 0xf4, 0xca };
            static const uint8_t m2[6] = { 0xec, 0x2c, 0x0d, 0x34, 0x6d, 0x0a };
            memcpy(out, &cnt, 4);
            memcpy(out + 4, m1, 6);
            memcpy(out + 10, m2, 6);
            out_n = 16;
        } else if (wcmd == 162 || wcmd == 117 || wcmd == 218) { /* UP, AP, FRAMEBURST */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=%u set=0\n", ifn, wcmd);
            int32_t v = 1; memcpy(out, &v, 4); out_n = 4;
        } else if (wcmd == 220) { /* WLC_GET_PWRPERCENT */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=220 set=0\n", ifn);
            int32_t v = 100; memcpy(out, &v, 4); out_n = 4;
        } else if (wcmd == 245) { /* WLC_GET_SPECT_MANAGMENT */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=245 set=0\n", ifn);
            int32_t v = 0; memcpy(out, &v, 4); out_n = 4;
        } else if (wcmd == 135) { /* WLC_GET_PHY_NOISE */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=135 set=0\n", ifn);
            int32_t v = -84; memcpy(out, &v, 4); out_n = 4;
        } else if (wcmd == 141) { /* WLC_GET_BAND */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=141 set=0\n", ifn);
            int32_t v = 0; memcpy(out, &v, 4); out_n = 4;
        } else if (wcmd == 140) { /* WLC_GET_BANDLIST */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=140 set=0\n", ifn);
            uint32_t b[2] = { 1, 1 }; memcpy(out, b, 8); out_n = 8;
        } else if (wcmd == 37) { /* WLC_GET_RADIO */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=37 set=0\n", ifn);
            uint32_t v = 0; memcpy(out, &v, 4); out_n = 4;
        } else if (wcmd == 23) { /* WLC_GET_BSSID */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=23 set=0\n", ifn);
            static const uint8_t bssid[6] = { 0x98, 0xee, 0xca, 0x15, 0x57, 0x48 };
            memcpy(out, bssid, 6); out_n = 6;
        } else if (wcmd == 25) { /* WLC_GET_SSID */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=25 set=0\n", ifn);
            uint32_t sl = 7; memcpy(out, &sl, 4); memcpy(out + 4, "OpenWrt", 7); out_n = 36;
        } else if (wcmd == 29) { /* WLC_GET_CHANNEL */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=29 set=0\n", ifn);
            int32_t ch[3] = { 48, 48, -1 }; memcpy(out, ch, 12); out_n = 12;
        } else if (wcmd == 217) { /* WLC_GET_VALID_CHANNELS */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=217 set=0\n", ifn);
            uint32_t chs[5] = { 4, 36, 40, 44, 48 }; memcpy(out, chs, sizeof(chs)); out_n = sizeof(chs);
        } else if (wcmd == 98) { /* WLC_GET_REVINFO */
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=98 set=0\n", ifn);
            uint32_t r[25] = {0};
            r[0] = 0x14e4; r[1] = 0x4492; r[2] = 0x150e6; r[3] = 1; r[4] = 130;
            r[5] = 0x99d; r[6] = 0x14e4; r[7] = 0x1303; r[8] = 0; /* driverrev in r[19..22] */
            r[9] = 0x06bd6c35; r[10] = 8; r[11] = 0x6756; r[12] = 0xb; r[13] = 131;
            r[14] = 0; r[15] = 0; r[16] = 784781; r[17] = 1; r[18] = 2;
            r[19] = 17; r[20] = 10; r[21] = 251; r[22] = 3202; r[23] = 1;
            memcpy(out, r, sizeof(r)); out_n = sizeof(r);
        } else {
            sb_appendf(&ms->trace, "WL_IOCTL if=%s cmd=%u set=0\n", ifn, wcmd);
            uint32_t v = 0; memcpy(out, &v, 4); out_n = 4;
        }
    }

    if (out_n > 0 && buf_ptr) {
        if (out_n > blen) out_n = blen;
        memcpy(poke_buf, out, out_n);
        *poke_addr = buf_ptr;
        *poke_len = out_n;
    }
}

typedef struct {
    int      rc;
    strbuf_t out;
    strbuf_t err;
    strbuf_t trace;
    uint8_t  meta1_after[1280];
    uint8_t  meta2_after[1280];
    char     reset_reason_after[32];
} run_result_t;

static void drain_fd(int fd, strbuf_t *sb) {
    char tmp[512];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        size_t space = sizeof(sb->data) - 1 - sb->len;
        size_t take = (size_t)n < space ? (size_t)n : space;
        memcpy(sb->data + sb->len, tmp, take);
        sb->len += take;
        sb->data[sb->len] = '\0';
    }
}

static int run_traced(const char *exe, char *const argv[], char *const envp[],
                      mock_state_t *ms, run_result_t *res) {
    memset(res, 0, sizeof(*res));
    mock_reset(ms);

    int out_p[2], err_p[2];
    if (pipe(out_p) < 0 || pipe(err_p) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(out_p[1], 1);
        dup2(err_p[1], 2);
        close(out_p[0]); close(out_p[1]);
        close(err_p[0]); close(err_p[1]);
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execve(exe, argv, envp);
        _exit(127);
    }

    close(out_p[1]);
    close(err_p[1]);
    fcntl(out_p[0], F_SETFL, O_NONBLOCK);
    fcntl(err_p[0], F_SETFL, O_NONBLOCK);

    int status = 0;
    waitpid(pid, &status, 0);
    ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);
    ptrace(PTRACE_SYSCALL, pid, 0, 0);

    int in_sys = 0;
    int mock_active = 0;
    int32_t mock_ret = 0;
    uint32_t poke_addr = 0;
    static uint8_t poke_buf[16384];
    size_t   poke_len = 0;
    uint32_t poke2_addr = 0;
    uint8_t  poke2_buf[128];
    size_t   poke2_len = 0;

    for (;;) {
        if (waitpid(pid, &status, 0) < 0) break;
        drain_fd(out_p[0], &res->out);
        drain_fd(err_p[0], &res->err);

        if (WIFEXITED(status)) {
            res->rc = WEXITSTATUS(status);
            break;
        }
        if (WIFSIGNALED(status)) {
            res->rc = 128 + WTERMSIG(status);
            break;
        }
        if (!WIFSTOPPED(status)) continue;

        int sig = WSTOPSIG(status);
        if (sig != (SIGTRAP | 0x80)) {
            ptrace(PTRACE_SYSCALL, pid, 0, (void *)(uintptr_t)(sig == SIGTRAP ? 0 : sig));
            continue;
        }

        struct arm_regs regs;
        ptrace(PTRACE_GETREGS, pid, 0, &regs);
        in_sys = !in_sys;

        if (in_sys) {
            /* SYSCALL ENTRY */
            uint32_t nr = regs.uregs[7];
            uint32_t a0 = regs.uregs[0], a1 = regs.uregs[1], a2 = regs.uregs[2];
            mock_active = 0;
            poke_len = 0;
            poke2_len = 0;

            if (nr == ARM_SYS_open || nr == ARM_SYS_openat) {
                char path[256] = {0};
                uint32_t paddr = (nr == ARM_SYS_open) ? a0 : a1;
                peek_cstr(pid, paddr, path, sizeof(path));
                int vfd = map_special_path(path);
                if (vfd >= 0) {
                    int slot = vfd_slot(vfd);
                    if (slot >= 0) ms->pos[slot] = 0;
                    mock_active = 1;
                    mock_ret = vfd;
                }
            } else if (nr == ARM_SYS_close) {
                if (vfd_slot((int)a0) >= 0) {
                    mock_active = 1;
                    mock_ret = 0;
                }
            } else if (nr == ARM_SYS_fstat64) {
                int slot = vfd_slot((int)a0);
                if (slot >= 0) {
                    size_t blen = 0;
                    vfd_ro_buf(ms, (int)a0, &blen);
                    /* Minimal stat64 zeroed with st_mode=S_IFREG|0644 and st_size=blen */
                    memset(poke_buf, 0, 104);
                    uint32_t mode = 0100644;
                    memcpy(poke_buf + 16, &mode, 4);
                    uint64_t sz64 = blen;
                    memcpy(poke_buf + 48, &sz64, 8);
                    uint32_t blk = 4096;
                    memcpy(poke_buf + 56, &blk, 4);
                    poke_addr = a1;
                    poke_len = 104;
                    mock_active = 1;
                    mock_ret = 0;
                }
            } else if (nr == ARM_SYS_lseek) {
                int slot = vfd_slot((int)a0);
                if (slot >= 0) {
                    size_t blen = 0;
                    vfd_ro_buf(ms, (int)a0, &blen);
                    int32_t off = (int32_t)a1;
                    size_t np = ms->pos[slot];
                    if (a2 == SEEK_SET) np = (size_t)off;
                    else if (a2 == SEEK_CUR) np = (size_t)((int32_t)np + off);
                    else if (a2 == SEEK_END) np = (size_t)((int32_t)blen + off);
                    ms->pos[slot] = np;
                    mock_active = 1;
                    mock_ret = (int32_t)np;
                }
            } else if (nr == ARM_SYS__llseek) {
                int slot = vfd_slot((int)a0);
                if (slot >= 0) {
                    uint64_t off64 = ((uint64_t)a1 << 32) | a2;
                    uint32_t res_ptr = regs.uregs[3];
                    uint32_t whence = regs.uregs[4];
                    size_t blen = 0;
                    vfd_ro_buf(ms, (int)a0, &blen);
                    uint64_t np = ms->pos[slot];
                    if (whence == SEEK_SET) np = off64;
                    else if (whence == SEEK_CUR) np += off64;
                    else if (whence == SEEK_END) np = blen + off64;
                    ms->pos[slot] = (size_t)np;
                    poke_addr = res_ptr;
                    memcpy(poke_buf, &np, 8);
                    poke_len = 8;
                    mock_active = 1;
                    mock_ret = 0;
                }
            } else if (nr == ARM_SYS_readv) {
                int slot = vfd_slot((int)a0);
                if (slot >= 0) {
                    size_t blen = 0;
                    const uint8_t *bp = vfd_ro_buf(ms, (int)a0, &blen);
                    size_t pos = ms->pos[slot];
                    uint32_t iovcnt = a2 > 2 ? 2 : a2;
                    uint32_t iovs[4] = {0};
                    peek_bytes(pid, a1, iovs, iovcnt * 8);
                    size_t total = 0;
                    for (uint32_t k = 0; k < iovcnt; k++) {
                        uint32_t dst = iovs[k * 2];
                        uint32_t cap = iovs[k * 2 + 1];
                        size_t avail = (bp && pos < blen) ? (blen - pos) : 0;
                        size_t take = cap < avail ? cap : avail;
                        if (take > 0) {
                            poke_bytes(pid, dst, bp + pos, take);
                            pos += take;
                            total += take;
                        }
                    }
                    ms->pos[slot] = pos;
                    mock_active = 1;
                    mock_ret = (int32_t)total;
                }
            } else if (nr == ARM_SYS_writev) {
                int fd = (int)a0;
                if (fd == VFD_RESET_REASON || fd == VFD_UBI_META1 || fd == VFD_UBI_META2 || fd == VFD_WATCHDOG) {
                    uint32_t iovcnt = a2 > 4 ? 4 : a2;
                    uint32_t iovs[8] = {0};
                    peek_bytes(pid, a1, iovs, iovcnt * 8);
                    uint8_t flat[2048];
                    size_t flen = 0;
                    for (uint32_t k = 0; k < iovcnt; k++) {
                        uint32_t src = iovs[k * 2];
                        uint32_t n = iovs[k * 2 + 1];
                        if (n > sizeof(flat) - flen) n = (uint32_t)(sizeof(flat) - flen);
                        if (n > 0) peek_bytes(pid, src, flat + flen, n);
                        flen += n;
                    }
                    if (fd == VFD_RESET_REASON) {
                        size_t take = flen < 31 ? flen : 31;
                        memcpy(ms->reset_reason, flat, take);
                        ms->reset_reason[take] = '\0';
                        sb_appendf(&ms->trace, "RESET_REASON_WRITE val=%s", ms->reset_reason);
                        if (!take || ms->reset_reason[take - 1] != '\n') sb_appendf(&ms->trace, "\n");
                    } else if (fd == VFD_UBI_META1 || fd == VFD_UBI_META2) {
                        int idx = (fd == VFD_UBI_META1) ? 0 : 1;
                        uint8_t *dst = (idx == 0) ? ms->meta1 : ms->meta2;
                        size_t pos = ms->pos[vfd_slot(fd)];
                        size_t take = (pos < 1280) ? (1280 - pos) : 0;
                        if (flen < take) take = flen;
                        if (take > 0) memcpy(dst + pos, flat, take);
                        ms->pos[vfd_slot(fd)] += take;
                        sb_appendf(&ms->trace, "UBI_WRITE vol=%d armed=%d volup_bytes=%lld off=%zu len=%zu\n",
                                   idx + 1, ms->volup_armed[idx], (long long)ms->volup_bytes[idx], pos, take);
                    } else if (fd == VFD_WATCHDOG) {
                        sb_appendf(&ms->trace, "WDT_WRITE len=%zu data=", flen);
                        sb_hex(&ms->trace, flat, flen > 16 ? 16 : flen);
                        sb_appendf(&ms->trace, "\n");
                    }
                    mock_active = 1;
                    mock_ret = (int32_t)flen;
                }
            } else if (nr == ARM_SYS_read) {
                int slot = vfd_slot((int)a0);
                if (slot >= 0) {
                    size_t blen = 0;
                    const uint8_t *bp = vfd_ro_buf(ms, (int)a0, &blen);
                    size_t pos = ms->pos[slot];
                    size_t avail = (bp && pos < blen) ? (blen - pos) : 0;
                    size_t take = a2 < avail ? a2 : avail;
                    if (take > sizeof(poke_buf)) take = sizeof(poke_buf);
                    if (take > 0) {
                        memcpy(poke_buf, bp + pos, take);
                        poke_addr = a1;
                        poke_len = take;
                        ms->pos[slot] += take;
                    }
                    mock_active = 1;
                    mock_ret = (int32_t)take;
                }
            } else if (nr == ARM_SYS_write) {
                int fd = (int)a0;
                if (fd == VFD_UBI_META1 || fd == VFD_UBI_META2) {
                    int idx = (fd == VFD_UBI_META1) ? 0 : 1;
                    uint8_t *dst = (idx == 0) ? ms->meta1 : ms->meta2;
                    size_t pos = ms->pos[vfd_slot(fd)];
                    size_t take = (pos < 1280) ? (1280 - pos) : 0;
                    if (a2 < take) take = a2;
                    if (take > 0) peek_bytes(pid, a1, dst + pos, take);
                    ms->pos[vfd_slot(fd)] += take;
                    sb_appendf(&ms->trace, "UBI_WRITE vol=%d armed=%d volup_bytes=%lld off=%zu len=%zu\n",
                               idx + 1, ms->volup_armed[idx], (long long)ms->volup_bytes[idx], pos, take);
                    mock_active = 1;
                    mock_ret = (int32_t)a2;
                } else if (fd == VFD_RESET_REASON) {
                    char tmp[32] = {0};
                    size_t take = a2 < 31 ? a2 : 31;
                    peek_bytes(pid, a1, tmp, take);
                    tmp[take] = '\0';
                    strcpy(ms->reset_reason, tmp);
                    sb_appendf(&ms->trace, "RESET_REASON_WRITE val=%s", tmp);
                    if (!take || tmp[take - 1] != '\n') sb_appendf(&ms->trace, "\n");
                    mock_active = 1;
                    mock_ret = (int32_t)a2;
                } else if (fd == VFD_WATCHDOG) {
                    uint8_t tmp[16] = {0};
                    size_t take = a2 < 16 ? a2 : 16;
                    peek_bytes(pid, a1, tmp, take);
                    sb_appendf(&ms->trace, "WDT_WRITE len=%u data=", a2);
                    sb_hex(&ms->trace, tmp, take);
                    sb_appendf(&ms->trace, "\n");
                    mock_active = 1;
                    mock_ret = (int32_t)a2;
                }
            } else if (nr == ARM_SYS_socket) {
                if (a0 == 16 && a2 == 31) { /* AF_NETLINK, proto 31 */
                    mock_active = 1;
                    mock_ret = VFD_NETLINK_NV;
                } else if (a0 == 2 && (a1 & 0xf) == 2) { /* AF_INET, SOCK_DGRAM [|SOCK_CLOEXEC] */
                    mock_active = 1;
                    mock_ret = VFD_ETHSW_SOCK;
                }
            } else if (nr == ARM_SYS_bind && (int)a0 == VFD_NETLINK_NV) {
                mock_active = 1;
                mock_ret = 0;
            } else if (nr == ARM_SYS_sendto && (int)a0 == VFD_NETLINK_NV) {
                uint8_t pkt[512];
                size_t take = a2 < sizeof(pkt) ? a2 : sizeof(pkt);
                peek_bytes(pid, a1, pkt, take);
                handle_nvram_nl_tx(ms, pkt, take);
                mock_active = 1;
                mock_ret = (int32_t)a2;
            } else if (nr == ARM_SYS_sendmsg && (int)a0 == VFD_NETLINK_NV) {
                /* struct msghdr: +0x08 iov*, +0x0c iovlen */
                uint32_t mh[7] = {0};
                peek_bytes(pid, a1, mh, sizeof(mh));
                uint32_t iov[2] = {0};
                peek_bytes(pid, mh[2], iov, sizeof(iov));
                uint8_t pkt[512];
                size_t take = iov[1] < sizeof(pkt) ? iov[1] : sizeof(pkt);
                peek_bytes(pid, iov[0], pkt, take);
                handle_nvram_nl_tx(ms, pkt, take);
                mock_active = 1;
                mock_ret = (int32_t)iov[1];
            } else if ((nr == ARM_SYS_recvfrom || nr == ARM_SYS_recvmsg) && (int)a0 == VFD_NETLINK_NV) {
                uint32_t dst_addr = a1;
                size_t dst_cap = a2;
                if (nr == ARM_SYS_recvmsg) {
                    uint32_t mh[7] = {0};
                    peek_bytes(pid, a1, mh, sizeof(mh));
                    uint32_t iov[2] = {0};
                    peek_bytes(pid, mh[2], iov, sizeof(iov));
                    dst_addr = iov[0];
                    dst_cap = iov[1];
                }
                if (ms->nl_ridx < ms->nl_rcount) {
                    size_t rl = ms->nl_rlen[ms->nl_ridx];
                    if (rl > dst_cap) rl = dst_cap;
                    memcpy(poke_buf, ms->nl_reply[ms->nl_ridx], rl);
                    poke_addr = dst_addr;
                    poke_len = rl;
                    ms->nl_ridx++;
                    mock_active = 1;
                    mock_ret = (int32_t)rl;
                } else {
                    mock_active = 1;
                    mock_ret = -EAGAIN;
                }
            } else if (nr == ARM_SYS_ioctl) {
                int fd = (int)a0;
                uint32_t cmd = a1;
                if (fd == VFD_FCACHE) {
                    mock_active = 1;
                    mock_ret = 0;
                    if (cmd == 0x64) { /* STATUS */
                        uint8_t st[0x40];
                        memset(st, 0, sizeof(st));
                        /* Fill exact FcStatusInfo_t (0x40 B) offsets */
                        uint32_t u32;
                        int16_t s16;
                        uint16_t u16;
                        u32 = 10000; memcpy(st + 0x00, &u32, 4); /* flow_refresh_timer_ms */
                        s16 = 50;    memcpy(st + 0x04, &s16, 2); /* hw_activate_defer_rate */
                        s16 = 10;    memcpy(st + 0x06, &s16, 2); /* low_pkt_rate */
                        u32 = 16384; memcpy(st + 0x08, &u32, 4); /* ucast_max */
                        u32 = 100;   memcpy(st + 0x0c, &u32, 4); /* ucast_created */
                        u32 = 42;    memcpy(st + 0x10, &u32, 4); /* ucast_evicted */
                        u32 = 20;    memcpy(st + 0x14, &u32, 4); /* sw_activate_defer_count */
                        u32 = 1024;  memcpy(st + 0x18, &u32, 4); /* mcast_max */
                        u32 = 12;    memcpy(st + 0x1c, &u32, 4); /* mcast_created */
                        u32 = 3;     memcpy(st + 0x20, &u32, 4); /* mcast_evicted */
                        u32 = 256;   memcpy(st + 0x24, &u32, 4); /* uucast_max */
                        u32 = 9;     memcpy(st + 0x28, &u32, 4); /* uucast_created */
                        u32 = 4;     memcpy(st + 0x2c, &u32, 4); /* uucast_evicted */
                        u16 = 300;   memcpy(st + 0x30, &u16, 2); /* tcp_idle_s */
                        u16 = 180;   memcpy(st + 0x32, &u16, 2); /* udp_idle_s */
                        u16 = 120;   memcpy(st + 0x34, &u16, 2); /* l2_idle_s */
                        u16 = 90;    memcpy(st + 0x36, &u16, 2); /* mcast_idle_s */
                        u16 = 60;    memcpy(st + 0x38, &u16, 2); /* other_idle_s */
                        u16 = 45;    memcpy(st + 0x3a, &u16, 2); /* uucast_idle_s */
                        st[0x3c] = 0x1b;
                        st[0x3d] = 0x2d;
                        memcpy(poke_buf, st, sizeof(st));
                        poke_addr = a2;
                        poke_len = sizeof(st);
                        sb_appendf(&ms->trace, "FC_IOCTL cmd=0x%x\n", cmd);
                    } else if (cmd == 0x68) { /* FLUSH (20 B) */
                        uint8_t fl[20] = {0};
                        peek_bytes(pid, a2, fl, sizeof(fl));
                        sb_appendf(&ms->trace, "FC_IOCTL cmd=0x68 flush=");
                        sb_hex(&ms->trace, fl, sizeof(fl));
                        sb_appendf(&ms->trace, "\n");
                    } else if (cmd == 0x7b) { /* DUMP (16 B) */
                        uint32_t d[4] = {0};
                        peek_bytes(pid, a2, d, sizeof(d));
                        sb_appendf(&ms->trace, "FC_IOCTL cmd=0x7b dump=%u,%u,%u,%u\n", d[0], d[1], d[2], d[3]);
                    } else if (cmd == 0x6f) { /* QRY_CREATE (144 B) */
                        uint8_t q[0x90] = {0};
                        peek_bytes(pid, a2, q, sizeof(q));
                        uint32_t flg = 0, srcipver = 0, dstipver = 0, srcv4 = 0, dstv4 = 0;
                        uint16_t sport = 0, dport = 0, ivid = 0, ovid = 0;
                        uint8_t proto = q[0x54];
                        memcpy(&srcipver, q + 0x20, 4);
                        memcpy(&dstipver, q + 0x24, 4);
                        memcpy(&srcv4, q + 0x28, 4);
                        memcpy(&dstv4, q + 0x2c, 4);
                        memcpy(&sport, q + 0x50, 2);
                        memcpy(&dport, q + 0x52, 2);
                        memcpy(&ivid, q + 0x56, 2);
                        memcpy(&ovid, q + 0x58, 2);
                        memcpy(&flg, q + 0x88, 4);
                        sb_appendf(&ms->trace,
                                   "FC_IOCTL cmd=0x6f flags=0x%x srcv4=0x%08x dstv4=0x%08x sport=%u dport=%u proto=%u ivid=%u ovid=%u sver=%u dver=%u\n",
                                   flg, srcv4, dstv4, sport, dport, proto, ivid, ovid, srcipver, dstipver);
                        uint32_t qid = 3;
                        memcpy(q + 0x8c, &qid, 4);
                        memcpy(poke_buf, q, sizeof(q));
                        poke_addr = a2;
                        poke_len = sizeof(q);
                    } else if (cmd == 0x73) { /* QRY_NUM */
                        sb_appendf(&ms->trace, "FC_IOCTL cmd=0x73\n");
                        mock_ret = 1;
                    } else if (cmd == 0x74) { /* QRY_DUMP */
                        uint8_t dreq[0x1a0] = {0};
                        peek_bytes(pid, a2, dreq, sizeof(dreq));
                        uint32_t num = 0, eptr = 0;
                        memcpy(&num, dreq + 0x00, 4);
                        memcpy(&eptr, dreq + 0x198, 4);
                        sb_appendf(&ms->trace, "FC_IOCTL cmd=0x74 num=%u\n", num);
                        /* Zero prevSt[0..24] and populate prevSt[0] at dreq+0x08 (16B: pkts, lost, bytes) */
                        memset(dreq + 0x08, 0, 25 * 16);
                        uint32_t p0_pkts = 11, p0_lost = 2;
                        uint64_t p0_bytes = 2222ULL;
                        memcpy(dreq + 0x08, &p0_pkts, 4);
                        memcpy(dreq + 0x0c, &p0_lost, 4);
                        memcpy(dreq + 0x10, &p0_bytes, 8);
                        memcpy(poke_buf, dreq, 0x19c);
                        poke_addr = a2;
                        poke_len = 0x19c;
                        /* Populate 1 entry (168 B) at eptr */
                        if (num >= 1 && eptr != 0) {
                            uint8_t ent[168];
                            memset(ent, 0, sizeof(ent));
                            ent[0x00] = 3; /* qid */
                            ent[0x01] = 1; /* flag */
                            uint32_t pkts = 42, rtp = 1;
                            uint64_t bytes = 9999ULL;
                            memcpy(ent + 0x08, &pkts, 4);
                            memcpy(ent + 0x0c, &rtp, 4);
                            memcpy(ent + 0x10, &bytes, 8);
                            /* tuple at ent+0x18 (network-order IPv4 + network-order ports as kernel returns) */
                            uint32_t sv4 = 0x0101a8c0u; /* 192.168.1.1 in network byte order */
                            uint16_t sport = 0x5000, dport = 0xbb01; /* htons(80), htons(443) */
                            uint32_t tflg = 0x1000 | 0x01c0 | 0x4000; /* srcv4|srcport|dstport|proto|srcipver */
                            memcpy(ent + 0x18 + 0x28, &sv4, 4);
                            memcpy(ent + 0x18 + 0x50, &sport, 2);
                            memcpy(ent + 0x18 + 0x52, &dport, 2);
                            ent[0x18 + 0x54] = 6;
                            memcpy(ent + 0x18 + 0x88, &tflg, 4);
                            poke_bytes(pid, eptr, ent, sizeof(ent));
                        }
                    } else if (cmd == 0x70) { /* QRY_GET */
                        uint8_t q[0x20] = {0};
                        peek_bytes(pid, a2, q, sizeof(q));
                        uint32_t h = 0; memcpy(&h, q, 4);
                        sb_appendf(&ms->trace, "FC_IOCTL cmd=0x70 handle=%u\n", h);
                        uint32_t pkts = 123, rtp = 4;
                        uint64_t bytes = 456789ULL;
                        memcpy(q + 0x08, &pkts, 4);
                        memcpy(q + 0x0c, &rtp, 4);
                        memcpy(q + 0x10, &bytes, 8);
                        memcpy(poke_buf, q, sizeof(q));
                        poke_addr = a2;
                        poke_len = sizeof(q);
                    } else if (cmd == 0x71 || cmd == 0x72) { /* QRY_DELETE / QRY_CLEAR */
                        uint32_t h = 0;
                        peek_bytes(pid, a2, &h, 4);
                        sb_appendf(&ms->trace, "FC_IOCTL cmd=0x%x handle=%u\n", cmd, h);
                    } else if (cmd == 0x8a) { /* MCAST_WHITELIST_STATS */
                        uint64_t s[4] = { 10, 2000, 3, 450 };
                        memcpy(poke_buf, s, sizeof(s));
                        poke_addr = a2;
                        poke_len = sizeof(s);
                        sb_appendf(&ms->trace, "FC_IOCTL cmd=0x8a\n");
                    } else {
                        sb_appendf(&ms->trace, "FC_IOCTL cmd=0x%x arg=0x%x\n", cmd, a2);
                    }
                } else if (fd == VFD_BDMF) {
                    mock_active = 1;
                    mock_ret = 0;
                    if (cmd == 0x40045301) { /* INIT */
                        int32_t sid = 7;
                        memcpy(poke_buf, &sid, 4);
                        poke_addr = a2;
                        poke_len = 4;
                        sb_appendf(&ms->trace, "BDMF_IOCTL INIT\n");
                    } else if (cmd == 0x40045302) { /* CMD */
                        uint8_t req[2056];
                        peek_bytes(pid, a2, req, sizeof(req));
                        int32_t sid = 0;
                        memcpy(&sid, req, 4);
                        req[2055] = '\0';
                        sb_appendf(&ms->trace, "BDMF_IOCTL CMD sid=%d cmd=[%s]\n", sid, (char *)(req + 8));
                        int32_t rc0 = 0;
                        memcpy(poke_buf, &rc0, 4);
                        poke_addr = a2 + 4;
                        poke_len = 4;
                    } else if (cmd == 0x40045303) { /* CLOSE */
                        int32_t sid = 0;
                        peek_bytes(pid, a2, &sid, 4);
                        sb_appendf(&ms->trace, "BDMF_IOCTL CLOSE sid=%d\n", sid);
                    }
                } else if (fd == VFD_UBI_META1 || fd == VFD_UBI_META2) {
                    if (cmd == 0x40084f00) { /* UBI_IOCVOLUP = _IOW('O', 0, int64_t) */
                        int idx = (fd == VFD_UBI_META1) ? 0 : 1;
                        int64_t bytes = 0;
                        peek_bytes(pid, a2, &bytes, 8);
                        ms->volup_armed[idx] = 1;
                        ms->volup_bytes[idx] = bytes;
                        ms->pos[vfd_slot(fd)] = 0;
                        sb_appendf(&ms->trace, "UBI_IOCVOLUP vol=%d bytes=%lld\n", idx + 1, (long long)bytes);
                        mock_active = 1;
                        mock_ret = 0;
                    }
                } else if (fd == VFD_DEV_MTD2) {
                    if (cmd == 0x80204d01) { /* MEMGETINFO (32 bytes) */
                        uint8_t mi[32];
                        memset(mi, 0, sizeof(mi));
                        mi[0] = 4; /* MTD_NANDFLASH */
                        uint32_t flags = 0x400, sz = 0x7c00000, esz = 131072, wsz = 2048, oob = 64;
                        memcpy(mi + 4, &flags, 4);
                        memcpy(mi + 8, &sz, 4);
                        memcpy(mi + 12, &esz, 4);
                        memcpy(mi + 16, &wsz, 4);
                        memcpy(mi + 20, &oob, 4);
                        memcpy(poke_buf, mi, sizeof(mi));
                        poke_addr = a2;
                        poke_len = sizeof(mi);
                        mock_active = 1;
                        mock_ret = 0;
                    }
                } else if (fd == VFD_WATCHDOG) {
                    int32_t v = 0;
                    if (a2) peek_bytes(pid, a2, &v, 4);
                    sb_appendf(&ms->trace, "WDT_IOCTL cmd=0x%08x val=%d\n", cmd, v);
                    mock_active = 1;
                    mock_ret = 0;
                } else if (fd == VFD_ETHSW_SOCK) {
                    mock_active = 1;
                    mock_ret = 0;
                    if (cmd == 0x8933) { /* SIOCGIFINDEX */
                        char ifn[16] = {0};
                        peek_bytes(pid, a2, ifn, 16);
                        int32_t idx = (strcmp(ifn, "bcmsw") == 0) ? 2 : 3;
                        memcpy(poke_buf, &idx, 4);
                        poke_addr = a2 + 16;
                        poke_len = 4;
                    } else if (cmd == 0x89fd) { /* SIOCETHSWCTLOPS */
                        uint32_t data_ptr = 0;
                        peek_bytes(pid, a2 + 16, &data_ptr, 4);
                        uint8_t ed[456];
                        peek_bytes(pid, data_ptr, ed, sizeof(ed));
                        int32_t op = 0, unit = 0, port = 0, subp = 0, type = 0, p0 = 0, p1 = 0, p2 = 0, val = 0;
                        uint32_t phycfg = 0, lim = 0, bst = 0;
                        int32_t pmask = 0, queue = 0, tparam = 0;
                        memcpy(&op, ed + 0x00, 4);
                        memcpy(&unit, ed + 0x08, 4);
                        memcpy(&port, ed + 0x0c, 4);
                        memcpy(&subp, ed + 0x14, 4);
                        memcpy(&type, ed + 0x1c, 4);
                        memcpy(&p0, ed + 0x20, 4);
                        memcpy(&p1, ed + 0x24, 4);
                        memcpy(&p2, ed + 0x28, 4);
                        memcpy(&phycfg, ed + 0x34, 4);
                        memcpy(&val, ed + 0x3c, 4);
                        memcpy(&pmask, ed + 0x74, 4);
                        memcpy(&queue, ed + 0xa8, 4);
                        memcpy(&tparam, ed + 0xac, 4);
                        memcpy(&lim, ed + 0xb4, 4);
                        memcpy(&bst, ed + 0xb8, 4);
                        /* Log all defined fields per op (stock leaves unused stack fields uninitialized) */
                        if (op == 14) {
                            if (type == 3)
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d lim=%u bst=%u\n",
                                           op, type, unit, port, lim, bst);
                            else
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d\n",
                                           op, type, unit, port);
                        } else if (op == 15) {
                            if (type == 3)
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d q=%d lim=%u bst=%u\n",
                                           op, type, unit, port, queue, lim, bst);
                            else
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d q=%d\n",
                                           op, type, unit, port, queue);
                        } else if (op == 25 && type == 3) {
                            sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d speed=%d duplex=%d phycfg=0x%x\n",
                                       op, type, unit, port, p0, p1, phycfg);
                        } else if (op == 22) {
                            sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d speed=%d duplex=%d\n",
                                       op, type, unit, port, p0, p1);
                        } else if (op == 3) {
                            sb_appendf(&ms->trace, "ETHSW op=%d val=%d\n", op, val);
                        } else if ((op == 7 || op == 23) && type == 3) {
                            sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d val=%d\n",
                                       op, type, unit, port, val);
                        } else if (op == 18) {
                            if (type == 3)
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d mask=%d val=%d\n",
                                           op, type, unit, port, pmask, val);
                            else
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d mask=%d\n",
                                           op, type, unit, port, pmask);
                        } else if (op == 11) {
                            uint16_t m16 = 0, sp16 = 0;
                            int32_t w[4] = {0};
                            memcpy(&m16, ed + 0x20, 2);
                            memcpy(&sp16, ed + 0x22, 2);
                            memcpy(w, ed + 0x48, 16);
                            if (type == 3)
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d mode=%u sp=%u w=%d,%d,%d,%d\n",
                                           op, type, unit, port, m16, sp16, w[0], w[1], w[2], w[3]);
                            else
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d\n",
                                           op, type, unit, port);
                        } else if (op == 16) {
                            if (type == 3)
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d ttype=%d tparam=%d val=%d\n",
                                           op, type, unit, p0, (p0 == 0) ? tparam : 0, val);
                            else
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d ttype=%d tparam=%d\n",
                                           op, type, unit, p0, (p0 == 0 || p0 == 3) ? tparam : 0);
                        } else if (op == 32) {
                            char ifn[16] = {0};
                            memcpy(ifn, ed + 0xf0, 15);
                            sb_appendf(&ms->trace, "ETHSW op=%d type=%d ifname=%s\n", op, type, ifn);
                        } else if (op == 33) {
                            if (type == 3) {
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d dos=", op, type, unit);
                                sb_hex(&ms->trace, ed + 0x20, 15);
                                sb_appendf(&ms->trace, "\n");
                            } else {
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d\n", op, type, unit);
                            }
                        } else if (op == 34) {
                            if (type == 3)
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d val=0x%x\n", op, type, unit, (unsigned)p0);
                            else
                                sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d buflen=%u\n", op, type, unit, (unsigned)p1);
                        } else if (op == 58) {
                            sb_appendf(&ms->trace, "ETHSW op=%d type=%d oam=%d\n", op, type, p1);
                        } else if (op == 1 || op == 2) {
                            char ifn[16] = {0};
                            memcpy(ifn, ed + 0x30, 15);
                            if (op == 1)
                                sb_appendf(&ms->trace, "ETHSW op=%d port=%d wan=%d ifname=%s\n", op, p0, p2, ifn);
                            else
                                sb_appendf(&ms->trace, "ETHSW op=%d port=%d\n", op, p0);
                        } else if (op == 62) {
                            char i1[16] = {0}, i2[16] = {0};
                            int32_t elen = 0;
                            memcpy(i1, ed + 0x20, 15);
                            memcpy(i2, ed + 0x30, 15);
                            memcpy(&elen, ed + 0x48, 4);
                            sb_appendf(&ms->trace, "ETHSW op=%d type=%d i1=%s i2=%s elen=%d\n", op, type, i1, i2, elen);
                        } else if (op == 24) {
                            int32_t aflag = 0;
                            memcpy(&aflag, ed + 0x18, 4);
                            sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d sub=%d af=%d val=%d spd=%d\n",
                                       op, type, unit, port, subp, aflag, (type == 3) ? val : 0, (type == 3) ? p2 : 0);
                        } else {
                            sb_appendf(&ms->trace, "ETHSW op=%d type=%d unit=%d port=%d\n",
                                       op, type, unit, port);
                        }
                        /* Populate deterministic mock GET replies */
                        if (op == 4 && type == 2) { /* hw-switching get */
                            int32_t st = 0, rv = 0;
                            memcpy(ed + 0x24, &st, 4);
                            memcpy(ed + 0x38, &rv, 4);
                        } else if (op == 7 && type == 2) { /* pause get */
                            int32_t v8 = 2; memcpy(ed + 0x20, &v8, 4);
                        } else if (op == 11 && type == 2) { /* cosqsched get */
                            uint16_t m16 = 2, sp16 = 2;
                            int32_t w[4] = { 1, 2, 4, 8 };
                            memcpy(ed + 0x20, &m16, 2);
                            memcpy(ed + 0x22, &sp16, 2);
                            memcpy(ed + 0x48, w, sizeof(w));
                        } else if (op == 14 && type == 2) { /* rxrate get */
                            uint32_t l = 10000, b = 512;
                            memcpy(ed + 0xb4, &l, 4);
                            memcpy(ed + 0xb8, &b, 4);
                        } else if (op == 15 && type == 2) { /* txrate get */
                            uint32_t l = 20000, b = 1024;
                            memcpy(ed + 0xb4, &l, 4);
                            memcpy(ed + 0xb8, &b, 4);
                        } else if (op == 16 && type == 2) { /* test get */
                            int32_t rv = 77; memcpy(ed + 0x38, &rv, 4);
                        } else if (op == 18 && type == 2) { /* cosqpriomethod get */
                            int32_t rv = 2; memcpy(ed + 0x38, &rv, 4);
                        } else if (op == 23 && type == 2) { /* portctrl get */
                            int32_t rv = 0; memcpy(ed + 0x38, &rv, 4);
                        } else if (op == 24 && type == 2) { /* portloopback get */
                            int32_t rv = 1, spd = 1000;
                            memcpy(ed + 0x38, &rv, 4);
                            memcpy(ed + 0x28, &spd, 4);
                        } else if (op == 25 && type == 2) { /* phymode get */
                            if (port == 3) {
                                /* Link-down port returns ioctl==0 and ret_val==-1 */
                                int32_t sp = 0, dp = 0, rv = -1;
                                memcpy(ed + 0x20, &sp, 4);
                                memcpy(ed + 0x24, &dp, 4);
                                memcpy(ed + 0x38, &rv, 4);
                            } else {
                                int32_t sp = 1000, dp = 1, rv = 0;
                                memcpy(ed + 0x20, &sp, 4);
                                memcpy(ed + 0x24, &dp, 4);
                                memcpy(ed + 0x38, &rv, 4);
                            }
                        } else if (op == 30 && type == 2) { /* ifname get */
                            strcpy((char *)(ed + 0xf0), "eth0");
                            int32_t rv = 0; memcpy(ed + 0x38, &rv, 4);
                        } else if (op == 32 && type == 2) { /* unit_port get */
                            int32_t u = 0; uint32_t pm = 0x04;
                            memcpy(ed + 0x08, &u, 4);
                            memcpy(ed + 0xe0, &pm, 4);
                        } else if (op == 33 && type == 2) { /* dosctrl get */
                            for (int k = 0; k < 15; k++) ed[0x20 + k] = (uint8_t)(k & 1);
                        } else if (op == 34 && (type == 2 || type == 7)) { /* snoopctrl get/help */
                            uint32_t sptr = 0;
                            memcpy(&sptr, ed + 0x28, 4);
                            int32_t sv = 0x15;
                            memcpy(ed + 0x20, &sv, 4);
                            const char *msg = (type == 7) ? "SNOOP_HELP_TEXT\n" : "IGMP=1 MLD=1";
                            if (sptr) poke_bytes(pid, sptr, msg, strlen(msg) + 1);
                        } else if (op == 36 && type == 2) { /* softswitch get */
                            int32_t pm = 0x00; memcpy(ed + 0x24, &pm, 4);
                        } else if (op == 58 && type == 2) { /* oamindex get */
                            int32_t u = 0, pt = 2;
                            memcpy(ed + 0x08, &u, 4);
                            memcpy(ed + 0x0c, &pt, 4);
                        } else if (op == 1) { /* createport returns kernel name */
                            strcpy((char *)(ed + 0x30), "eth4");
                        } else if (op == 62) { /* bondchk */
                            char i2[16] = {0};
                            memcpy(i2, ed + 0x30, 15);
                            int32_t st = (strcmp(i2, "badif") == 0) ? 1 : 0;
                            memcpy(ed + 0x48, &st, 4);
                            if (st != 0) {
                                uint32_t eptr = 0;
                                memcpy(&eptr, ed + 0x40, 4);
                                const char *emsg = "ports cannot bond";
                                if (eptr) poke_bytes(pid, eptr, emsg, strlen(emsg) + 1);
                            }
                        }
                        memcpy(poke_buf, ed, sizeof(ed));
                        poke_addr = data_ptr;
                        poke_len = sizeof(ed);
                    } else if (cmd == 0x89f0) { /* SIOCDEVPRIVATE (wl) */
                        handle_wl_ioctl(pid, ms, a2, poke_buf, &poke_addr, &poke_len, &mock_ret);
                    } else if (cmd == 0x89f2) { /* SIOCSCLEARMIBCNTR */
                        sb_appendf(&ms->trace, "ETHSW_CLEAR_MIB\n");
                    } else if (cmd == 0x89f7) { /* SIOCGQUERYNUMPORTS */
                        char ifn[16] = {0};
                        uint32_t pm_ptr = 0;
                        peek_bytes(pid, a2, ifn, 16);
                        peek_bytes(pid, a2 + 16, &pm_ptr, 4);
                        sb_appendf(&ms->trace, "ETHSW_QNUMPORTS if=%s\n", ifn);
                        int32_t pm = 0x04; /* single subport 2 */
                        if (pm_ptr) {
                            memcpy(poke_buf, &pm, 4);
                            poke_addr = pm_ptr;
                            poke_len = 4;
                        }
                    } else if (cmd == 0x89fb) { /* SIOCETHCTLOPS */
                        uint32_t dptr = 0;
                        peek_bytes(pid, a2 + 16, &dptr, 4);
                        uint8_t ec[116] = {0};
                        peek_bytes(pid, dptr, ec, sizeof(ec));
                        int32_t op = 0, sp = 0;
                        memcpy(&op, ec + 0x00, 4);
                        memcpy(&sp, ec + 0x18, 4);
                        sb_appendf(&ms->trace, "ETHSW_ETHCTL op=%d sub=%d\n", op, sp);
                        int32_t phyid = 5;
                        memcpy(ec + 0x1c, &phyid, 4);
                        memcpy(poke_buf, ec, sizeof(ec));
                        poke_addr = dptr;
                        poke_len = sizeof(ec);
                    } else if (cmd == 0x89ff) { /* SIOCIFREQ_EXT */
                        uint32_t ext_ptr = 0;
                        peek_bytes(pid, a2 + 16, &ext_ptr, 4);
                        uint32_t ext[6] = {0};
                        peek_bytes(pid, ext_ptr, ext, sizeof(ext));
                        sb_appendf(&ms->trace, "ETHSW_EXT opcode=%u bufLen=%u\n", ext[0], ext[4]);
                        const char *ans = (ext[0] == 2) ? "eth0,eth1" : "";
                        poke_addr = ext[2];
                        poke_len = strlen(ans) + 1;
                        memcpy(poke_buf, ans, poke_len);
                    } else if (cmd == 0x89f9) { /* SIOCSWANPORT */
                        char ifn[16] = {0};
                        int32_t en = 0;
                        peek_bytes(pid, a2, ifn, 16);
                        peek_bytes(pid, a2 + 16, &en, 4);
                        sb_appendf(&ms->trace, "ETHSW_WAN_SET if=%s en=%d\n", ifn, en);
                    } else if (cmd == 0x89fa) { /* SIOCGWANPORT */
                        uint32_t data_ptr = 0;
                        peek_bytes(pid, a2 + 16, &data_ptr, 4);
                        uint8_t we[64] = {0};
                        peek_bytes(pid, data_ptr, we, sizeof(we));
                        uint32_t nptr = 0, nlen = 0;
                        memcpy(&nptr, we + 0x20, 4);
                        memcpy(&nlen, we + 0x28, 4);
                        sb_appendf(&ms->trace, "ETHSW_WAN_GET len=%u\n", nlen);
                        const char *wn = "eth0";
                        if (nptr) poke_bytes(pid, nptr, wn, strlen(wn) + 1);
                    } else {
                        sb_appendf(&ms->trace, "SOCK_IOCTL cmd=0x%04x\n", cmd);
                    }
                }
            }

            if (mock_active) {
                ptrace(PTRACE_SET_SYSCALL, pid, 0, (void *)(uintptr_t)-1);
            }
        } else {
            /* SYSCALL EXIT */
            if (mock_active) {
                if (poke_len > 0) poke_bytes(pid, poke_addr, poke_buf, poke_len);
                if (poke2_len > 0) poke_bytes(pid, poke2_addr, poke2_buf, poke2_len);
                regs.uregs[0] = (uint32_t)mock_ret;
                ptrace(PTRACE_SETREGS, pid, 0, &regs);
                mock_active = 0;
            }
        }

        ptrace(PTRACE_SYSCALL, pid, 0, 0);
    }

    drain_fd(out_p[0], &res->out);
    drain_fd(err_p[0], &res->err);
    close(out_p[0]);
    close(err_p[0]);
    res->trace = ms->trace;
    memcpy(res->meta1_after, ms->meta1, 1280);
    memcpy(res->meta2_after, ms->meta2, 1280);
    strcpy(res->reset_reason_after, ms->reset_reason);
    return 0;
}

typedef struct {
    const char *name;
    const char *tool;
    const char *args[20];
    int         check_stdout; /* 1=strict, 0=skip volatile/format-only */
} test_case_t;

static const test_case_t TESTS[] = {
    /* fcctl — read AND write ioctls (ALL config stdout strictly checked!) */
    { "fcctl status",              "fcctl", { "status", NULL }, 1 },
    { "fcctl enable",              "fcctl", { "enable", NULL }, 1 },
    { "fcctl disable",             "fcctl", { "disable", NULL }, 1 },
    { "fcctl resetstats",          "fcctl", { "resetstats", NULL }, 1 },
    { "fcctl flush all",           "fcctl", { "flush", NULL }, 1 },
    { "fcctl flush --hw alone",    "fcctl", { "flush", "--hw", NULL }, 1 },
    { "fcctl flush --silent",      "fcctl", { "flush", "--silent", NULL }, 1 },
    { "fcctl flush hw+flow+mac",   "fcctl", { "flush", "--hw", "--flow", "42", "--mac", "aa:bb:cc:dd:ee:ff", NULL }, 1 },
    { "fcctl flush bad flow 0",    "fcctl", { "flush", "--flow", "0", NULL }, 1 },
    { "fcctl config hw-accel 1",   "fcctl", { "config", "--hw-accel", "1", NULL }, 1 },
    { "fcctl config hw-accel 2",   "fcctl", { "config", "--hw-accel", "2", NULL }, 1 },
    { "fcctl config tos-mflows",   "fcctl", { "config", "--tos-mflows", "1", NULL }, 1 },
    { "fcctl config idle-uucast",  "fcctl", { "config", "--idle-timer-unknown-ucast", "60", NULL }, 1 },
    { "fcctl config idle-other",   "fcctl", { "config", "--idle-timer-other", "90", NULL }, 1 },
    { "fcctl config idle-tcp",     "fcctl", { "config", "--idle-timer-tcp", "300", NULL }, 1 },
    { "fcctl config l2tp clamp",   "fcctl", { "config", "--l2tp", "5", NULL }, 1 },
    { "fcctl config sw-defer oor", "fcctl", { "config", "--sw-defer", "70000", NULL }, 1 },
    { "fcctl config test 0",       "fcctl", { "config", "--test", "0", NULL }, 1 },
    { "fcctl config test 1 2 3",   "fcctl", { "config", "--test", "1", "2", "3", NULL }, 1 },
    { "fcctl chained cmds",        "fcctl", { "disable", "flush", "enable", NULL }, 1 },
    { "fcctl dump flow 5",         "fcctl", { "dump", "--flow", "5", NULL }, 1 },
    { "fcctl dump blog 5",         "fcctl", { "dump", "--blog", "5", NULL }, 1 },
    { "fcctl dump mcast-bmp",      "fcctl", { "dump", "--mcast-bitmap", NULL }, 1 },
    { "fcctl dump mcast-bmp 123",  "fcctl", { "dump", "--mcast-bitmap", "1", "2", "3", NULL }, 1 },
    { "fcctl dump uucast-bmp",     "fcctl", { "dump", "--unknown-ucast-bitmap", NULL }, 1 },
    { "fcctl flwstats n",          "fcctl", { "flwstats", "n", "srcv4", "192.168.1.1", "dstv4", "10.0.0.1", "srcport", "80", "dstport", "443", "proto", "6", "srcipver", "v4", "dstipver", "v6", NULL }, 1 },
    { "fcctl flwstats tos+dscp",   "fcctl", { "flwstats", "n", "txtos", "1", "txdscp", "2", NULL }, 1 },
    { "fcctl flwstats p (num=1)",  "fcctl", { "flwstats", "p", NULL }, 1 },
    { "fcctl flwstats g 1",        "fcctl", { "flwstats", "g", "1", NULL }, 1 },
    { "fcctl flwstats d all",      "fcctl", { "flwstats", "d", "all", NULL }, 1 },
    { "fcctl flwstats c 1",        "fcctl", { "flwstats", "c", "1", NULL }, 1 },
    { "fcctl mcaststats",          "fcctl", { "mcaststats", NULL }, 1 },
    { "fcctl invalid",             "fcctl", { "nonexistent_cmd", NULL }, 0 },

    /* nvram — Netlink 31 GET (real namevalue_buf!) / SET / UNSET / GETALL */
    { "nvram kget boardid",        "nvram", { "kget", "boardid", NULL }, 1 },
    { "nvram kget missing",        "nvram", { "kget", "__missing__", NULL }, 1 },
    { "nvram kset foo=bar",        "nvram", { "kset", "foo=bar", NULL }, 1 },
    { "nvram kset foo (unset)",    "nvram", { "kset", "foo", NULL }, 1 },
    { "nvram kunset foo",          "nvram", { "kunset", "foo", NULL }, 1 },
    { "nvram kshow",               "nvram", { "kshow", NULL }, 1 },

    /* bcm_flashutil — bootfs1/bootfs2 ident scan */
    { "bcm_flashutil help",        "bcm_flashutil", { NULL }, 0 },
    { "bcm_flashutil imgver 2",    "bcm_flashutil", { "2", "-1", "-1", "imageversion", NULL }, 1 },
    { "bcm_flashutil imgver 1",    "bcm_flashutil", { "1", "-1", "-1", "imageversion", NULL }, 1 },

    /* bcm_bootstate — read (strict stdout after stripping rd_metadata) & invalid */
    { "bcm_bootstate read",        "bcm_bootstate", { NULL }, 1 },
    { "bcm_bootstate invalid",     "bcm_bootstate", { "999", NULL }, 1 },

    /* bdmf_shell — serial ioctls init / cmd / close / errors */
    { "bdmf_shell no-args",        "bdmf_shell", { NULL }, 1 },
    { "bdmf_shell -c init",        "bdmf_shell", { "-c", "init", NULL }, 1 },
    { "bdmf_shell -c 7 -cmd",      "bdmf_shell", { "-c", "7", "-cmd", "/b/e", NULL }, 1 },
    { "bdmf_shell -c 7 -close",    "bdmf_shell", { "-c", "7", "-close", NULL }, 1 },
    { "bdmf_shell -c 7 -f (miss)", "bdmf_shell", { "-c", "7", "-f", NULL }, 1 },

    /* wdtctl — usage, timeleft, start, ping, stop, invalid options */
    { "wdtctl usage",              "wdtctl", { NULL }, 1 },
    { "wdtctl timeleft",           "wdtctl", { "timeleft", NULL }, 1 },
    { "wdtctl -t 60 start",        "wdtctl", { "-t", "60", "start", NULL }, 1 },
    { "wdtctl ping",               "wdtctl", { "ping", NULL }, 1 },
    { "wdtctl stop",               "wdtctl", { "stop", NULL }, 1 },
    { "wdtctl bad timeout",        "wdtctl", { "-t", "999", "start", NULL }, 1 },
    { "wdtctl -t 10 stop reject",  "wdtctl", { "-t", "10", "stop", NULL }, 1 },
    { "wdtctl -p 99 start reject", "wdtctl", { "-p", "99", "start", NULL }, 1 },

    /* ethswctl — comprehensive read AND write ioctls covering every fixed bug */
    { "ethswctl help",             "ethswctl", { "-h", NULL }, 1 },
    { "ethswctl hw-switching get", "ethswctl", { "-c", "hw-switching", NULL }, 1 },
    { "ethswctl hw-switching en",  "ethswctl", { "-c", "hw-switching", "-o", "enable", NULL }, 1 },
    { "ethswctl hw-switching dis", "ethswctl", { "-c", "hw-switching", "-o", "disable", NULL }, 1 },
    { "ethswctl softswitch get",   "ethswctl", { "-c", "softswitch", NULL }, 1 },
    { "ethswctl softswitch set",   "ethswctl", { "-c", "softswitch", "-i", "eth0", "-o", "enable", NULL }, 1 },
    { "ethswctl mibdump 0 1 -a",   "ethswctl", { "-c", "mibdump", "-n", "0", "-p", "1", "-a", NULL }, 1 },
    { "ethswctl mibdump no-port",  "ethswctl", { "-c", "mibdump", NULL }, 1 },
    { "ethswctl pause get 0 0",    "ethswctl", { "-c", "pause", "-n", "0", "-p", "0", NULL }, 1 },
    { "ethswctl pause set 0 0 2",  "ethswctl", { "-c", "pause", "-n", "0", "-p", "0", "-v", "2", NULL }, 1 },
    { "ethswctl portctrl get 0 0", "ethswctl", { "-c", "portctrl", "-n", "0", "-p", "0", NULL }, 1 },
    { "ethswctl portctrl set",     "ethswctl", { "-c", "portctrl", "-n", "0", "-p", "0", "-v", "3", NULL }, 1 },
    { "ethswctl portloopback get", "ethswctl", { "-c", "portloopback", "-n", "0", "-p", "0", NULL }, 1 },
    { "ethswctl portloopback set", "ethswctl", { "-c", "portloopback", "-n", "0", "-p", "0", "-v", "1", "-s", "1000", NULL }, 1 },
    { "ethswctl phymode get 0 0",  "ethswctl", { "-c", "phymode", "-n", "0", "-p", "0", NULL }, 1 },
    { "ethswctl phymode linkdown", "ethswctl", { "-c", "phymode", "-n", "0", "-p", "3", NULL }, 1 },
    { "ethswctl phymode set 1G",   "ethswctl", { "-c", "phymode", "-n", "0", "-p", "0", "-y", "1000", "-z", "1", NULL }, 1 },
    { "ethswctl phymode set 100H", "ethswctl", { "-c", "phymode", "-n", "0", "-p", "0", "-y", "100", "-z", "0", NULL }, 1 },
    { "ethswctl setlinkstatus",    "ethswctl", { "-c", "setlinkstatus", "-n", "0", "-p", "1", "-x", "1", "-y", "1000", "-z", "1", NULL }, 1 },
    { "ethswctl dosctrl get",      "ethswctl", { "-c", "dosctrl", "-n", "0", NULL }, 1 },
    { "ethswctl dosctrl set",      "ethswctl", { "-c", "dosctrl", "-n", "0", "-v", "0x55", NULL }, 1 },
    { "ethswctl snoopctrl get",    "ethswctl", { "-c", "snoopctrl", "-n", "0", NULL }, 1 },
    { "ethswctl snoopctrl set",    "ethswctl", { "-c", "snoopctrl", "-n", "0", "-v", "0x0f", NULL }, 1 },
    { "ethswctl snoopctrl -h",     "ethswctl", { "-c", "snoopctrl", "-n", "0", "-h", NULL }, 1 },
    { "ethswctl getifname 0 0",    "ethswctl", { "getifname", "0", "0", NULL }, 1 },
    { "ethswctl getwanonly",       "ethswctl", { "-c", "getwanonly", NULL }, 1 },
    { "ethswctl getwanpreferred",  "ethswctl", { "-c", "getwanpreferred", NULL }, 1 },
    { "ethswctl getlanonly",       "ethswctl", { "-c", "getlanonly", NULL }, 1 },
    { "ethswctl getlanwan",        "ethswctl", { "-c", "getlanwan", NULL }, 1 },
    { "ethswctl wan get",          "ethswctl", { "-c", "wan", NULL }, 1 },
    { "ethswctl wan set en",       "ethswctl", { "-c", "wan", "-i", "eth0", "-o", "enable", NULL }, 1 },
    { "ethswctl rxratectrl get",   "ethswctl", { "-c", "rxratectrl", "-n", "0", "-p", "0", NULL }, 1 },
    { "ethswctl rxratectrl set",   "ethswctl", { "-c", "rxratectrl", "-n", "0", "-p", "0", "-x", "5000", "-y", "256", NULL }, 1 },
    { "ethswctl txratectrl get",   "ethswctl", { "-c", "txratectrl", "-n", "0", "-p", "0", NULL }, 1 },
    { "ethswctl txratectrl set",   "ethswctl", { "-c", "txratectrl", "-n", "0", "-p", "0", "-x", "5000", "-y", "256", NULL }, 1 },
    { "ethswctl cosqpriomethod g", "ethswctl", { "-c", "cosqpriomethod", "-n", "0", "-p", "1", NULL }, 1 },
    { "ethswctl cosqpriomethod s", "ethswctl", { "-c", "cosqpriomethod", "-n", "0", "-p", "1", "-v", "2", NULL }, 1 },
    { "ethswctl cosqsched get",    "ethswctl", { "-c", "cosqsched", "-n", "0", NULL }, 1 },
    { "ethswctl cosqsched set",    "ethswctl", { "-c", "cosqsched", "-n", "0", "-v", "2", "-t", "2", "-w", "1", "-x", "2", "-y", "4", "-z", "8", NULL }, 1 },
    { "ethswctl oamindex",         "ethswctl", { "-c", "oamindex", "-v", "5", "-t", "0", NULL }, 1 },
    { "ethswctl createport",       "ethswctl", { "-c", "createport", "-p", "4", "-w", "1", "-i", "eth4", NULL }, 1 },
    { "ethswctl deleteport",       "ethswctl", { "-c", "deleteport", "-p", "4", NULL }, 1 },
    { "ethswctl test -t 0 -x 1",   "ethswctl", { "-c", "test", "-t", "0", "-x", "1", NULL }, 1 },
    { "ethswctl test -t 2 clear",  "ethswctl", { "-c", "test", "-t", "2", NULL }, 1 },
    { "ethswctl bondchk ok",       "ethswctl", { "-c", "bondchk", "-i", "eth0", "-j", "eth1", NULL }, 1 },
    { "ethswctl bondchk err",      "ethswctl", { "-c", "bondchk", "-i", "eth0", "-j", "badif", NULL }, 1 },
    { "ethswctl gponmcastgemset",  "ethswctl", { "-c", "gponmcastgemset", "-p", "12", NULL }, 1 },

    /* wl — T1 & T2 READ + WRITE wire-ABI and stdout verification */
    { "wl up",                     "wl", { "-i", "wl1", "up", NULL }, 1 },
    { "wl down",                   "wl", { "-i", "wl1", "down", NULL }, 1 },
    { "wl isup",                   "wl", { "-i", "wl1", "isup", NULL }, 1 },
    { "wl ap get",                 "wl", { "-i", "wl1", "ap", NULL }, 1 },
    { "wl ap 1",                   "wl", { "-i", "wl1", "ap", "1", NULL }, 1 },
    { "wl frameburst get",         "wl", { "-i", "wl1", "frameburst", NULL }, 1 },
    { "wl frameburst 1",           "wl", { "-i", "wl1", "frameburst", "1", NULL }, 1 },
    { "wl txpwr_percent get",      "wl", { "-i", "wl1", "txpwr_percent", NULL }, 1 },
    { "wl txpwr_percent 80",       "wl", { "-i", "wl1", "txpwr_percent", "80", NULL }, 1 },
    { "wl wme_apsd get",           "wl", { "-i", "wl1", "wme_apsd", NULL }, 1 },
    { "wl wme_apsd 1",             "wl", { "-i", "wl1", "wme_apsd", "1", NULL }, 1 },
    { "wl phy_tempsense",          "wl", { "-i", "wl1", "phy_tempsense", NULL }, 1 },
    { "wl assoc_rssi_th -75",      "wl", { "-i", "wl1", "assoc_rssi_th", "-75", NULL }, 1 },
    { "wl cur_etheraddr get",      "wl", { "-i", "wl1", "cur_etheraddr", NULL }, 1 },
    { "wl cur_etheraddr set",      "wl", { "-i", "wl1", "cur_etheraddr", "98:ee:ca:15:57:48", NULL }, 1 },
    { "wl country get",            "wl", { "-i", "wl1", "country", NULL }, 1 },
    { "wl country CN/947",         "wl", { "-i", "wl1", "country", "CN/947", NULL }, 1 },
    { "wl spect get",              "wl", { "-i", "wl1", "spect", NULL }, 1 },
    { "wl spect 0",                "wl", { "-i", "wl1", "spect", "0", NULL }, 1 },
    { "wl bw_cap 5g",              "wl", { "-i", "wl1", "bw_cap", "5g", NULL }, 1 },
    { "wl bw_cap 5g 0xf",          "wl", { "-i", "wl1", "bw_cap", "5g", "0xf", NULL }, 1 },
    { "wl chanspec get",           "wl", { "-i", "wl1", "chanspec", NULL }, 1 },
    { "wl chanspec 48/160",        "wl", { "-i", "wl1", "chanspec", "48/160", NULL }, 1 },
    { "wl he get",                 "wl", { "-i", "wl1", "he", NULL }, 1 },
    { "wl he 1",                   "wl", { "-i", "wl1", "he", "1", NULL }, 1 },
    { "wl he 0",                   "wl", { "-i", "wl1", "he", "0", NULL }, 1 },
    { "wl twt get",                "wl", { "-i", "wl1", "twt", NULL }, 1 },
    { "wl twt 1",                  "wl", { "-i", "wl1", "twt", "1", NULL }, 1 },
    { "wl txpwr1 get",             "wl", { "-i", "wl1", "txpwr1", NULL }, 1 },
    { "wl txpwr1 -d 20",           "wl", { "-i", "wl1", "txpwr1", "-d", "20", NULL }, 1 },
    { "wl wme_ac ap get",          "wl", { "-i", "wl1", "wme_ac", "ap", NULL }, 1 },
    { "wl wme_ac ap be set",       "wl", { "-i", "wl1", "wme_ac", "ap", "be", "aifsn", "2", "ecwmin", "3", "ecwmax", "5", "txop", "47", NULL }, 1 },
    { "wl assoclist",              "wl", { "-i", "wl1", "assoclist", NULL }, 1 },
    { "wl rssi mac",               "wl", { "-i", "wl1", "rssi", "ce:68:6d:ce:f4:ca", NULL }, 1 },
    { "wl csimon enable",          "wl", { "-i", "wl1", "csimon", "enable", NULL }, 1 },
    { "wl csimon disable",         "wl", { "-i", "wl1", "csimon", "disable", NULL }, 1 },
    { "wl csimon add",             "wl", { "-i", "wl1", "csimon", "add", "aa:bb:cc:dd:ee:ff", "100", "enable", NULL }, 1 },
    { "wl csimon del",             "wl", { "-i", "wl1", "csimon", "del", "aa:bb:cc:dd:ee:ff", NULL }, 1 },
    { "wl sta_monitor enable",     "wl", { "-i", "wl1", "sta_monitor", "enable", NULL }, 1 },
    { "wl sta_monitor disable",    "wl", { "-i", "wl1", "sta_monitor", "disable", NULL }, 1 },
    { "wl sta_monitor add",        "wl", { "-i", "wl1", "sta_monitor", "add", "aa:bb:cc:dd:ee:ff", NULL }, 1 },
    { "wl sta_monitor del",        "wl", { "-i", "wl1", "sta_monitor", "del", "aa:bb:cc:dd:ee:ff", NULL }, 1 },
    { "wl sta_monitor dump",       "wl", { "-i", "wl1", "sta_monitor", "dump", NULL }, 1 },
    { "wl dump csimon",            "wl", { "-i", "wl1", "dump", "csimon", NULL }, 1 },
    { "wl chanim_stats",           "wl", { "-i", "wl1", "chanim_stats", NULL }, 1 },
    { "wl ver",                    "wl", { "-i", "wl1", "ver", NULL }, 1 },
    { "wl revinfo",                "wl", { "-i", "wl1", "revinfo", NULL }, 1 },
    { "wl cap",                    "wl", { "-i", "wl1", "cap", NULL }, 1 },
    { "wl band",                   "wl", { "-i", "wl1", "band", NULL }, 1 },
    { "wl bands",                  "wl", { "-i", "wl1", "bands", NULL }, 1 },
    { "wl noise",                  "wl", { "-i", "wl1", "noise", NULL }, 1 },
    { "wl channel",                "wl", { "-i", "wl1", "channel", NULL }, 1 },
    { "wl channels",               "wl", { "-i", "wl1", "channels", NULL }, 1 },
    { "wl radio",                  "wl", { "-i", "wl1", "radio", NULL }, 1 },
    { "wl bss",                    "wl", { "-i", "wl1", "bss", NULL }, 1 },
    { "wl bssid",                  "wl", { "-i", "wl1", "bssid", NULL }, 1 },
    { "wl ssid",                   "wl", { "-i", "wl1", "ssid", NULL }, 1 },
    { "wl deauth mac",             "wl", { "-i", "wl1", "deauthenticate", "aa:bb:cc:dd:ee:ff", NULL }, 1 },
    { "wl deauth mac 3",           "wl", { "-i", "wl1", "deauthenticate", "aa:bb:cc:dd:ee:ff", "3", NULL }, 1 },
    { "wl csa",                    "wl", { "-i", "wl1", "csa", "1", "5", "48/160", NULL }, 1 },
    { "wl pktcnt",                 "wl", { "-i", "wl1", "pktcnt", NULL }, 1 },
    { "wl chanim_mode",            "wl", { "-i", "wl1", "chanim_mode", NULL }, 1 },
    { "wl dfs_status",             "wl", { "-i", "wl1", "dfs_status", NULL }, 1 },
    { "wl wme_counters",           "wl", { "-i", "wl1", "wme_counters", NULL }, 1 },
    { "wl counters",               "wl", { "-i", "wl1", "counters", NULL }, 0 },
    { "wl sta_info",               "wl", { "-i", "wl1", "sta_info", "ce:68:6d:ce:f4:ca", NULL }, 0 },
    /* Regression tests for EVERY C1-C5 & H1-H13 audit fix */
    { "wl scan default",           "wl", { "-i", "wl1", "scan", NULL }, 1 },
    { "wl scan -c 36 -t passive",  "wl", { "-i", "wl1", "scan", "-c", "36", "-t", "passive", NULL }, 1 },
    { "wl chanim_stats all",       "wl", { "-i", "wl1", "chanim_stats", "all", NULL }, 1 },
    { "wl txpwr1 -1",              "wl", { "-i", "wl1", "txpwr1", "-1", NULL }, 1 },
    { "wl txpwr1 -o -1",           "wl", { "-i", "wl1", "txpwr1", "-o", "-1", NULL }, 1 },
    { "wl txpwr1 -d -1",           "wl", { "-i", "wl1", "txpwr1", "-d", "-1", NULL }, 1 },
    { "wl txpwr1 -q -1",           "wl", { "-i", "wl1", "txpwr1", "-q", "-1", NULL }, 1 },
    { "wl bss up",                 "wl", { "-i", "wl1", "bss", "up", NULL }, 1 },
    { "wl bss down",               "wl", { "-i", "wl1", "bss", "down", NULL }, 1 },
    { "wl bss del",                "wl", { "-i", "wl1", "bss", "del", NULL }, 1 },
    { "wl bss -C 1 down",          "wl", { "-i", "wl1", "bss", "-C", "1", "down", NULL }, 1 },
    { "wl chanspecs",              "wl", { "-i", "wl1", "chanspecs", NULL }, 1 },
    { "wl mbssid get",             "wl", { "-i", "wl1", "mbssid", NULL }, 1 },
    { "wl mbssid 1",               "wl", { "-i", "wl1", "mbssid", "1", NULL }, 1 },
    { "wl txpwr_target_max",       "wl", { "-i", "wl1", "txpwr_target_max", NULL }, 1 },
    { "wl txpwrlimit",             "wl", { "-i", "wl1", "txpwrlimit", NULL }, 1 },
    { "wl dfs_ap_move get",        "wl", { "-i", "wl1", "dfs_ap_move", NULL }, 1 },
    { "wl dfs_ap_move 48/160",     "wl", { "-i", "wl1", "dfs_ap_move", "48/160", NULL }, 1 },
    { "wl reset_cnts",             "wl", { "-i", "wl1", "reset_cnts", NULL }, 1 },
    { "wl memuse",                 "wl", { "-i", "wl1", "memuse", NULL }, 1 },
    { "wl country_rev CN",         "wl", { "-i", "wl1", "country_rev", "CN", NULL }, 1 },
    { "wl radio on",               "wl", { "-i", "wl1", "radio", "on", NULL }, 1 },
    { "wl radio off",              "wl", { "-i", "wl1", "radio", "off", NULL }, 1 },
    { "wl radio 1",                "wl", { "-i", "wl1", "radio", "1", NULL }, 1 },
    { "wl csa unicast u",          "wl", { "-i", "wl1", "csa", "1", "5", "48/160", "u", NULL }, 1 },
    { "wl mac get",                "wl", { "-i", "wl1", "mac", NULL }, 1 },
    { "wl mac none",               "wl", { "-i", "wl1", "mac", "none", NULL }, 1 },
    { "wl mac add",                "wl", { "-i", "wl1", "mac", "aa:bb:cc:dd:ee:ff", NULL }, 1 },
    { "wl autochannel get",        "wl", { "-i", "wl1", "autochannel", NULL }, 1 },
    { "wl autochannel 1",          "wl", { "-i", "wl1", "autochannel", "1", NULL }, 1 },
    /* Sign-off audit H1-H6 & M1-M2 regression tests */
    { "wl txpwr1 20 (default dBm)","wl", { "-i", "wl1", "txpwr1", "20", NULL }, 1 },
    { "wl txpwr1 -m 100",          "wl", { "-i", "wl1", "txpwr1", "-m", "100", NULL }, 1 },
    { "wl txpwr_cap get",          "wl", { "-i", "wl1", "txpwr_cap", NULL }, 1 },
    { "wl txpwr_cap 80 80",        "wl", { "-i", "wl1", "txpwr_cap", "80", "80", NULL }, 1 },
    { "wl cfp_dump mac",           "wl", { "-i", "wl1", "cfp_dump", "ce:68:6d:ce:f4:ca", NULL }, 1 },
    { "wl taf",                    "wl", { "-i", "wl1", "taf", NULL }, 1 },
    { "wl clear_radar_status",     "wl", { "-i", "wl1", "clear_radar_status", NULL }, 1 },
    { "wl chan_info 36",           "wl", { "-i", "wl1", "chan_info", "36", NULL }, 1 },
    { "wl chan_info 52",           "wl", { "-i", "wl1", "chan_info", "52", NULL }, 1 },
    { "wl chanim_mode 2",          "wl", { "-i", "wl1", "chanim_mode", "2", NULL }, 1 },
    { "wl muinfo",                 "wl", { "-i", "wl1", "muinfo", NULL }, 1 },
    { "wl muinfo -v",              "wl", { "-i", "wl1", "muinfo", "-v", NULL }, 1 },
    /* Round-3 sign-off audit H1/H2 & M3 coverage */
    { "wl wnm get",                "wl", { "-i", "wl1", "wnm", NULL }, 1 },
    { "wl wnm +0x4",               "wl", { "-i", "wl1", "wnm", "+0x4", NULL }, 1 },
    { "wl wnm -0x1",               "wl", { "-i", "wl1", "wnm", "-0x1", NULL }, 1 },
    { "wl wnm 0x3",                "wl", { "-i", "wl1", "wnm", "0x3", NULL }, 1 },
    { "wl taf mac get",            "wl", { "-i", "wl1", "taf", "ce:68:6d:ce:f4:ca", NULL }, 1 },
    { "wl taf mac set",            "wl", { "-i", "wl1", "taf", "ce:68:6d:ce:f4:ca", "ebs", "1", "2", NULL }, 1 },
    { "wl dump_clear ampdu",       "wl", { "-i", "wl1", "dump_clear", "ampdu", NULL }, 1 },
    { "wl dlystats_clear",         "wl", { "-i", "wl1", "dlystats_clear", NULL }, 1 },
    { "wl disassoc mac",           "wl", { "-i", "wl1", "disassoc", "ce:68:6d:ce:f4:ca", NULL }, 1 },
    { "wl chanim_state 48/160",    "wl", { "-i", "wl1", "chanim_state", "48/160", NULL }, 1 },
    { "wl radar get",              "wl", { "-i", "wl1", "radar", NULL }, 1 },
    { "wl radar 1",                "wl", { "-i", "wl1", "radar", "1", NULL }, 1 },
    { "wl bssmax",                 "wl", { "-i", "wl1", "bssmax", NULL }, 1 },
    { "wl authe_sta_list",         "wl", { "-i", "wl1", "authe_sta_list", NULL }, 1 },
    { "wl autho_sta_list",         "wl", { "-i", "wl1", "autho_sta_list", NULL }, 1 },
    { "wl phy_rssi_ant",           "wl", { "-i", "wl1", "phy_rssi_ant", NULL }, 1 },
};

static void normalize_prog(char *s) {
    /* Replace /opt/bcm/libexec/ and /open/ with <BIN>/ for apples-to-apples stdout/stderr diff */
    const char *pats[] = { "/opt/bcm/libexec/", "/open/" };
    for (int p = 0; p < 2; p++) {
        size_t plen = strlen(pats[p]);
        char *hit;
        while ((hit = strstr(s, pats[p])) != NULL) {
            memmove(hit + 6, hit + plen, strlen(hit + plen) + 1);
            memcpy(hit, "<BIN>/", 6);
        }
    }
}

static void strip_rd_metadata(char *s) {
    char *p;
    while ((p = strstr(s, "rd_metadata:")) != NULL) {
        char *nl = strchr(p, '\n');
        if (nl) memmove(p, nl + 1, strlen(nl + 1) + 1);
        else { *p = '\0'; break; }
    }
}

/* Verify bdmf_shell UNIX domain socket handshake + response + EOT */
static int test_bdmf_unix_socket(void) {
    const char *spath = "/tmp/bdmf_test.sock";
    unlink(spath);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return 1;
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    strcpy(un.sun_path, spath);
    if (bind(srv, (struct sockaddr *)&un, sizeof(un)) < 0 || listen(srv, 2) < 0) {
        close(srv);
        return 1;
    }

    int fails = 0;
    const char *bins[2] = { "/opt/bcm/libexec/bdmf_shell", "/open/bdmf_shell" };
    char *envs[2][3] = {
        { "LD_LIBRARY_PATH=/opt/bcm/lib", "PATH=/opt/bcm/bin:/bin", NULL },
        { "PATH=/open:/bin", NULL, NULL }
    };
    char rx_log[2][256] = {{0}, {0}};
    char out_log[2][256] = {{0}, {0}};
    int rcs[2] = {-1, -1};

    for (int b = 0; b < 2; b++) {
        int out_p[2];
        pipe(out_p);
        pid_t cpid = fork();
        if (cpid == 0) {
            close(srv);
            dup2(out_p[1], 1);
            close(out_p[0]); close(out_p[1]);
            char *argv[] = { (char *)bins[b], (char *)spath, "-cmd", "/b/e", NULL };
            execve(bins[b], argv, envs[b]);
            _exit(127);
        }
        close(out_p[1]);
        int cfd = accept(srv, NULL, NULL);
        if (cfd >= 0) {
            size_t tot = 0;
            char buf[64];
            int replied = 0;
            for (;;) {
                ssize_t n = recv(cfd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                for (ssize_t k = 0; k < n && tot + 1 < sizeof(rx_log[b]); k++)
                    rx_log[b][tot++] = buf[k];
                rx_log[b][tot] = '\0';
                if (!replied && strstr(rx_log[b], "/b/e\n")) {
                    const char *banner = "BDMF_OK\n";
                    (void)send(cfd, banner, strlen(banner), 0);
                    replied = 1;
                }
                if (memchr(buf, 0x04, (size_t)n)) break;
            }
            close(cfd);
        }
        int st = 0;
        waitpid(cpid, &st, 0);
        rcs[b] = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
        ssize_t on = read(out_p[0], out_log[b], sizeof(out_log[b]) - 1);
        if (on > 0) out_log[b][on] = '\0';
        close(out_p[0]);
    }
    close(srv);
    unlink(spath);

    if (rcs[1] == 0 && strcmp(out_log[1], "BDMF_OK\n") == 0 &&
        rx_log[0][0] == '\n' && rx_log[1][0] == '\n' &&
        strstr(rx_log[0], "/b/e\n") != NULL &&
        strstr(rx_log[1], "/b/e\n") != NULL && strchr(rx_log[1], 0x04) != NULL) {
        printf("[PASS] bdmf_shell UNIX socket roundtrip (handshake+cmd+rx+EOT verified)\n");
    } else {
        printf("[FAIL] bdmf_shell UNIX socket roundtrip rc=%d/%d out=[%s]/[%s] rx0=[%s] rx1=[%s]\n",
               rcs[0], rcs[1], out_log[0], out_log[1], rx_log[0], rx_log[1]);
        fails++;
    }
    return fails;
}

/* Verify bcm_bootstate write transitions (open-source direct UBI_IOCVOLUP + write + CRC32) */
static int test_bootstate_writes(mock_state_t *ms) {
    struct {
        const char *arg;
        int         expect_volup;
        int         expect_committed;
        const char *expect_reason;
    } cases[] = {
        { "1",  0, 2, "0\n"   }, /* NEW (seq2=2 > seq1=1 -> '6', already '6', no-op) */
        { "2",  1, 1, "0x0\n" }, /* OLD (seq1=1 < seq2=2 -> '5', switches committed 2->1) */
        { "3",  1, 1, "0x1\n" }, /* NEW_ONCE (seq2>seq1 -> '8', commits 1 + reason=1) */
        { "4",  0, 2, "0x1\n" }, /* OLD_ONCE (seq1<seq2 -> '7', keeps committed=2 + reason=1) */
        { "5",  1, 1, "0x0\n" }, /* PART1: switches committed 2->1, reason=0 */
        { "6",  0, 2, "0x1\n" }, /* PART1_ONCE (6->'7'): keeps committed=2, sets reason=0x1 */
        { "7",  0, 2, "0\n"   }, /* PART2 (7->'6'): already in state '6', no-op */
        { "8",  1, 1, "0x1\n" }, /* PART2_ONCE: commits 1, sets reason=0x1 */
        { "+1", 1, 1, "0\n"   }, /* commit part 1 */
        { "-2", 1, 0, "0\n"   }, /* uncommit part 2 */
    };
    char *envp[] = { "PATH=/open:/bin", NULL };
    int fails = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *argv[] = { "/open/bcm_bootstate", (char *)cases[i].arg, NULL };
        run_result_t r;
        run_traced("/open/bcm_bootstate", argv, envp, ms, &r);
        int ok = (r.rc == 0);
        int saw_volup = (strstr(r.trace.data, "UBI_IOCVOLUP vol=1 bytes=1280\nUBI_WRITE vol=1 armed=1 volup_bytes=1280") != NULL &&
                         strstr(r.trace.data, "UBI_IOCVOLUP vol=2 bytes=1280\nUBI_WRITE vol=2 armed=1 volup_bytes=1280") != NULL);
        if (cases[i].expect_volup && !saw_volup) ok = 0;
        if (!cases[i].expect_volup && strstr(r.trace.data, "UBI_WRITE") != NULL) ok = 0;
        if (strcmp(r.reset_reason_after, cases[i].expect_reason) != 0) ok = 0;
        char expect_comm[32];
        snprintf(expect_comm, sizeof(expect_comm), "COMMITTED=%d", cases[i].expect_committed);
        if (cases[i].expect_volup) {
            if (memcmp(r.meta1_after + 0x0c, expect_comm, strlen(expect_comm)) != 0 ||
                memcmp(r.meta1_after, r.meta2_after, 1280) != 0) ok = 0;
        }
        if (ok) {
            printf("[PASS] bcm_bootstate write '%s' (volup=%d reason=%s)\n",
                   cases[i].arg, saw_volup, r.reset_reason_after[0] == '0' && r.reset_reason_after[1] == 'x' ? r.reset_reason_after : "unchanged");
        } else {
            printf("[FAIL] bcm_bootstate write '%s' rc=%d saw_volup=%d reason=[%s]\n  trace:\n%s",
                   cases[i].arg, r.rc, saw_volup, r.reset_reason_after, r.trace.data);
            fails++;
        }
    }
    return fails;
}

/* Verify open-source user NVRAM lifecycle + kcommit + bs applet + wdtctl -d lifecycle */
static int test_extra_open_flows(mock_state_t *ms, int *out_passed) {
    int fails = 0;
    int ok_cnt = 0;
    char *envp[] = { "PATH=/open:/bin", NULL };
    run_result_t r;

    /* 1. User NVRAM set/get/setflag/getflag/show/usavefile/unset/uloadfile/kcommit */
    unlink("/data/.user_nvram.setting");
    unlink("/data/.kernel_nvram.setting");
    char *a_set[] = { "/open/nvram", "set", "foo=bar", NULL };
    run_traced("/open/nvram", a_set, envp, ms, &r);
    char *a_get[] = { "/open/nvram", "get", "foo", NULL };
    run_result_t r2;
    run_traced("/open/nvram", a_get, envp, ms, &r2);
    if (r.rc == 0 && r2.rc == 0 && strcmp(r2.out.data, "bar\n") == 0) {
        printf("[PASS] nvram user set/get roundtrip\n"); ok_cnt++;
    } else {
        printf("[FAIL] nvram user set/get rc=%d/%d out=[%s]\n", r.rc, r2.rc, r2.out.data); fails++;
    }

    char *a_sf0[] = { "/open/nvram", "setflag", "flg", "2=1", NULL };
    run_traced("/open/nvram", a_sf0, envp, ms, &r);
    char *a_sf1[] = { "/open/nvram", "set", "flg=a", NULL };
    run_traced("/open/nvram", a_sf1, envp, ms, &r2);
    char *a_sf2[] = { "/open/nvram", "setflag", "flg", "2=1", NULL };
    run_result_t r3;
    run_traced("/open/nvram", a_sf2, envp, ms, &r3);
    char *a_gf[] = { "/open/nvram", "getflag", "flg", "2", NULL };
    run_result_t r4;
    run_traced("/open/nvram", a_gf, envp, ms, &r4);
    if (strcmp(r.out.data, "value:NULL->0\n") == 0 &&
        strcmp(r3.out.data, "value:a->e\n") == 0 &&
        strcmp(r4.out.data, "1\n") == 0) {
        printf("[PASS] nvram setflag/getflag hex & NULL->0 parity\n"); ok_cnt++;
    } else {
        printf("[FAIL] nvram setflag/getflag out0=[%s] out2=[%s] out3=[%s]\n",
               r.out.data, r3.out.data, r4.out.data); fails++;
    }

    char *a_show[] = { "/open/nvram", "show", NULL };
    run_traced("/open/nvram", a_show, envp, ms, &r);
    if (r.rc == 0 && strstr(r.out.data, "UserSpace NVRAMs") &&
        strstr(r.out.data, "foo=bar\n") && strstr(r.out.data, "KernelSpace NVRAMs") &&
        strstr(r.out.data, "boardid=HG6371F\n")) {
        printf("[PASS] nvram show (UserSpace + KernelSpace banners)\n"); ok_cnt++;
    } else {
        printf("[FAIL] nvram show rc=%d out=[%s]\n", r.rc, r.out.data); fails++;
    }

    char *a_kc[] = { "/open/nvram", "kcommit", NULL };
    run_traced("/open/nvram", a_kc, envp, ms, &r);
    FILE *kfp = fopen("/data/.kernel_nvram.setting", "r");
    char kbuf[256] = {0};
    if (kfp) { (void)fread(kbuf, 1, sizeof(kbuf) - 1, kfp); fclose(kfp); }
    if (r.rc == 0 && strstr(kbuf, "boardid=HG6371F\n")) {
        printf("[PASS] nvram kcommit atomic write\n"); ok_cnt++;
    } else {
        printf("[FAIL] nvram kcommit rc=%d file=[%s]\n", r.rc, kbuf); fails++;
    }

    /* 2. bs applet reading /var/bdmf_sh_id */
    FILE *bfp = fopen("/var/bdmf_sh_id", "w");
    if (bfp) { fputs("7\n", bfp); fclose(bfp); }
    char *a_bs[] = { "/open/bs", "/b/e", NULL };
    run_traced("/open/bs", a_bs, envp, ms, &r);
    if (r.rc == 0 && strstr(r.trace.data, "BDMF_IOCTL CMD sid=7 cmd=[/b/e\n]")) {
        printf("[PASS] bs applet (/var/bdmf_sh_id -> bdmf_shell)\n"); ok_cnt++;
    } else {
        printf("[FAIL] bs applet rc=%d trace=[%s]\n", r.rc, r.trace.data); fails++;
    }

    /* 3. wdtctl -d -p 0 stop safety (stock kills pgrp with SIGHUP=129; open safely rejects rc=1) */
    char *a_wdt0[] = { "/open/wdtctl", "-d", "-p", "0", "stop", NULL };
    run_traced("/open/wdtctl", a_wdt0, envp, ms, &r);
    if (r.rc == 1 && strstr(r.out.data, "Usage:")) {
        printf("[PASS] wdtctl -d -p 0 stop safety (rc=1, no kill(0,SIGHUP))\n"); ok_cnt++;
    } else {
        printf("[FAIL] wdtctl -d -p 0 stop rc=%d\n", r.rc); fails++;
    }

    *out_passed = ok_cnt;
    return fails;
}

static void write_file_str(const char *path, const char *s) {
    FILE *fp = fopen(path, "wb");
    if (fp) { fwrite(s, 1, strlen(s), fp); fclose(fp); }
}

static void setup_mock_sysfs(void) {
    /* Mount tmpfs on /sys so stock opendir("/sys/class/ubi"), "/sys/class/watchdog",
       and /sys/firmware/fdt work identically to the real device */
    mount("tmpfs", "/sys", "tmpfs", 0, NULL);
    mkdir("/sys/class", 0755);
    mkdir("/sys/class/ubi", 0755);
    mkdir("/sys/class/ubi/ubi0_1", 0755);
    mkdir("/sys/class/ubi/ubi0_2", 0755);
    mkdir("/sys/class/ubi/ubi0_3", 0755);
    mkdir("/sys/class/ubi/ubi0_5", 0755);
    write_file_str("/sys/class/ubi/ubi0_1/name", "metadata1\n");
    write_file_str("/sys/class/ubi/ubi0_2/name", "metadata2\n");
    write_file_str("/sys/class/ubi/ubi0_3/name", "bootfs1\n");
    write_file_str("/sys/class/ubi/ubi0_5/name", "bootfs2\n");
    mkdir("/sys/class/watchdog", 0755);
    mkdir("/sys/class/watchdog/watchdog0", 0755);
    write_file_str("/sys/class/watchdog/watchdog0/timeleft", "72\n");

    /* Minimal valid Flattened Device Tree (/sys/firmware/fdt): /chosen { active_image = <2>; } */
    mkdir("/sys/firmware", 0755);
    static const uint8_t fdt_blob[] = {
        /* header (40B): magic, totalsize=116, off_dt_struct=56, off_dt_strings=100,
           off_mem_rsvmap=40, version=17, last_comp=16, boot_cpuid=0, sz_strings=16, sz_struct=44 */
        0xd0,0x0d,0xfe,0xed, 0x00,0x00,0x00,0x74, 0x00,0x00,0x00,0x38, 0x00,0x00,0x00,0x64,
        0x00,0x00,0x00,0x28, 0x00,0x00,0x00,0x11, 0x00,0x00,0x00,0x10, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x10, 0x00,0x00,0x00,0x2c,
        /* mem_rsvmap terminator (16B) */
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        /* dt_struct (44B): BEGIN_NODE(""), BEGIN_NODE("chosen"), PROP(len=4,nameoff=0,val=2), END_NODE, END_NODE, END */
        0,0,0,1, 0,0,0,0,
        0,0,0,1, 'c','h','o','s','e','n',0,0,
        0,0,0,3, 0,0,0,4, 0,0,0,0, 0,0,0,2,
        0,0,0,2, 0,0,0,2, 0,0,0,9,
        /* dt_strings (16B): "active_image\0" */
        'a','c','t','i','v','e','_','i','m','a','g','e',0,0,0,0,
    };
    FILE *fp = fopen("/sys/firmware/fdt", "wb");
    if (fp) { fwrite(fdt_blob, 1, sizeof(fdt_blob), fp); fclose(fp); }
}

int main(int argc, char **argv) {
    if (strcmp(argv[0], "/bin/sh") == 0 || strcmp(argv[0], "sh") == 0) {
        /* Minimal /bin/sh for stock popen("/etc/get_rootfs_dev.sh") and system("cat .../timeleft") */
        if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
            if (strstr(argv[2], "get_rootfs_dev.sh")) {
                puts("/dev/ubiblock0_6");
            } else if (strstr(argv[2], "/sys/class/watchdog/") && strstr(argv[2], "timeleft")) {
                FILE *fp = fopen("/sys/class/watchdog/watchdog0/timeleft", "r");
                if (fp) {
                    char buf[64];
                    size_t n = fread(buf, 1, sizeof(buf), fp);
                    if (n > 0) (void)fwrite(buf, 1, n, stdout);
                    fclose(fp);
                }
            }
        }
        return 0;
    }
    mount("proc", "/proc", "proc", 0, NULL);
    setup_mock_sysfs();
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mkdir("/tmp", 0777);
    mkdir("/data", 0777);
    mkdir("/var", 0777);
    mkdir("/var/run", 0777);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
    mount("tmpfs", "/data", "tmpfs", 0, NULL);

    load_fixtures();

    printf("\n=== QEMU ARMv7 SYSCALL-MOCK DIFF SUITE ===\n");
    fflush(stdout);

    static mock_state_t ms;
    static run_result_t rs, ro;
    char *stock_env[] = { "LD_LIBRARY_PATH=/opt/bcm/lib", "PATH=/opt/bcm/bin:/bin", NULL };
    char *open_env[]  = { "PATH=/open:/bin", NULL };

    int passed = 0, failed = 0;

    for (size_t i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
        const test_case_t *tc = &TESTS[i];
        char s_exe[128], o_exe[128];
        snprintf(s_exe, sizeof(s_exe), "/opt/bcm/libexec/%s", tc->tool);
        snprintf(o_exe, sizeof(o_exe), "/open/%s", tc->tool);

        char *s_argv[24], *o_argv[24];
        s_argv[0] = s_exe;
        o_argv[0] = o_exe;
        int k = 0;
        while (tc->args[k]) {
            s_argv[k + 1] = (char *)tc->args[k];
            o_argv[k + 1] = (char *)tc->args[k];
            k++;
        }
        s_argv[k + 1] = NULL;
        o_argv[k + 1] = NULL;

        run_traced(s_exe, s_argv, stock_env, &ms, &rs);
        run_traced(o_exe, o_argv, open_env,  &ms, &ro);

        normalize_prog(rs.out.data); normalize_prog(ro.out.data);
        normalize_prog(rs.err.data); normalize_prog(ro.err.data);
        if (strcmp(tc->tool, "bcm_bootstate") == 0) {
            strip_rd_metadata(rs.out.data);
            strip_rd_metadata(ro.out.data);
        }

        int ok_rc    = (rs.rc == ro.rc);
        int ok_trace = (strcmp(rs.trace.data, ro.trace.data) == 0);
        int ok_io    = !tc->check_stdout ||
                       (strcmp(rs.out.data, ro.out.data) == 0 &&
                        strcmp(rs.err.data, ro.err.data) == 0);

        if (strcmp(tc->name, "nvram kget boardid") == 0 && strcmp(ro.out.data, "HG6371F\n") != 0) {
            ok_io = 0;
        }

        if (ok_rc && ok_trace && ok_io) {
            printf("[PASS] %s (rc=%d)\n", tc->name, ro.rc);
            passed++;
        } else {
            printf("[FAIL] %s (stock rc=%d, open rc=%d)\n", tc->name, rs.rc, ro.rc);
            if (!ok_trace) {
                printf("  -- stock trace --\n%s  -- open trace --\n%s", rs.trace.data, ro.trace.data);
            }
            if (!ok_io) {
                if (strcmp(rs.out.data, ro.out.data) != 0)
                    printf("  -- stock stdout --\n%s  -- open stdout --\n%s", rs.out.data, ro.out.data);
                if (strcmp(rs.err.data, ro.err.data) != 0)
                    printf("  -- stock stderr --\n%s  -- open stderr --\n%s", rs.err.data, ro.err.data);
            }
            failed++;
        }
        fflush(stdout);
    }

    int bs_fails = test_bootstate_writes(&ms);
    failed += bs_fails;
    passed += (10 - bs_fails);

    int ux_fails = test_bdmf_unix_socket();
    failed += ux_fails;
    passed += (1 - ux_fails);

    int ex_pass = 0;
    int ex_fails = test_extra_open_flows(&ms, &ex_pass);
    failed += ex_fails;
    passed += ex_pass;

    printf("\nVM_SUMMARY: %d passed, %d failed\n", passed, failed);
    fflush(stdout);
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    return 0;
}
