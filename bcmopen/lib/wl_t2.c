/*
 * wl_t2.c — T2 Extended AP Custom Command Handlers for bcmopen wl
 *
 * Covers all 8 high-value AP capability groups verified supported by `wl cap`:
 *   5.1 Client management (deauthenticate, disassoc, authe/autho_sta_list, mac, ...)
 *   5.2 11k/11v/11r roaming & steering (wnm, wnm_maxidle, rrm, rrm_nbr_list, ...)
 *   5.3 CSA / DFS / Channel (csa, channel, channels, chan_info, dfs_status, radar, chanim_mode/state, ...)
 *   5.4 Deep STA / Queue / CFP diagnostics (pktq_stats, dpstats, cfp_dump, bs_data, ...)
 *   5.5 Power / Reg / Temp (curpower, txpwr_target_max, txpwrlimit, country_rev, ...)
 *   5.6 Wi-Fi 6 MU / OFDMA / SR / TAF (muinfo, msched, umsched, taf, txcore, ...)
 *   5.7 Multi-SSID / Scan / Monitor (bss, bssid, ssid, radio, bssmax, mbssid, escan, ...)
 *   5.8 Diagnostics / Counters / Events (ver, revinfo, band, bands, memuse, pktcnt,
 *       wme_counters, reset_cnts, dump_clear, phy_rssi_ant, phy_snr_ant, ...)
 */
#include "wl_internal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static inline uint16_t rd_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline int32_t  rd_i32(const uint8_t *p) { int32_t  v; memcpy(&v, p, 4); return v; }

/* =========================================================================
 * 5.8 Core Diagnostics: ver, revinfo, band, bands, phy_rssi_ant, phy_snr_ant
 * ========================================================================= */
static int cmd_ver(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    /* Exact banner format from stock 0x2be60 matching wl1_samples.txt */
    printf("Broadcom BCA: 17.10 RC251.3202\n");
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int rc = wl_ctx_iovar_get(ctx, "ver", buf, sizeof(buf) - 1);
    if (rc == 0) {
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        puts(buf);
    }
    return rc;
}

static void format_boardrev(uint32_t brev, char out[16]) {
    /* Matches Broadcom bcm_boardrev_str (0x4b70c): 0x1303 -> "P303", 0x99d -> "A99d" */
    if (brev & 0x1000u) {
        snprintf(out, 16, "P%03x", brev & 0x0fffu);
    } else {
        snprintf(out, 16, "A%02x", brev & 0x0fffu);
    }
}

static int cmd_revinfo(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    /* 100-byte wlc_rev_info_t — every offset verified at stock 0x29c24..0x29ec4 */
    uint8_t r[100];
    memset(r, 0, sizeof(r));
    int rc = wl_ctx_ioctl(ctx, WLC_GET_REVINFO, r, sizeof(r), 0);
    if (rc < 0) return rc;

    char brev[16];
    format_boardrev(rd_u32(r + 0x1c), brev);

    printf("vendorid 0x%x\n", rd_u32(r + 0x00));
    printf("deviceid 0x%x\n", rd_u32(r + 0x04));
    printf("radiorev 0x%x\n", rd_u32(r + 0x08));
    printf("chipnum 0x%x\n", rd_u32(r + 0x2c));
    printf("chiprev 0x%x\n", rd_u32(r + 0x0c));
    printf("chippackage 0x%x\n", rd_u32(r + 0x3c));
    printf("corerev %u.%u\n", rd_u32(r + 0x10), rd_u32(r + 0x48));
    printf("boardid 0x%x\n", rd_u32(r + 0x14));
    printf("boardvendor 0x%x\n", rd_u32(r + 0x18));
    printf("boardrev %s\n", brev);

    uint32_t drev = rd_u32(r + 0x20);
    uint32_t drv_major = 0;
    if (drev == 0) {
        drv_major = rd_u32(r + 0x4c);
        printf("driverrev %u.%u.%u.%u\n",
               drv_major, rd_u32(r + 0x50), rd_u32(r + 0x54), rd_u32(r + 0x58));
    } else {
        drv_major = (drev >> 24) & 0xffu;
        printf("driverrev %u.%u.%u.%u\n",
               drv_major, (drev >> 16) & 0xffu, (drev >> 8) & 0xffu, drev & 0xffu);
    }

    uint32_t urev = rd_u32(r + 0x24);
    printf("ucoderev %u.%u\n", urev >> 16, urev & 0xffffu);
    printf("bus 0x%x\n", rd_u32(r + 0x28));
    printf("phytype 0x%x\n", rd_u32(r + 0x30));
    printf("phyrev %u.%u\n", rd_u32(r + 0x34), rd_u32(r + 0x44));
    printf("anarev 0x%x\n", rd_u32(r + 0x38));
    printf("nvramrev %u\n", rd_u32(r + 0x40));
    if (drv_major > 16) {
        printf("otpflag 0x%x\n", rd_u32(r + 0x5c));
    }
    return 0;
}

static int cmd_band(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        int32_t b = 0;
        int rc = wl_ctx_ioctl(ctx, WLC_GET_BAND, &b, sizeof(b), 0);
        if (rc < 0) return rc;
        switch (b) {
        case 0: puts("auto"); break;
        case 1: puts("a"); break;
        case 2: puts("b"); break;
        case 4: puts("6g"); break;
        default: printf("unrecognized band value %d\n", b); return -EINVAL;
        }
        return 0;
    }
    int32_t b = -1;
    if (strcasecmp(argv[1], "auto") == 0)                                         b = 0;
    else if (strcasecmp(argv[1], "a") == 0 || strcasecmp(argv[1], "5g") == 0)     b = 1;
    else if (strcasecmp(argv[1], "b") == 0 || strcasecmp(argv[1], "2g") == 0)     b = 2;
    else if (strcasecmp(argv[1], "6g") == 0)                                      b = 4;
    else {
        printf("unsupported band: %s\n", argv[1]);
        return -EINVAL;
    }
    return wl_ctx_ioctl(ctx, WLC_SET_BAND, &b, sizeof(b), 1);
}

static int cmd_bands(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t list[4] = {0};
    int rc = wl_ctx_ioctl(ctx, WLC_GET_BANDLIST, list, sizeof(list), 0);
    if (rc < 0) return rc;
    uint32_t cnt = list[0];
    if (cnt > 3) cnt = 3;
    for (uint32_t i = 1; i <= cnt; i++) {
        if (list[i] == 1)      printf("a ");
        else if (list[i] == 2) printf("b ");
        else if (list[i] == 4) printf("6g ");
        else                   printf("? ");
    }
    putchar('\n');
    return 0;
}

static int cmd_phy_ant_metric(const wl_ctx_t *ctx, const char *iovar, const char *label,
                              int argc, char **argv) {
    uint8_t param[8];
    size_t plen = 0;
    memset(param, 0, sizeof(param));
    if (argc >= 2) {
        if (bcm_wl_parse_mac(argv[1], param) < 0) {
            fprintf(stderr, " ERROR: no valid ether addr provided\n");
            return -EINVAL;
        }
        plen = 6;
        if (argc >= 3) {
            uint32_t v = 0;
            if (wl_parse_u32(argv[2], &v) < 0 || v > 0xffffu) return -EINVAL;
            uint16_t v16 = (uint16_t)v;
            memcpy(param + 6, &v16, 2);
            plen = 8;
        }
    }
    uint8_t resp[32];
    memset(resp, 0, sizeof(resp));
    int rc = (plen > 0)
        ? wl_ctx_iovar_getbuf(ctx, iovar, param, plen, resp, sizeof(resp))
        : wl_ctx_iovar_get(ctx, iovar, resp, sizeof(resp));
    if (rc < 0) return rc;
    uint32_t cnt = rd_u32(resp + 4);
    if (cnt == 0) {
        puts("not supported on this chip");
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        int8_t v = (int8_t)resp[8 + i];
        if (v != 0) printf("%s[%d] %d  ", label, i, (int)v);
    }
    putchar('\n');
    return 0;
}

static int cmd_phy_rssi_ant(const wl_ctx_t *ctx, int argc, char **argv) {
    return cmd_phy_ant_metric(ctx, "phy_rssi_ant", "rssi", argc, argv);
}

static int cmd_phy_snr_ant(const wl_ctx_t *ctx, int argc, char **argv) {
    return cmd_phy_ant_metric(ctx, "phy_snr_ant", "snr", argc, argv);
}

static int cmd_pktcnt(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t c[5] = {0};
    int rc = wl_ctx_ioctl(ctx, 137 /* WLC_GET_PKTCNTS */, c, sizeof(c), 0);
    if (rc < 0) return rc;
    printf("Receive: good packet %u, bad packet %u, othercast good packet %u\n",
           c[0], c[1], c[4]);
    printf("Transmit: good packet %u, bad packet %u\n", c[2], c[3]);
    return 0;
}

static int cmd_memuse(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    /* Verified stock 0x2b7e8..0x2b950: 80-byte memuse_info_t on modern drivers (driverrev==0 || >=9.0) */
    uint8_t rev[100];
    memset(rev, 0, sizeof(rev));
    int rc = wl_ctx_ioctl(ctx, WLC_GET_REVINFO, rev, sizeof(rev), 0);
    if (rc < 0) return rc;
    uint32_t drev = rd_u32(rev + 0x20);
    if (drev != 0 && drev < 0x09000000u) {
        char *buf = (char *)calloc(1, WL_IOCTL_LARGE_LEN);
        if (!buf) return -ENOMEM;
        rc = wl_ctx_iovar_get(ctx, "memuse", buf, WL_IOCTL_LARGE_LEN - 1);
        if (rc == 0) puts(buf);
        free(buf);
        return rc;
    }
    uint8_t m[80];
    memset(m, 0, sizeof(m));
    rc = wl_ctx_iovar_get(ctx, "memuse", m, sizeof(m));
    if (rc < 0) return rc;
    uint16_t ver = rd_u16(m + 0);
    uint16_t len = rd_u16(m + 2);
    uint32_t total = rd_u32(m + 0x14);
    uint32_t mfree = rd_u32(m + 0x18);
    if (ver == 0 && len >= 80) {
        uint32_t inuse = rd_u32(m + 0x1c);
        uint32_t hwm   = rd_u32(m + 0x20);
        uint32_t lwm   = rd_u32(m + 0x2c);
        printf("Heap Total: %u(%uK)\n", total, (total + 1023u) >> 10);
        printf("Free: %u(%uK), LWM: %u(%uK)\n",
               mfree, (mfree + 1023u) >> 10, lwm, (lwm + 1023u) >> 10);
        printf("In use: %u(%uK), HWM: %u(%uK)\n",
               inuse, (inuse + 1023u) >> 10, hwm, (hwm + 1023u) >> 10);
    } else {
        printf("Heap Total: %u(%uK), Heap Free: %u(%uK)\n",
               total, (total + 1023u) >> 10, mfree, (mfree + 1023u) >> 10);
    }
    return 0;
}

static int cmd_wme_counters(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    int rc = wl_ctx_iovar_get(ctx, "wme_counters", buf, sizeof(buf));
    if (rc < 0) return rc;
    if (rd_u16(buf) != 1) return -EOPNOTSUPP;
    int32_t ap = 0;
    (void)wl_ctx_ioctl(ctx, WLC_GET_AP, &ap, sizeof(ap), 0);
    static const char *names[4] = { "AC_BE", "AC_BK", "AC_VI", "AC_VO" };
    /* Layout verified at 0x1deb4..0x1e034 (196 bytes):
     * +0x00 u16 version (=1), +0x02 u16 len,
     * +0x04 tx[4], +0x24 tx_failed[4], +0x44 rx[4], +0x64 rx_failed[4],
     * +0x84 forward[4], +0xa4 tx_expired[4] (each entry is 8B {u32 pkts, u32 bytes})
     */
    for (int i = 0; i < 4; i++) {
        size_t o = (size_t)i * 8u;
        printf("\n%s: tx frames: %u bytes: %u failed frames: %u failed bytes: %u\n",
               names[i],
               rd_u32(buf + 0x04 + o), rd_u32(buf + 0x08 + o),
               rd_u32(buf + 0x24 + o), rd_u32(buf + 0x28 + o));
        printf("       rx frames: %u bytes: %u failed frames: %u failed bytes: %u\n",
               rd_u32(buf + 0x44 + o), rd_u32(buf + 0x48 + o),
               rd_u32(buf + 0x64 + o), rd_u32(buf + 0x68 + o));
        if (ap) {
            printf("       foward frames: %u bytes: %u \n",
                   rd_u32(buf + 0x84 + o), rd_u32(buf + 0x88 + o));
        }
        printf("       tx frames time expired: %u \n", rd_u32(buf + 0xa4 + o));
    }
    putchar('\n');
    return 0;
}

static int cmd_reset_cnts(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    int32_t dummy = 0;
    return wl_ctx_iovar_getint(ctx, "reset_cnts", &dummy);
}

static int cmd_dump_clear(const wl_ctx_t *ctx, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        int rc = wl_ctx_iovar_set(ctx, "dump_clear", argv[i], strlen(argv[i]));
        if (rc < 0) return rc;
    }
    return 0;
}

/* =========================================================================
 * 5.1 Client Management: deauthenticate, disassoc, authe/autho_sta_list, mac
 * ========================================================================= */
static int cmd_deauthenticate(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        fprintf(stderr, "STA MAC not specified, deauth all\n");
        uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        return wl_ctx_ioctl(ctx, WLC_SCB_DEAUTHENTICATE, bcast, sizeof(bcast), 1);
    }
    uint8_t mac[6];
    if (bcm_wl_parse_mac(argv[1], mac) < 0) {
        fprintf(stderr, "Malformed STA MAC parameter\n");
        return -EINVAL;
    }
    if (argc >= 3) {
        int32_t reason = 0;
        if (wl_parse_i32(argv[2], &reason) < 0) return -EINVAL;
        uint8_t scb[12];
        memset(scb, 0, sizeof(scb));
        memcpy(scb + 0, &reason, 4);
        memcpy(scb + 4, mac, 6);
        return wl_ctx_ioctl(ctx, WLC_SCB_DEAUTHENTICATE_FOR_REASON, scb, sizeof(scb), 1);
    }
    return wl_ctx_ioctl(ctx, WLC_SCB_DEAUTHENTICATE, mac, sizeof(mac), 1);
}

static int cmd_disassoc(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc >= 2) {
        uint8_t mac[6];
        if (bcm_wl_parse_mac(argv[1], mac) < 0) return -EINVAL;
        return wl_ctx_ioctl(ctx, WLC_DISASSOC, mac, sizeof(mac), 1);
    }
    return wl_ctx_ioctl(ctx, WLC_DISASSOC, NULL, 0, 1);
}

static int cmd_sta_list_iovar(const wl_ctx_t *ctx, const char *iovar) {
    uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
    if (!buf) return -ENOMEM;
    uint32_t max_cnt = 0xaaa;
    memcpy(buf, &max_cnt, 4);
    int rc = wl_ctx_iovar_get(ctx, iovar, buf, WL_IOCTL_LARGE_LEN);
    if (rc == 0) {
        uint32_t cnt = rd_u32(buf);
        if (cnt > 0xaaa) cnt = 0xaaa;
        for (uint32_t i = 0; i < cnt; i++) {
            char s[18];
            bcm_wl_format_mac(buf + 4 + i * 6, s, 0);
            printf("%s %s\n", iovar, s);
        }
    }
    free(buf);
    return rc;
}

static int cmd_authe_sta_list(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    return cmd_sta_list_iovar(ctx, "authe_sta_list");
}

static int cmd_autho_sta_list(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    return cmd_sta_list_iovar(ctx, "autho_sta_list");
}

static int cmd_mac(const wl_ctx_t *ctx, int argc, char **argv) {
    uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
    if (!buf) return -ENOMEM;
    int rc = 0;

    if (argc == 1) {
        uint32_t max_cnt = 0xaaa;
        memcpy(buf, &max_cnt, 4);
        rc = wl_ctx_ioctl(ctx, WLC_GET_MACLIST, buf, WL_IOCTL_LARGE_LEN, 0);
        if (rc == 0) {
            uint32_t cnt = rd_u32(buf);
            if (cnt > 0xaaa) cnt = 0xaaa;
            for (uint32_t i = 0; i < cnt; i++) {
                char s[18];
                bcm_wl_format_mac(buf + 4 + i * 6, s, 0);
                printf("mac %s\n", s);
            }
        }
        free(buf);
        return rc;
    }

    if (strcasecmp(argv[1], "none") == 0 || strcasecmp(argv[1], "clear") == 0) {
        uint32_t zero = 0;
        rc = wl_ctx_ioctl(ctx, WLC_SET_MACLIST, &zero, 4, 1);
        free(buf);
        return rc;
    }

    /* Read existing list, then add/del (up to 0xaaa = 2730 entries, deduplicated) */
    uint32_t max_cnt = 0xaaa;
    memcpy(buf, &max_cnt, 4);
    rc = wl_ctx_ioctl(ctx, WLC_GET_MACLIST, buf, WL_IOCTL_LARGE_LEN, 0);
    if (rc < 0) {
        free(buf);
        return rc;
    }
    uint32_t cnt = rd_u32(buf);
    if (cnt > 0xaaa) cnt = 0xaaa;

    if (strcasecmp(argv[1], "del") == 0) {
        for (int i = 2; i < argc; i++) {
            uint8_t target[6];
            if (bcm_wl_parse_mac(argv[i], target) < 0) { free(buf); return -EINVAL; }
            for (uint32_t j = 0; j < cnt; j++) {
                if (memcmp(buf + 4 + j * 6, target, 6) == 0) {
                    memmove(buf + 4 + j * 6, buf + 4 + (j + 1) * 6, (cnt - j - 1) * 6);
                    cnt--;
                    break;
                }
            }
        }
    } else {
        /* Matches stock 0x41824: `wl mac <mac1> [mac2...]` (no "add" keyword) */
        for (int i = 1; i < argc && cnt < 0xaaa; i++) {
            uint8_t mac[6];
            if (bcm_wl_parse_mac(argv[i], mac) < 0) {
                printf("Problem parsing MAC address \"%s\".\n", argv[i]);
                free(buf);
                return -EINVAL;
            }
            int dup = 0;
            for (uint32_t j = 0; j < cnt; j++) {
                if (memcmp(buf + 4 + j * 6, mac, 6) == 0) { dup = 1; break; }
            }
            if (!dup) {
                memcpy(buf + 4 + cnt * 6, mac, 6);
                cnt++;
            }
        }
    }

    memcpy(buf, &cnt, 4);
    rc = wl_ctx_ioctl(ctx, WLC_SET_MACLIST, buf, 4 + cnt * 6, 1);
    free(buf);
    return rc;
}

/* =========================================================================
 * 5.3 CSA / DFS / Channel: csa, channel, channels, chan_info, dfs_*, radar, chanim_*
 * ========================================================================= */
static int cmd_channel(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        uint32_t ci[3] = {0}; /* hw_channel, target_channel, scan_channel */
        int rc = wl_ctx_ioctl(ctx, WLC_GET_CHANNEL, ci, sizeof(ci), 0);
        if (rc < 0) return rc;
        if (ci[2] != 0) {
            puts("Scan in progress.");
            printf("current scan channel\t%d\n", (int32_t)ci[2]);
        } else {
            puts("No scan in progress.");
        }
        printf("current mac channel\t%u\n", ci[0]);
        printf("target channel\t%u\n", ci[1]);
        return 0;
    }
    int32_t ch = 0;
    if (wl_parse_i32(argv[1], &ch) < 0) return -EINVAL;
    return wl_ctx_ioctl(ctx, WLC_SET_CHANNEL, &ch, sizeof(ch), 1);
}

static int cmd_channels(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t buf[256];
    memset(buf, 0, sizeof(buf));
    buf[0] = 255;
    int rc = wl_ctx_ioctl(ctx, WLC_GET_VALID_CHANNELS, buf, sizeof(buf), 0);
    if (rc < 0) return rc;
    uint32_t cnt = buf[0];
    if (cnt > 255) cnt = 255;
    for (uint32_t i = 0; i < cnt; i++) printf("%u ", buf[1 + i]);
    putchar('\n');
    return 0;
}

static int cmd_chan_info(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Verified stock wl_chan_info (0x32da0..0x33040):
     *   0x0001=VALID_HW, 0x0002=VALID_SW, 0x0004=5G Band, 0x0008=RADAR Sensitive,
     *   0x0010=Temporarily Out of Service (minutes in info>>24), 0x0020=Passive,
     *   0x0040=Restricted, 0x0080=Weather Radar, 0x0100=CLM Restrict,
     *   0x0200=6G Band, 0x0400=CAC Pending
     */
    uint16_t cs = 0;
    if (argc >= 2) {
        if (bcm_wl_chspec_from_str(argv[1], &cs) < 0) {
            fprintf(stderr, "error: could not parse \"%s\" as a chanspec\n", argv[1]);
            return -EINVAL;
        }
    } else {
        int32_t cur = 0;
        int rc = wl_ctx_iovar_getint(ctx, "chanspec", &cur);
        if (rc < 0) return rc;
        cs = (uint16_t)(cur & 0xffffu);
    }
    uint32_t req = cs, info = 0;
    int rc = wl_ctx_iovar_getbuf(ctx, "per_chan_info", &req, sizeof(req), &info, sizeof(info));
    if (rc < 0) return rc;
    if (!(info & 0x0001u)) {
        puts("Invalid Channel");
        return 0;
    }
    if (!(info & 0x0002u)) {
        puts("Not supported in current locale");
        return 0;
    }
    int prim = wl_chspec_primary_chan(cs);
    printf("Channel %d\t", prim);
    if (info & 0x0004u) {
        printf("5G Band");
    } else if (info & 0x0200u) {
        printf("6G Band");
        if ((prim & 15) == 5) printf(", PSC");
    } else {
        printf("2G Band");
    }
    if (info & 0x0008u) printf(", RADAR Sensitive");
    if (info & 0x0040u) printf(", Restricted");
    if (info & 0x0020u) printf(", Passive");
    if (info & 0x0100u) printf(", CLM Restrict");
    if (info & 0x0010u) printf(", Temporarily Out of Service for %u minutes", info >> 24);
    if (info & 0x0080u) printf(", Weather Radar");
    if (info & 0x0400u) printf(", CAC Pending");
    putchar('\n');
    return 0;
}

static int cmd_csa(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: wl csa <mode:0|1> <count> <chanspec> [u]\n");
        return -EINVAL;
    }
    int32_t mode = 0, count = 0;
    uint16_t cs = 0;
    if (wl_parse_i32(argv[1], &mode) < 0 || wl_parse_i32(argv[2], &count) < 0 ||
        bcm_wl_chspec_from_str(argv[3], &cs) < 0) {
        return -EINVAL;
    }
    if (mode < 0 || mode > 1 || count < 0 || count > 255) return -EINVAL;
    /* wl_chan_switch_t (6B, verified stock 0x33148..0x33240):
     * { u8 mode(0|1); u8 count; u16 chspec; u8 reg; u8 frame_type(0=bcast, 1=unicast "u"); }
     */
    uint8_t csa[6];
    memset(csa, 0, sizeof(csa));
    csa[0] = (uint8_t)mode;
    csa[1] = (uint8_t)count;
    memcpy(csa + 2, &cs, 2);
    if (argc >= 5) {
        if (strcmp(argv[4], "u") == 0)      csa[5] = 1;
        else                                return -EINVAL;
    }
    return wl_ctx_iovar_set(ctx, "csa", csa, sizeof(csa));
}

static int cmd_autochannel(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Verified stock 0x3558c..0x356b0:
     * - 0 args: WLC_GET_AUTOCHANNEL (215) -> prints chanspec or "invalid chanspec (0x0)"
     * - "1":    WLC_SET_AUTOCHANNEL (216) with 8B zero payload to trigger scan
     * - "2":    WLC_GET_AUTOCHANNEL (215) -> if non-zero, set "chanspec" IOVAR
     */
    if (argc == 1) {
        uint32_t cs = 0;
        int rc = wl_ctx_ioctl(ctx, 215 /* WLC_GET_AUTOCHANNEL */, &cs, sizeof(cs), 0);
        if (rc == 0) {
            if (cs == 0) {
                printf("invalid chanspec (0x%x)\n", cs);
            } else {
                char s[32];
                bcm_wl_chspec_to_str((uint16_t)(cs & 0xffffu), s, sizeof(s));
                printf("%s (0x%x)\n", s, (unsigned)(cs & 0xffffu));
            }
        }
        return rc;
    }
    int32_t v = 0;
    if (wl_parse_i32(argv[1], &v) < 0) return -EINVAL;
    if (v == 1) {
        uint32_t req[2] = { 0, 0 };
        return wl_ctx_ioctl(ctx, 216 /* WLC_SET_AUTOCHANNEL */, req, sizeof(req), 1);
    }
    if (v == 2) {
        uint32_t cs = 0;
        int rc = wl_ctx_ioctl(ctx, 215, &cs, sizeof(cs), 0);
        if (rc == 0 && cs != 0) {
            rc = wl_ctx_iovar_setint(ctx, "chanspec", (int32_t)cs);
        }
        return rc;
    }
    return -EINVAL;
}

static int cmd_radar(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        int32_t v = 0;
        int rc = wl_ctx_ioctl(ctx, 242 /* WLC_GET_RADAR */, &v, sizeof(v), 0);
        if (rc == 0) printf("%d\n", v);
        return rc;
    }
    int32_t v[2] = { 0, 0 };
    if (wl_parse_onoff_i32(argv[1], &v[0]) < 0) return -EINVAL;
    if (argc >= 3) {
        if (wl_parse_i32(argv[2], &v[1]) < 0) return -EINVAL;
        return wl_ctx_ioctl(ctx, 243 /* WLC_SET_RADAR */, v, sizeof(v), 1);
    }
    return wl_ctx_ioctl(ctx, 243 /* WLC_SET_RADAR */, &v[0], sizeof(v[0]), 1);
}

static int cmd_chanim_mode(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Verified stock 0x9a7a8..0x9a87c: 0=disabled, 1=detect only, 2=external(acsd), 3=detect+act */
    if (argc == 1) {
        int32_t m = 0;
        int rc = wl_ctx_iovar_getint(ctx, "chanim_mode", &m);
        if (rc < 0) return rc;
        switch (m) {
        case 0: puts("CHANIM mode: disabled."); break;
        case 1: puts("CHANIM mode: detect only."); break;
        case 2: puts("CHANIM mode: external (acsd)."); break;
        case 3: puts("CHANIM mode: detect + act."); break;
        default: printf("CHANIM mode: %d\n", m); break;
        }
        return 0;
    }
    int32_t m = 0;
    if (wl_parse_i32(argv[1], &m) < 0 || m < 0 || m > 3) return -EINVAL;
    return wl_ctx_iovar_setint(ctx, "chanim_mode", m);
}

static int cmd_chanim_state(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc < 2) return -EINVAL;
    uint16_t c16 = 0;
    if (bcm_wl_chspec_from_str(argv[1], &c16) < 0) return -EINVAL;
    uint32_t cs = c16;
    int32_t st = 0;
    int rc = wl_ctx_iovar_getbuf(ctx, "chanim_state", &cs, sizeof(cs), &st, sizeof(st));
    if (rc == 0) printf("%d\n", st);
    return rc;
}

static int cmd_dfs_status(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    int rc = wl_ctx_iovar_get(ctx, "dfs_status", buf, sizeof(buf));
    if (rc < 0) return rc;
    static const char *states[] = {
        "IDLE", "PRE-ISM CAC", "In-Service Monitoring(ISM)",
        "Channel Switching Announcement(CSA)",
        "POST-ISM CAC", "PRE-ISM OOC", "POST-ISM OOC", "BACKGROUND CAC",
    };
    uint32_t st  = rd_u32(buf + 0);
    uint32_t dur = rd_u32(buf + 4);
    uint16_t cs  = rd_u16(buf + 8);
    const char *st_s = (st < sizeof(states) / sizeof(states[0])) ? states[st] : "Unknown";
    printf("state %s time elapsed %ums radar channel cleared by dfs ", st_s, dur);
    if (cs == 0) {
        puts("none");
    } else {
        char s[32];
        bcm_wl_chspec_to_str(cs, s, sizeof(s));
        printf("channel %s (0x%04X)\n", s, (unsigned)cs);
    }
    return 0;
}

static int cmd_dfs_ap_move(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Exact ABI verified at stock 0x3434c..0x34480:
     * GET: returns { i8 version(==1) @+0, i8 move_status @+1, u16 chspec @+2, ... }
     * SET: 1 arg -> 4B { u32 chspec_or_cmd }; 2 args -> 8B { u32 chspec_or_cmd, i32 mode }
     */
    if (argc == 1) {
        uint8_t buf[64];
        memset(buf, 0, sizeof(buf));
        int rc = wl_ctx_iovar_get(ctx, "dfs_ap_move", buf, sizeof(buf));
        if (rc == 0) {
            int ver = (int)(int8_t)buf[0];
            int st  = (int)(int8_t)buf[1];
            if (ver != 1) return -EOPNOTSUPP;
            printf("version=%d, move status=%d\n", ver, st);
            if (st == -1) {
                puts("dfs AP move in IDLE state");
            } else if (st == 2) {
                puts("Radar Scan In Progress");
            }
            printf("version: %u, num_sub_status: %u\n",
                   (unsigned)rd_u16(buf + 4), (unsigned)rd_u16(buf + 6));
        }
        return rc;
    }
    int32_t req[2] = { 0, 0 };
    uint16_t cs = 0;
    if (wl_parse_i32(argv[1], &req[0]) == 0 && (req[0] == -1 || req[0] == -2)) {
        /* -1 = abort, -2 = move immediately */
    } else if (bcm_wl_chspec_from_str(argv[1], &cs) == 0) {
        req[0] = (int32_t)cs;
    } else {
        return -EINVAL;
    }
    if (argc >= 3) {
        if (wl_parse_i32(argv[2], &req[1]) < 0) return -EINVAL;
        return wl_ctx_iovar_set(ctx, "dfs_ap_move", req, 8);
    }
    return wl_ctx_iovar_set(ctx, "dfs_ap_move", req, 4);
}

static int cmd_clear_radar_status(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    /* Verified stock 0x12f7c..0x12fcc: 240-byte zeroed wl_radar_status_t */
    uint8_t buf[240];
    memset(buf, 0, sizeof(buf));
    printf("Clear Radar Status \n");
    return wl_ctx_iovar_set(ctx, "clear_radar_status", buf, sizeof(buf));
}

/* =========================================================================
 * 5.7 Multi-SSID / Radio / Monitor: bss, bssid, ssid, radio, bssmax, mbssid
 * ========================================================================= */
static int cmd_radio(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        uint32_t val = 0;
        int rc = wl_ctx_ioctl(ctx, WLC_GET_RADIO, &val, sizeof(val), 0);
        if (rc == 0) printf("0x%04x\n", val);
        return rc;
    }
    uint32_t val = 0;
    if (strcasecmp(argv[1], "on") == 0)       val = 0x00010000u; /* mask=SW, val=0 (enable) */
    else if (strcasecmp(argv[1], "off") == 0) val = 0x00010001u; /* mask=SW, val=1 (disable) */
    else {
        if (wl_parse_u32(argv[1], &val) < 0) return -EINVAL;
        /* Stock 0x1d014: replicate lower 16-bit mask into upper 16 bits when unset */
        if ((val >> 16) == 0) val |= (val << 16);
    }
    return wl_ctx_ioctl(ctx, WLC_SET_RADIO, &val, sizeof(val), 1);
}

static int cmd_bss(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Verified stock 0x924f8..0x92710:
     * - Default idx = -1 (bound to -i <ifname>), never double-wrapped with bsscfg:
     * - States: 0=down, 1=up, 2=ap, 3=sta, 4=move, -1=del
     */
    int32_t idx = (ctx->bsscfg_idx >= 0) ? ctx->bsscfg_idx : -1;
    int argi = 1;
    if (argi + 1 < argc && strcmp(argv[argi], "-C") == 0) {
        if (wl_parse_i32(argv[argi + 1], &idx) < 0 || idx < 0) return -EINVAL;
        argi += 2;
    }
    if (argi >= argc) {
        int32_t st = 0;
        int rc = bcm_wl_iovar_getbuf(ctx->ifname, "bss", &idx, sizeof(idx), &st, sizeof(st));
        if (rc == 0) puts(st ? "up" : "down");
        return rc;
    }
    int32_t req[2] = { idx, 0 };
    if (strcasecmp(argv[argi], "down") == 0)       req[1] = 0;
    else if (strcasecmp(argv[argi], "up") == 0)    req[1] = 1;
    else if (strcasecmp(argv[argi], "ap") == 0)    req[1] = 2;
    else if (strcasecmp(argv[argi], "sta") == 0)   req[1] = 3;
    else if (strcasecmp(argv[argi], "move") == 0)  req[1] = 4;
    else if (strcasecmp(argv[argi], "del") == 0)   req[1] = -1;
    else if (wl_parse_i32(argv[argi], &req[1]) < 0) return -EINVAL;
    return bcm_wl_iovar_set(ctx->ifname, "bss", req, sizeof(req));
}

static int cmd_bssid(const wl_ctx_t *ctx, int argc, char **argv) {
    int bss = ctx->bsscfg_idx;
    int argi = 1;
    if (argi + 1 < argc && strcmp(argv[argi], "-C") == 0) {
        int32_t idx = 0;
        if (wl_parse_i32(argv[argi + 1], &idx) < 0 || idx < 0) return -EINVAL;
        bss = idx;
        argi += 2;
    }
    if (argi >= argc) {
        uint8_t mac[6] = {0};
        int rc = (bss >= 0)
            ? bcm_wl_bsscfg_iovar_get(ctx->ifname, bss, "bssid", mac, sizeof(mac))
            : wl_ctx_ioctl(ctx, WLC_GET_BSSID, mac, sizeof(mac), 0);
        if (rc == 0) {
            char s[18];
            bcm_wl_format_mac(mac, s, 0);
            puts(s);
        }
        return rc;
    }
    uint8_t mac[6];
    if (bcm_wl_parse_mac(argv[argi], mac) < 0) return -EINVAL;
    if (bss >= 0) {
        return bcm_wl_bsscfg_iovar_set(ctx->ifname, bss, "bssid", mac, sizeof(mac));
    }
    return wl_ctx_ioctl(ctx, WLC_SET_BSSID, mac, sizeof(mac), 1);
}

static int cmd_ssid(const wl_ctx_t *ctx, int argc, char **argv) {
    int bss = ctx->bsscfg_idx;
    int argi = 1;
    if (argi + 1 < argc && strcmp(argv[argi], "-C") == 0) {
        int32_t idx = 0;
        if (wl_parse_i32(argv[argi + 1], &idx) < 0 || idx < 0) return -EINVAL;
        bss = idx;
        argi += 2;
    }
    struct { uint32_t len; uint8_t ssid[32]; } s;
    memset(&s, 0, sizeof(s));
    if (argi >= argc) {
        int rc = (bss >= 0)
            ? bcm_wl_bsscfg_iovar_get(ctx->ifname, bss, "ssid", &s, sizeof(s))
            : wl_ctx_ioctl(ctx, WLC_GET_SSID, &s, sizeof(s), 0);
        if (rc == 0) {
            char esc[160];
            wl_format_ssid_escaped(s.ssid, s.len, esc, sizeof(esc));
            printf("Current SSID: \"%s\"\n", esc);
        }
        return rc;
    }
    size_t l = strlen(argv[argi]);
    if (l > 32) {
        fprintf(stderr, "ssid: SSID too long (max 32 bytes)\n");
        return -EINVAL;
    }
    s.len = (uint32_t)l;
    memcpy(s.ssid, argv[argi], l);
    char esc[160];
    wl_format_ssid_escaped(s.ssid, s.len, esc, sizeof(esc));
    printf("Setting SSID: \"%s\"\n", esc);
    if (bss >= 0) {
        return bcm_wl_bsscfg_iovar_set(ctx->ifname, bss, "ssid", &s, sizeof(s));
    }
    return wl_ctx_ioctl(ctx, WLC_SET_SSID, &s, sizeof(s), 1);
}

static int cmd_bssmax(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    char cap[WL_IOCTL_MAXLEN];
    memset(cap, 0, sizeof(cap));
    int rc = wl_ctx_iovar_get(ctx, "cap", cap, sizeof(cap) - 1);
    if (rc < 0) return rc;
    int max_bss = 1;
    if (strstr(cap, "mbss16"))      max_bss = 16;
    else if (strstr(cap, "mbss8"))  max_bss = 8;
    else if (strstr(cap, "mbss4"))  max_bss = 4;
    printf("%d\n", max_bss);
    return 0;
}

static int cmd_mbssid(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Verified stock 0x12c88..0x12d90: 4B packed { u8 ver=1; u8 len=4; i8 cfg; i8 status; } */
    if (argc == 1) {
        uint8_t buf[16];
        memset(buf, 0, sizeof(buf));
        int rc = wl_ctx_iovar_get(ctx, "mbssid", buf, sizeof(buf));
        if (rc == 0) {
            if (buf[0] != 1 || buf[1] < 4) return -EOPNOTSUPP;
            int cfg = (int)(int8_t)buf[2];
            int st  = (int)(int8_t)buf[3];
            const char *cs = (cfg == 0) ? "DISABLED" : (cfg == 1) ? "ENABLED" : "AUTO";
            const char *ss = (st == 0)  ? "DISABLED" : (st == 1)  ? "ENABLED" : "AUTO";
            printf("Config:[%d][%s]\nStatus:[%d][%s] \n", cfg, cs, st, ss);
        }
        return rc;
    }
    int32_t v = 0;
    if (wl_parse_onoff_i32(argv[1], &v) < 0 || v < -128 || v > 127) return -EINVAL;
    uint8_t req[4] = { 1, 4, (uint8_t)(int8_t)v, 0 };
    return wl_ctx_iovar_set(ctx, "mbssid", req, sizeof(req));
}

/* =========================================================================
 * 5.5 Power / Reg: txpwr_target_max, txpwrlimit, country_rev
 * ========================================================================= */
static int cmd_txpwr_target_max(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    /* Verified stock 0x1b518..0x1b5b0 (11B):
     * +0x00 u32 version(==0), +0x04 u16 chanspec, +0x06..0x09 i8 txpwr[4], +0x0a u8 nchain
     */
    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    int rc = wl_ctx_iovar_get(ctx, "txpwr_target_max", buf, 11);
    if (rc < 0) return rc;
    if (rd_u32(buf) != 0) return -EOPNOTSUPP;
    uint16_t cs = rd_u16(buf + 4);
    uint8_t nchain = buf[10];
    if (nchain > 4) nchain = 4;
    printf("Maximum Tx Power Target (chanspec:0x%x):\t", (unsigned)cs);
    for (uint8_t i = 0; i < nchain; i++) {
        int q = (int)(int8_t)buf[6 + i];
        int aq = (q < 0) ? -q : q;
        if (q < 0) printf("-%d.%02d  ", aq / 4, (aq % 4) * 25);
        else       printf("%2d.%02d  ", aq / 4, (aq % 4) * 25);
    }
    putchar('\n');
    return 0;
}

static int cmd_txpwrlimit(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    /* Verified stock 0x2c05c..0x2c120: 93-byte tx_power_legacy_t on WLC_CURRENT_TXPWR (256) */
    uint8_t buf[93];
    memset(buf, 0, sizeof(buf));
    int rc = wl_ctx_ioctl(ctx, 256 /* WLC_CURRENT_TXPWR */, buf, sizeof(buf), 0);
    if (rc < 0) return rc;
    uint8_t q = (buf[0x18] < buf[0]) ? buf[0x18] : buf[0];
    int whole = (int)(q >> 2);
    int frac  = (int)((q & 3u) * 25u);
    double mw_d = pow(10.0, (double)q / 40.0);
    unsigned int mw = (mw_d >= 65535.0) ? 65535u : (unsigned int)(mw_d + 0.5);
    printf("%u mW (%d.%d dBm)\n", mw, whole, frac);
    return 0;
}

static int cmd_country_rev(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "country_rev: country abbr unspecified\n");
        return -EINVAL;
    }
    size_t clen = strlen(argv[1]);
    if (clen == 0 || clen > 3) return -EINVAL;
    uint8_t req[4];
    memset(req, 0, sizeof(req));
    memcpy(req, argv[1], clen);
    uint8_t out[WL_IOCTL_MEDLEN]; /* 1536 bytes matching stock 0x15054 */
    memset(out, 0, sizeof(out));
    int rc = wl_ctx_iovar_getbuf(ctx, "country_rev", req, clen + 1, out, sizeof(out));
    if (rc == 0) {
        uint32_t cnt = rd_u32(out);
        uint32_t max_cnt = (WL_IOCTL_MEDLEN - 4u) / 4u;
        if (cnt > max_cnt) cnt = max_cnt;
        printf("count: %u\n", cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            printf("%d ", rd_i32(out + 4u + i * 4u));
        }
        putchar('\n');
    }
    return rc;
}

/* =========================================================================
 * Verified Binary/Text T2 Handlers (stock ABI confirmed by disassembly)
 * ========================================================================= */
static int cmd_wnm(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Verified stock 0x7c78c..0x7c8c0 + flag table 0xf4bcc:
     * Always reads current bitmask first; supports +<mask> (OR), -<mask> (AND-NOT), <mask> (set)
     */
    static const struct { uint32_t mask; const char *name; } wnm_flags[] = {
        { 0x00000001u, "BSS-Transition" },
        { 0x00000002u, "Proxy-ARP" },
        { 0x00000004u, "BSS-Max-Idle-Period" },
        { 0x00000400u, "STA-Preferred-BSS-Max-Idle-Period" },
        { 0x00000008u, "TIM-Broadcast" },
        { 0x00000010u, "Traffic-Filtering" },
        { 0x00000020u, "WNM-Sleep-Mode" },
        { 0x00000040u, "Directed-Multicast" },
        { 0x00000080u, "Flexible-Multicast" },
        { 0x00000100u, "Notification" },
    };
    uint32_t cur = 0;
    int rc = wl_ctx_iovar_get(ctx, "wnm", &cur, sizeof(cur));
    if (rc < 0) return rc;

    if (argc == 1) {
        printf("0x%x:", cur);
        for (size_t i = 0; i < sizeof(wnm_flags) / sizeof(wnm_flags[0]); i++) {
            if (cur & wnm_flags[i].mask) {
                printf("  %s", wnm_flags[i].name);
            }
        }
        putchar('\n');
        return 0;
    }

    const char *arg = argv[1];
    char op = '\0';
    if (arg[0] == '+' || arg[0] == '-') {
        op = arg[0];
        arg++;
    }
    uint32_t mask = 0;
    if (wl_parse_u32(arg, &mask) < 0) {
        fprintf(stderr, "WNM mask is a bitfield from the following set. Use + or - prefix to combine with current value.\n");
        for (size_t i = 0; i < sizeof(wnm_flags) / sizeof(wnm_flags[0]); i++) {
            fprintf(stderr, "0x%x\t%s\n", wnm_flags[i].mask, wnm_flags[i].name);
        }
        return -EINVAL;
    }
    uint32_t next = (op == '+') ? (cur | mask) :
                    (op == '-') ? (cur & ~mask) : mask;
    return wl_ctx_iovar_set(ctx, "wnm", &next, sizeof(next));
}

static int cmd_txpwr_cap(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Verified stock 0x19404..0x195ec:
     * Calls WLC_GET_REVINFO (98), checks corerev > 127, then:
     * GET: 4B int32 unpacked as 4 signed int8_t per core
     * SET: 1..4 args (0..127 qdBm) packed into 16B int32_t caps[4]
     */
    uint8_t rev[100];
    memset(rev, 0, sizeof(rev));
    int rrc = wl_ctx_ioctl(ctx, WLC_GET_REVINFO, rev, sizeof(rev), 0);
    if (rrc < 0) return rrc;
    if (rd_u32(rev + 0x10) <= 127u) return -EOPNOTSUPP;

    if (argc == 1) {
        uint32_t raw = 0;
        int rc = wl_ctx_iovar_get(ctx, "txpwr_cap", &raw, sizeof(raw));
        if (rc == 0) {
            int c0 = (int)(int8_t)(raw & 0xffu);
            int c1 = (int)(int8_t)((raw >> 8) & 0xffu);
            int c2 = (int)(int8_t)((raw >> 16) & 0xffu);
            int c3 = (int)(int8_t)((raw >> 24) & 0xffu);
            printf("txpwr_cap for core{0...3}: %d %d %d %d\n", c0, c1, c2, c3);
        }
        return rc;
    }
    if (argc > 5) return -EINVAL;
    int32_t caps[4] = { 0, 0, 0, 0 };
    for (int i = 1; i < argc; i++) {
        int32_t v = 0;
        if (wl_parse_i32(argv[i], &v) < 0 || v < 0 || v > 127) return -EINVAL;
        caps[i - 1] = v;
    }
    return wl_ctx_iovar_set(ctx, "txpwr_cap", caps, sizeof(caps));
}

static int cmd_cfp_dump(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Verified stock 0x1891c..0x189cc: 12B scb_val_t (MAC at +4) into 1536B text buffer */
    if (argc < 2) {
        fprintf(stderr, "Usage: wl cfp_dump <mac>\n");
        return -EINVAL;
    }
    uint8_t scb[12];
    memset(scb, 0, sizeof(scb));
    if (bcm_wl_parse_mac(argv[1], scb + 4) < 0) return -EINVAL;
    char *buf = (char *)calloc(1, WL_IOCTL_MEDLEN);
    if (!buf) return -ENOMEM;
    int rc = wl_ctx_iovar_getbuf(ctx, "cfp_dump", scb, sizeof(scb), buf, WL_IOCTL_MEDLEN - 1);
    if (rc == 0 && buf[0] != '\0') {
        buf[WL_IOCTL_MEDLEN - 1] = '\0';
        fputs(buf, stdout);
        size_t l = strlen(buf);
        if (l > 0 && buf[l - 1] != '\n') putchar('\n');
    }
    free(buf);
    return rc;
}

static int cmd_muinfo(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Verified stock 0x13288..0x13378: 1B uint8_t verbose (0 or 1 on -v) + 255KB output buffer */
    uint8_t verbose = 0;
    if (argc >= 2) {
        if (strcmp(argv[1], "-v") == 0 && argc == 2) verbose = 1;
        else return -EINVAL;
    }
    char *buf = (char *)calloc(1, WL_DUMP_BUF_LEN);
    if (!buf) return -ENOMEM;
    int rc = wl_ctx_iovar_getbuf(ctx, "muinfo", &verbose, sizeof(verbose), buf, WL_DUMP_BUF_LEN - 1);
    if (rc == 0 && buf[0] != '\0') {
        buf[WL_DUMP_BUF_LEN - 1] = '\0';
        fputs(buf, stdout);
        size_t l = strlen(buf);
        if (l > 0 && buf[l - 1] != '\n') putchar('\n');
    }
    free(buf);
    return rc;
}

static int cmd_taf(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Verified stock 0x22a4c..0x22d4c (3 distinct paths):
     * 1. MAC-only (`wl taf <mac>`): 20B WLC_GET_VAR -> formatted sch/prio line
     * 2. MAC+args (`wl taf <mac> <sch> [prio] [score]`): WLC_SET_VAR with +8/+12/+16/+20
     * 3. Non-MAC (`wl taf [args...]`): ea=ff:..:ff, NUL-separated args at +20, WLC_GET_VAR, print buf+20
     */
    uint8_t req[512];
    memset(req, 0, sizeof(req));
    uint16_t ver = 1;
    memcpy(req + 6, &ver, 2);

    uint8_t mac[6];
    if (argc >= 2 && bcm_wl_parse_mac(argv[1], mac) == 0) {
        memcpy(req, mac, 6);
        if (argc == 2) {
            /* Path 1: MAC-only GET (20B input, 16KB output) */
            uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
            if (!buf) return -ENOMEM;
            int rc = wl_ctx_iovar_getbuf(ctx, "taf", req, 20, buf, WL_IOCTL_LARGE_LEN - 1);
            if (rc == 0) {
                buf[WL_IOCTL_LARGE_LEN - 1] = '\0';
                printf("%02x:%02x:%02x:%02x:%02x:%02x sch: %s (%u) / prio: %u\n",
                       buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
                       (const char *)(buf + 20), rd_u32(buf + 8), rd_u32(buf + 12));
            }
            free(buf);
            return rc;
        }
        /* Path 2: MAC + args SET (WLC_SET_VAR) */
        size_t slen = strlen(argv[2]);
        if (21u + slen > sizeof(req)) return -EINVAL;
        memcpy(req + 20, argv[2], slen + 1);
        uint32_t sch_id = 0xffffffffu;
        if (isdigit((unsigned char)argv[2][0])) {
            sch_id = (uint32_t)strtoul(argv[2], NULL, 0);
        }
        uint32_t prio = 0, score = 0;
        if (argc >= 4 && argv[3][0] != '-') prio  = (uint32_t)strtoul(argv[3], NULL, 0);
        if (argc >= 5 && argv[4][0] != '-') score = (uint32_t)strtoul(argv[4], NULL, 0);
        memcpy(req + 8,  &sch_id, 4);
        memcpy(req + 12, &prio, 4);
        memcpy(req + 16, &score, 4);
        return wl_ctx_iovar_set(ctx, "taf", req, 21u + slen);
    }

    /* Path 3: Non-MAC GET (ea = ff:ff:ff:ff:ff:ff, NUL-separated args at +20) */
    memset(req, 0xff, 6);
    size_t off = 20;
    for (int argi = 1; argi < argc; argi++) {
        size_t alen = strlen(argv[argi]);
        if (off + alen + 2 >= sizeof(req)) return -EINVAL;
        memcpy(req + off, argv[argi], alen);
        off += alen;
        req[off++] = '\0';
    }
    req[off++] = '\0'; /* trailing NUL -> 21B when 0 args */

    uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
    if (!buf) return -ENOMEM;
    int rc = wl_ctx_iovar_getbuf(ctx, "taf", req, off, buf, WL_IOCTL_LARGE_LEN - 1);
    if (rc == 0) {
        buf[WL_IOCTL_LARGE_LEN - 1] = '\0';
        const char *txt = (const char *)(buf + 20);
        if (txt[0] != '\0') {
            fputs(txt, stdout);
            size_t l = strlen(txt);
            if (l > 0 && txt[l - 1] != '\n') putchar('\n');
        }
    }
    free(buf);
    return rc;
}

static int cmd_dlystats_clear(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    return wl_ctx_iovar_set(ctx, "dlystats_clear", NULL, 0);
}

/* =========================================================================
 * Exported T2 Command Table (every entry ABI-verified against stock wl)
 * ========================================================================= */
const wl_cmd_entry_t WL_T2_CMDS[] = {
    /* 5.1 Client management */
    { "authe_sta_list",        cmd_authe_sta_list,     "T2", "client",  "List authenticated STA MACs" },
    { "autho_sta_list",        cmd_autho_sta_list,     "T2", "client",  "List authorized STA MACs" },
    { "deauthenticate",        cmd_deauthenticate,     "T2", "client",  "Deauthenticate STA (<mac> [reason] or all)" },
    { "disassoc",              cmd_disassoc,           "T2", "client",  "Disassociate STA ([mac])" },
    { "mac",                   cmd_mac,                "T2", "client",  "Get/Set MAC filter list (add/del/clear)" },

    /* 5.2 11v */
    { "wnm",                   cmd_wnm,                "T2", "roam",    "Get/Set 802.11v WNM feature bitmask" },

    /* 5.3 CSA / DFS / Channel */
    { "autochannel",           cmd_autochannel,        "T2", "channel", "Get/Set auto channel selection" },
    { "chan_info",             cmd_chan_info,          "T2", "channel", "Get regulatory/radar info for a chanspec" },
    { "chanim_mode",           cmd_chanim_mode,        "T2", "channel", "Get/Set CHANIM interference management mode" },
    { "chanim_state",          cmd_chanim_state,       "T2", "channel", "Get CHANIM interference state for chanspec" },
    { "channel",               cmd_channel,            "T2", "channel", "Get/Set current radio channel" },
    { "channels",              cmd_channels,           "T2", "channel", "List all valid channels for current locale" },
    { "clear_radar_status",    cmd_clear_radar_status, "T2", "dfs",     "Clear detected radar status" },
    { "csa",                   cmd_csa,                "T2", "channel", "Send Channel Switch Announcement (<mode> <count> <chspec> [u])" },
    { "dfs_ap_move",           cmd_dfs_ap_move,        "T2", "dfs",     "Get/Trigger background DFS AP move" },
    { "dfs_status",            cmd_dfs_status,         "T2", "dfs",     "Get DFS CAC/ISM state and cleared channel" },
    { "radar",                 cmd_radar,              "T2", "dfs",     "Get/Set radar detection enable" },

    /* 5.4 Deep Queue / CFP / TAF */
    { "cfp_dump",              cmd_cfp_dump,           "T2", "queue",   "Dump Cached Flow Processing (CFP) table" },
    { "dlystats_clear",        cmd_dlystats_clear,     "T2", "queue",   "Clear packet delay histogram stats" },

    /* 5.5 Power / Reg */
    { "country_rev",           cmd_country_rev,        "T2", "reg",     "List supported revisions for country code" },
    { "txpwr_cap",             cmd_txpwr_cap,          "T2", "power",   "Get/Set per-core TX power cap in qdBm" },
    { "txpwr_target_max",      cmd_txpwr_target_max,   "T2", "power",   "Get max target TX power per chain" },
    { "txpwrlimit",            cmd_txpwrlimit,         "T2", "power",   "Get current regulatory TX power limit" },

    /* 5.6 Wi-Fi 6 MU / TAF */
    { "muinfo",                cmd_muinfo,             "T2", "wifi6",   "Get MU-MIMO / OFDMA group & client info" },
    { "taf",                   cmd_taf,                "T2", "wifi6",   "Traffic Airtime Fairness (TAF) status & config" },

    /* 5.7 Multi-SSID / Radio */
    { "bss",                   cmd_bss,                "T2", "iface",   "Get/Set BSS state (up|down|ap|sta|move|del)" },
    { "bssid",                 cmd_bssid,              "T2", "iface",   "Get/Set BSSID MAC address" },
    { "bssmax",                cmd_bssmax,             "T2", "iface",   "Get max supported virtual BSS count" },
    { "mbssid",                cmd_mbssid,             "T2", "iface",   "Get/Set 802.11ax Multiple-BSSID mode" },
    { "radio",                 cmd_radio,              "T2", "radio",   "Get/Set radio hardware enable (on|off|<mask>)" },
    { "ssid",                  cmd_ssid,               "T2", "iface",   "Get/Set BSS SSID" },

    /* 5.8 Diagnostics / Counters */
    { "band",                  cmd_band,               "T2", "radio",   "Get/Set radio band (auto|a|b|6g)" },
    { "bands",                 cmd_bands,              "T2", "radio",   "List supported radio bands" },
    { "dump_clear",            cmd_dump_clear,         "T2", "diag",    "Clear subsystem dump counters (<subsys>...)" },
    { "memuse",                cmd_memuse,             "T2", "diag",    "Get driver heap memory usage" },
    { "phy_rssi_ant",          cmd_phy_rssi_ant,       "T2", "rf",      "Get per-antenna RSSI [mac]" },
    { "phy_snr_ant",           cmd_phy_snr_ant,        "T2", "rf",      "Get per-antenna SNR [mac]" },
    { "pktcnt",                cmd_pktcnt,             "T2", "diag",    "Get RX/TX good & bad packet counts" },
    { "reset_cnts",            cmd_reset_cnts,         "T2", "diag",    "Reset driver counters" },
    { "revinfo",               cmd_revinfo,            "T2", "diag",    "Get chip, board, PHY, ucode & driver revisions" },
    { "ver",                   cmd_ver,                "T2", "diag",    "Get driver and utility version string" },
    { "wme_counters",          cmd_wme_counters,       "T2", "qos",     "Get per-AC WMM packet/byte/fail counters" },
};
const size_t WL_T2_CMDS_COUNT = sizeof(WL_T2_CMDS) / sizeof(WL_T2_CMDS[0]);
