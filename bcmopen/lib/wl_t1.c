/*
 * wl_t1.c — T1 Core AP Custom Command Handlers for bcmopen wl
 *
 * Implements all 19 T1 custom handlers (plus `dump` in wl_core.c and the 7 T1
 * generic entries in wl_table.h = 100% of T1 commands used by ap-panel,
 * tune.sh, and /etc/init.d/bcm-hw).
 *
 * Verified directly against stock ARMv7 wl (driver 17.10.251.3202) disassembly.
 */
#include "wl_internal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* =========================================================================
 * Shared Little-Endian & Formatting Helpers
 * ========================================================================= */
static inline uint16_t rd_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static inline int16_t rd_i16(const uint8_t *p) {
    int16_t v;
    memcpy(&v, p, 2);
    return v;
}
static inline uint32_t rd_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline int32_t rd_i32(const uint8_t *p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline uint64_t rd_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void print_rateset(const uint8_t *rates, uint32_t count) {
    if (count > 16) count = 16;
    printf("[ ");
    for (uint32_t i = 0; i < count; i++) {
        uint8_t r = rates[i] & 0x7fu;
        int basic = (rates[i] & 0x80u) != 0;
        if (r == 0) continue;
        if (r & 1) printf("%u.5%s ", (unsigned)(r / 2), basic ? "(b)" : "");
        else       printf("%u%s ", (unsigned)(r / 2), basic ? "(b)" : "");
    }
    printf("]");
}

/* =========================================================================
 * 1. assoclist (WLC_GET_ASSOCLIST = 159)
 * ========================================================================= */
static int cmd_assoclist(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
    if (!buf) return -ENOMEM;

    uint32_t max_cnt = 0xaaa; /* matches stock 0x414cc */
    memcpy(buf, &max_cnt, 4);

    int rc = wl_ctx_ioctl(ctx, WLC_GET_ASSOCLIST, buf, WL_IOCTL_LARGE_LEN, 0);
    if (rc == 0) {
        uint32_t count = rd_u32(buf);
        if (count > 0xaaa) count = 0xaaa;
        for (uint32_t i = 0; i < count; i++) {
            char mac_s[18];
            bcm_wl_format_mac(buf + 4 + i * 6, mac_s, 0);
            printf("assoclist %s\n", mac_s);
        }
    }
    free(buf);
    return rc;
}

/* =========================================================================
 * 2. rssi (WLC_GET_RSSI = 127)
 * ========================================================================= */
static int cmd_rssi(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc >= 2) {
        /* 12-byte scb_val_t: { int32_t val; uint8_t ea[6]; uint16_t pad; } */
        uint8_t scb[12];
        memset(scb, 0, sizeof(scb));
        if (bcm_wl_parse_mac(argv[1], scb + 4) < 0) {
            fprintf(stderr, " ERROR: no valid ether addr provided\n");
            return -EINVAL;
        }
        int rc = wl_ctx_ioctl(ctx, WLC_GET_RSSI, scb, sizeof(scb), 0);
        if (rc == 0) printf("%d\n", rd_i32(scb));
        return rc;
    }
    int32_t val = 0;
    int rc = wl_ctx_ioctl(ctx, WLC_GET_RSSI, &val, sizeof(val), 0);
    if (rc == 0) printf("%d\n", (int)(int8_t)(val & 0xff));
    return rc;
}

/* =========================================================================
 * 3. bw_cap (IOVAR "bw_cap", 8 bytes: { uint32_t band; uint32_t cap; })
 * ========================================================================= */
static int parse_bw_band(const char *s, uint32_t *out_band) {
    if (strcmp(s, "a") == 0 || strcmp(s, "5") == 0 || strcasecmp(s, "5g") == 0) {
        *out_band = 1; /* WLC_BAND_5G */
        return 0;
    }
    if (strcmp(s, "b") == 0 || strcmp(s, "2") == 0 || strcasecmp(s, "2g") == 0) {
        *out_band = 2; /* WLC_BAND_2G */
        return 0;
    }
    if (strcmp(s, "6") == 0 || strcasecmp(s, "6g") == 0) {
        *out_band = 4; /* WLC_BAND_6G */
        return 0;
    }
    return -EINVAL;
}

static int cmd_bw_cap(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "bw_cap: band unspecified\n");
        return -EINVAL;
    }
    uint32_t req[2] = { 0, 0 };
    if (parse_bw_band(argv[1], &req[0]) < 0) {
        fprintf(stderr, "bw_cap: invalid band %s\n", argv[1]);
        return -EINVAL;
    }
    if (argc >= 3) {
        if (wl_parse_u32(argv[2], &req[1]) < 0) {
            fprintf(stderr, "bw_cap: invalid bandwidth '%s'\n", argv[2]);
            return -EINVAL;
        }
        if (req[1] != 0) {
            return wl_ctx_iovar_set(ctx, "bw_cap", req, sizeof(req));
        }
    }
    uint32_t out_cap = 0;
    int rc = wl_ctx_iovar_getbuf(ctx, "bw_cap", req, sizeof(req), &out_cap, sizeof(out_cap));
    if (rc == 0) printf("0x%x\n", out_cap);
    return rc;
}

/* =========================================================================
 * 4. chanspec (IOVAR "chanspec", 4-byte uint32)
 * ========================================================================= */
static int cmd_chanspec(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        int32_t val = 0;
        int rc = wl_ctx_iovar_getint(ctx, "chanspec", &val);
        if (rc == 0) {
            uint16_t cs = (uint16_t)(val & 0xffffu);
            char s[32];
            bcm_wl_chspec_to_str(cs, s, sizeof(s));
            printf("%s (0x%x)\n", s, (unsigned)cs);
        }
        return rc;
    }

    uint16_t cs = 0;
    if (argv[1][0] != '-') {
        if (bcm_wl_chspec_from_str(argv[1], &cs) < 0) {
            fprintf(stderr, "chanspec: invalid chanspec '%s'\n", argv[1]);
            return -EINVAL;
        }
    } else {
        /* Option form: -c <ch> -b <2|5|6> -w <20|40|80|160> -s <-1|0|1> */
        if (((argc - 1) & 1) != 0) return -EINVAL;
        int32_t ch = 0, band = 0, bw = 20, sb = 0;
        for (int i = 1; i + 1 < argc; i += 2) {
            int32_t v = 0;
            if (wl_parse_i32(argv[i + 1], &v) < 0) return -EINVAL;
            if (strcmp(argv[i], "-c") == 0)      ch = v;
            else if (strcmp(argv[i], "-b") == 0) { if (v != 2 && v != 5 && v != 6) return -EINVAL; band = v; }
            else if (strcmp(argv[i], "-w") == 0) { if (v != 20 && v != 40 && v != 80 && v != 160) return -EINVAL; bw = v; }
            else if (strcmp(argv[i], "-s") == 0) { if (v != -1 && v != 0 && v != 1) return -EINVAL; sb = v; }
            else return -EINVAL;
        }
        if (ch <= 0 || ch > 224) return -EINVAL;
        uint16_t bbits = (band == 5) ? WL_CHANSPEC_BAND_5G :
                         (band == 6) ? WL_CHANSPEC_BAND_6G :
                         (band == 2) ? WL_CHANSPEC_BAND_2G :
                         ((ch <= 14) ? WL_CHANSPEC_BAND_2G : WL_CHANSPEC_BAND_5G);
        uint16_t wbits = (bw == 160) ? WL_CHANSPEC_BW_160 :
                         (bw == 80)  ? WL_CHANSPEC_BW_80  :
                         (bw == 40)  ? WL_CHANSPEC_BW_40  : WL_CHANSPEC_BW_20;
        uint16_t sbits = (sb == 1) ? (1u << WL_CHANSPEC_CTL_SB_SHIFT) : 0u;
        cs = (uint16_t)(bbits | wbits | sbits | (uint16_t)(ch & 0xff));
    }

    int32_t val = (int32_t)cs;
    int rc = wl_ctx_iovar_setint(ctx, "chanspec", val);
    if (rc == 0) {
        printf("Chanspec set to 0x%x\n", (unsigned)cs);
    }
    return rc;
}

/* =========================================================================
 * 5. country (IOVAR "country" 12B wl_country_t + WLC_GET_COUNTRY_LIST = 261)
 * ========================================================================= */
static const char *lookup_country_name(const char *cc) {
    static const struct { const char *code; const char *name; } tbl[] = {
        { "CN", "CHINA" }, { "US", "UNITED STATES" }, { "JP", "JAPAN" },
        { "GB", "UNITED KINGDOM" }, { "DE", "GERMANY" }, { "FR", "FRANCE" },
        { "KR", "KOREA, REPUBLIC OF" }, { "TW", "TAIWAN, PROVINCE OF CHINA" },
        { "HK", "HONG KONG" }, { "SG", "SINGAPORE" }, { "AU", "AUSTRALIA" },
        { "CA", "CANADA" }, { "EU", "EUROPEAN UNION" }, { "RU", "RUSSIAN FEDERATION" },
        { "IN", "INDIA" }, { "BR", "BRAZIL" }, { "ALL", "ALL" }, { "XZ", "WORLDWIDE" },
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (strcasecmp(cc, tbl[i].code) == 0) return tbl[i].name;
    }
    return "";
}

static int cmd_country(const wl_ctx_t *ctx, int argc, char **argv) {
    typedef struct {
        char    country_abbrev[4];
        int32_t rev;
        char    ccode[4];
    } wl_country_t;

    if (argc == 1) {
        wl_country_t c;
        memset(&c, 0, sizeof(c));
        int rc = wl_ctx_iovar_get(ctx, "country", &c, sizeof(c));
        if (rc < 0) return rc;
        c.country_abbrev[3] = '\0';
        c.ccode[3] = '\0';
        const char *cname = lookup_country_name(c.ccode[0] ? c.ccode : c.country_abbrev);
        if (c.rev >= 0) {
            printf("%s (%s/%d) %s\n", c.country_abbrev, c.ccode, c.rev, cname);
        } else {
            printf("%s %s\n", c.country_abbrev, cname);
        }
        return 0;
    }

    if (strcasecmp(argv[1], "list") == 0) {
        uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
        if (!buf) return -ENOMEM;
        uint32_t buflen = WL_IOCTL_LARGE_LEN;
        uint32_t band_set = 0, band = 0;
        if (argc >= 3) {
            if (parse_bw_band(argv[2], &band) < 0) {
                free(buf);
                return -EINVAL;
            }
            band_set = 1;
        }
        memcpy(buf + 0, &buflen, 4);
        memcpy(buf + 4, &band_set, 4);
        memcpy(buf + 8, &band, 4);
        int rc = wl_ctx_ioctl(ctx, 261 /* WLC_GET_COUNTRY_LIST */, buf, WL_IOCTL_LARGE_LEN, 0);
        if (rc == 0) {
            uint32_t count = rd_u32(buf + 12);
            if (count > (WL_IOCTL_LARGE_LEN - 16) / 4) count = (WL_IOCTL_LARGE_LEN - 16) / 4;
            for (uint32_t i = 0; i < count; i++) {
                char cc[5];
                memcpy(cc, buf + 16 + i * 4, 4);
                cc[4] = '\0';
                printf("%s ", cc);
            }
            putchar('\n');
        }
        free(buf);
        return rc;
    }

    /* Set country: "<CC>" or "<CC>/<rev>" */
    wl_country_t c;
    memset(&c, 0, sizeof(c));
    c.rev = -1;
    const char *slash = strchr(argv[1], '/');
    if (slash) {
        size_t clen = (size_t)(slash - argv[1]);
        if (clen == 0 || clen > 3) return -EINVAL;
        memcpy(c.ccode, argv[1], clen);
        memcpy(c.country_abbrev, argv[1], clen);
        if (wl_parse_i32(slash + 1, &c.rev) < 0) return -EINVAL;
    } else {
        size_t clen = strlen(argv[1]);
        if (clen == 0 || clen > 3) return -EINVAL;
        memcpy(c.ccode, argv[1], clen);
        memcpy(c.country_abbrev, argv[1], clen);
    }
    return wl_ctx_iovar_set(ctx, "country", &c, sizeof(c));
}

/* =========================================================================
 * 6. spect (WLC_GET_SPECT_MANAGMENT = 245, WLC_SET_SPECT_MANAGMENT = 244)
 * ========================================================================= */
static int cmd_spect(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        int32_t val = 0;
        int rc = wl_ctx_ioctl(ctx, WLC_GET_SPECT_MANAGMENT, &val, sizeof(val), 0);
        if (rc < 0) return rc;
        switch (val) {
        case 0: puts("Off"); break;
        case 1: puts("Loose interpretation of 11h spec - may join non 11h AP."); break;
        case 2: puts("Strict interpretation of 11h spec - may not join non 11h AP."); break;
        case 3: puts("802.11d mode"); break;
        case 4: puts("Loose interpretation of 11h+d spec - may join non-11h APs"); break;
        default: printf("invalid value 0x%x\n", (unsigned)val); return -EINVAL;
        }
        return 0;
    }
    int32_t val = 0;
    if (wl_parse_i32(argv[1], &val) < 0 || val < 0 || val > 4) return -EINVAL;
    return wl_ctx_ioctl(ctx, WLC_SET_SPECT_MANAGMENT, &val, sizeof(val), 1);
}

/* =========================================================================
 * 7. cur_etheraddr (IOVAR "cur_etheraddr", 6B MAC, colon format)
 * ========================================================================= */
static int cmd_cur_etheraddr(const wl_ctx_t *ctx, int argc, char **argv) {
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
        int rc = bcm_wl_bsscfg_iovar_get(ctx->ifname, bss, "cur_etheraddr", mac, sizeof(mac));
        if (rc == 0) {
            char s[18];
            bcm_wl_format_mac(mac, s, 0);
            printf("cur_etheraddr: %s\n", s);
        }
        return rc;
    }
    uint8_t mac[6];
    if (bcm_wl_parse_mac(argv[argi], mac) < 0) {
        fprintf(stderr, " ERROR: no valid ether addr provided\n");
        return -EINVAL;
    }
    return bcm_wl_bsscfg_iovar_set(ctx->ifname, bss, "cur_etheraddr", mac, sizeof(mac));
}

/* =========================================================================
 * 8. txpwr1 (IOVAR "qtxpower", 4B int with bit 31 = override)
 * ========================================================================= */
static uint16_t qdbm_to_mw(int qdbm) {
    if (qdbm <= 3) return (qdbm < 0) ? 0 : 1;
    double dbm = (double)qdbm / 4.0;
    double mw = pow(10.0, dbm / 10.0);
    if (mw >= 65535.0) return 65535;
    return (uint16_t)(mw + 0.5);
}

static uint8_t mw_to_qdbm(unsigned int mw) {
    if (mw == 0) return 0;
    double q = 40.0 * log10((double)mw);
    if (q < 0.0) return 0;
    if (q > 127.0) return 127;
    return (uint8_t)(q + 0.5);
}

static int cmd_txpwr1(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        int32_t raw = 0;
        int rc = wl_ctx_iovar_getint(ctx, "qtxpower", &raw);
        if (rc < 0) return rc;
        int override_on = (raw < 0); /* bit 31 set */
        int qdbm = (int)(int8_t)(raw & 0xff);
        int abs_q = (qdbm < 0) ? -qdbm : qdbm;
        int whole = abs_q / 4;
        int frac = (abs_q % 4) * 25;
        uint16_t mw = qdbm_to_mw(qdbm);
        printf("TxPower is %d qdbm, %s%d.%d dbm, %u mW  Override is %s\n",
               qdbm, (qdbm < 0) ? "-" : "", whole, frac,
               (unsigned)mw, override_on ? "On" : "Off");
        return 0;
    }

    int override_on = 0;
    int unit = 3; /* 1=mW, 2=qdbm, 3=dBm (stock 0x20834 default = dBm!) */
    int unit_explicit = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0' && !isdigit((unsigned char)argv[i][1])) {
        if (strcmp(argv[i], "-o") == 0)      override_on = 1;
        else if (strcmp(argv[i], "-d") == 0) { unit = 3; unit_explicit = 1; }
        else if (strcmp(argv[i], "-q") == 0) { unit = 2; unit_explicit = 1; }
        else if (strcmp(argv[i], "-m") == 0) { unit = 1; unit_explicit = 1; }
        else return -EINVAL;
        i++;
    }
    if (i >= argc) return -EINVAL;

    int32_t in_val = 0;
    if (wl_parse_i32(argv[i], &in_val) < 0) return -EINVAL;

    uint32_t qval = 0;
    if (!unit_explicit) {
        if (in_val == -1) {
            qval = 127; /* stock 0x20860: -1 restores default (31.75 dBm), still honors -o */
        } else if (in_val <= 0 || in_val > 31) {
            return -EINVAL;
        } else {
            qval = (uint8_t)(in_val * 4); /* default unit is dBm */
        }
    } else if (unit == 3) {
        if (in_val > 31 || in_val < -32) return -EINVAL;
        qval = (uint8_t)(in_val * 4);
    } else if (unit == 2) {
        if (in_val > 127 || in_val < -128) return -EINVAL;
        qval = (uint8_t)in_val;
    } else {
        if (in_val <= 0) return -EINVAL;
        qval = mw_to_qdbm((unsigned int)in_val);
    }
    if (override_on) qval |= 0x80000000u;
    return wl_ctx_iovar_setint(ctx, "qtxpower", (int32_t)qval);
}

/* =========================================================================
 * 9. wme_ac (IOVAR "wme_ac_ap" / "wme_ac_sta", 16B get / 4B set)
 * ========================================================================= */
static int cmd_wme_ac(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc < 2) return -EINVAL;
    const char *iov = NULL;
    if (strcmp(argv[1], "ap") == 0)       iov = "wme_ac_ap";
    else if (strcmp(argv[1], "sta") == 0) iov = "wme_ac_sta";
    else return -EINVAL;

    uint8_t acp[16];
    memset(acp, 0, sizeof(acp));
    int rc = wl_ctx_iovar_get(ctx, iov, acp, sizeof(acp));
    if (rc < 0) return rc;

    static const char *ac_names[4] = { "AC_BE", "AC_BK", "AC_VI", "AC_VO" };

    if (argc == 2) {
        puts("AC Parameters");
        for (int idx = 0; idx < 4; idx++) {
            uint8_t aci_raw = acp[idx * 4 + 0];
            uint8_t ecw_raw = acp[idx * 4 + 1];
            uint16_t txop   = rd_u16(acp + idx * 4 + 2);
            int aci    = (aci_raw >> 5) & 3;
            int acm    = (aci_raw >> 4) & 1;
            int aifsn  = aci_raw & 0x0f;
            int ecwmin = ecw_raw & 0x0f;
            int ecwmax = (ecw_raw >> 4) & 0x0f;
            int cwmin  = (1 << ecwmin) - 1;
            int cwmax  = (1 << ecwmax) - 1;
            printf("%s: raw: ACI 0x%x ECW 0x%x TXOP 0x%x\n",
                   ac_names[idx], aci_raw, ecw_raw, (unsigned)txop);
            printf("       dec: aci %d acm %d aifsn %d ecwmin %d ecwmax %d txop 0x%x\n",
                   aci, acm, aifsn, ecwmin, ecwmax, (unsigned)txop);
            printf("       eff: CWmin %d CWmax %d TXop %uusec\n",
                   cwmin, cwmax, (unsigned)txop * 32u);
        }
        return 0;
    }

    int ac_idx = -1;
    if (strcasecmp(argv[2], "be") == 0)      ac_idx = 0;
    else if (strcasecmp(argv[2], "bk") == 0) ac_idx = 1;
    else if (strcasecmp(argv[2], "vi") == 0) ac_idx = 2;
    else if (strcasecmp(argv[2], "vo") == 0) ac_idx = 3;
    else return -EINVAL;

    uint8_t entry[4];
    memcpy(entry, acp + ac_idx * 4, 4);

    if (argc < 5) return -EINVAL;
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc) return -EINVAL;
        const char *k = argv[i];
        const char *v = argv[i + 1];
        if (strcmp(k, "acm") == 0) {
            int32_t b = 0;
            if (wl_parse_onoff_i32(v, &b) < 0 || (b != 0 && b != 1)) return -EINVAL;
            if (b) entry[0] |= 0x10u;
            else   entry[0] &= (uint8_t)~0x10u;
        } else if (strcmp(k, "aifsn") == 0) {
            int32_t n = 0;
            if (wl_parse_i32(v, &n) < 0 || n < 1 || n > 15) return -EINVAL;
            entry[0] = (uint8_t)((entry[0] & 0xf0u) | (uint8_t)n);
        } else if (strcmp(k, "ecwmin") == 0) {
            int32_t n = 0;
            if (wl_parse_i32(v, &n) < 0 || n < 0 || n > 15) return -EINVAL;
            entry[1] = (uint8_t)((entry[1] & 0xf0u) | (uint8_t)n);
        } else if (strcmp(k, "ecwmax") == 0) {
            int32_t n = 0;
            if (wl_parse_i32(v, &n) < 0 || n < 0 || n > 15) return -EINVAL;
            entry[1] = (uint8_t)((entry[1] & 0x0fu) | ((uint8_t)n << 4));
        } else if (strcmp(k, "txop") == 0) {
            int32_t n = 0;
            if (wl_parse_i32(v, &n) < 0 || n < 0 || n > 65535) return -EINVAL;
            uint16_t t16 = (uint16_t)n;
            memcpy(entry + 2, &t16, 2);
        } else {
            return -EINVAL;
        }
    }

    /* Matches stock 0x1b224: sends ONLY the 4-byte edcf_acparam_t of selected AC */
    return wl_ctx_iovar_set(ctx, iov, entry, sizeof(entry));
}

/* =========================================================================
 * 10. csimon (IOVAR "csimon", 56B struct + "csimon_state")
 * ========================================================================= */
static int cmd_csimon(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
        if (!buf) return -ENOMEM;
        uint32_t max_cnt = 0xaaa;
        memcpy(buf, &max_cnt, 4);
        int rc = wl_ctx_iovar_get(ctx, "csimon", buf, WL_IOCTL_LARGE_LEN);
        if (rc == 0) {
            uint32_t cnt = rd_u32(buf);
            if (cnt > 0xaaa) cnt = 0xaaa;
            for (uint32_t i = 0; i < cnt; i++) {
                char s[18];
                bcm_wl_format_mac(buf + 4 + i * 6, s, 0);
                puts(s);
            }
        }
        free(buf);
        return rc;
    }

    if (strcasecmp(argv[1], "state") == 0) {
        uint8_t buf[64];
        memset(buf, 0, sizeof(buf));
        int rc = wl_ctx_iovar_get(ctx, "csimon_state", buf, sizeof(buf));
        if (rc < 0) return rc;
        if (rd_u16(buf) != 1) return -EINVAL;
        printf("CSI Monitor: Enabled: %u\n"
               "CSI record transfer (to DDR) count: %u\n"
               "Ack fail count: %u\n"
               "Record overflow count: %u\n",
               (unsigned)buf[0x08], rd_u32(buf + 0x10),
               rd_u32(buf + 0x18), rd_u32(buf + 0x1c));
        return 0;
    }

    /* 56-byte wlc_csimon_config_t (matches stock 0x21d48..0x21f10):
     * +0x00 u32 cmd (0=del, 1=add, 2=enable, 3=disable, 4=reset_cnts, 5=addap)
     * +0x04 u8  ea[6]
     * +0x0a u16 version = 1
     * +0x0c u16 length  = 56
     * +0x10 u32 interval_ms (> 32)
     * +0x14 char ssid[32]
     * +0x34 u32 ssid_len
     */
    uint8_t cfg[56];
    memset(cfg, 0, sizeof(cfg));
    uint16_t ver = 1, len = 56;
    memcpy(cfg + 0x0a, &ver, 2);
    memcpy(cfg + 0x0c, &len, 2);

    uint32_t op = 0;
    if (strcasecmp(argv[1], "add") == 0) {
        op = 1;
        if (argc < 3 || bcm_wl_parse_mac(argv[2], cfg + 0x04) < 0) {
            puts(" ERROR: no valid ethernet address provided");
            return -EINVAL;
        }
        if (argc >= 4) {
            uint32_t iv = 0;
            if (wl_parse_u32(argv[3], &iv) < 0 || iv <= 32) {
                puts(" ERROR: Invalid monitor interval provided");
                return -EINVAL;
            }
            memcpy(cfg + 0x10, &iv, 4);
        }
    } else if (strcasecmp(argv[1], "addap") == 0) {
        op = 5;
        if (argc < 3 || bcm_wl_parse_mac(argv[2], cfg + 0x04) < 0) {
            puts(" ERROR: no valid ethernet address provided");
            return -EINVAL;
        }
        if (argc >= 4) {
            size_t slen = strlen(argv[3]);
            if (slen > 31) slen = 31;
            memcpy(cfg + 0x14, argv[3], slen);
            uint32_t slen32 = (uint32_t)slen;
            memcpy(cfg + 0x34, &slen32, 4);
        }
        if (argc >= 5) {
            uint32_t iv = 0;
            if (wl_parse_u32(argv[4], &iv) < 0 || iv <= 32) {
                puts(" ERROR: Invalid monitor interval provided");
                return -EINVAL;
            }
            memcpy(cfg + 0x10, &iv, 4);
        }
    } else if (strcasecmp(argv[1], "del") == 0) {
        op = 0;
        if (argc < 3 || bcm_wl_parse_mac(argv[2], cfg + 0x04) < 0) {
            puts(" ERROR: no valid ethernet address provided");
            return -EINVAL;
        }
    } else if (strcasecmp(argv[1], "enable") == 0) {
        op = 2;
    } else if (strcasecmp(argv[1], "disable") == 0) {
        op = 3;
    } else if (strcasecmp(argv[1], "reset_cnts") == 0) {
        op = 4;
    } else {
        return -EINVAL;
    }

    memcpy(cfg + 0x00, &op, 4);
    return wl_ctx_iovar_set(ctx, "csimon", cfg, sizeof(cfg));
}

/* =========================================================================
 * 11. sta_monitor (IOVAR "sta_monitor", 24B struct + "sta_monitor_cnt")
 * ========================================================================= */
static int cmd_sta_monitor(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
        if (!buf) return -ENOMEM;
        uint32_t max_cnt = 0xaaa;
        memcpy(buf, &max_cnt, 4);
        int rc = wl_ctx_iovar_get(ctx, "sta_monitor", buf, WL_IOCTL_LARGE_LEN);
        if (rc == 0) {
            uint32_t cnt = rd_u32(buf);
            if (cnt > 0xaaa) cnt = 0xaaa;
            for (uint32_t i = 0; i < cnt; i++) {
                char s[18];
                bcm_wl_format_mac(buf + 4 + i * 6, s, 0);
                puts(s);
            }
        }
        free(buf);
        return rc;
    }

    if (strcasecmp(argv[1], "counters") == 0) {
        uint8_t buf[32];
        memset(buf, 0, sizeof(buf));
        int rc = wl_ctx_iovar_get(ctx, "sta_monitor_cnt", buf, sizeof(buf));
        if (rc < 0) return rc;
        if (rd_u16(buf) != 1) return -EINVAL;
        printf("stamon_cnts: data %u, mngt %u, cntl %u\n",
               rd_u32(buf + 4), rd_u32(buf + 8), rd_u32(buf + 12));
        return 0;
    }

    /* 24-byte wlc_stamon_sta_config_t (matches stock 0x3332c..0x3377c):
     * +0x00 u32 cmd (0=del, 1=add, 2=enable, 3=disable, 5=reset_cnts, 6=stats, 7=monitor_time, 8=status, 9=dump)
     * +0x04 u8  ea[6]
     * +0x0a u16 version = 1
     * +0x0c u16 length  = 24
     * +0x0e u16 chanspec
     * +0x10 u32 monitor_time
     * +0x14 u32 offchan_time
     */
    uint8_t cfg[24];
    memset(cfg, 0, sizeof(cfg));
    uint16_t ver = 1, len = 24;
    memcpy(cfg + 0x0a, &ver, 2);
    memcpy(cfg + 0x0c, &len, 2);

    if (strcasecmp(argv[1], "dump") == 0) {
        uint32_t op = 9;
        memcpy(cfg, &op, 4);
        uint8_t *out = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
        if (!out) return -ENOMEM;
        int rc = wl_ctx_iovar_getbuf(ctx, "sta_monitor", cfg, sizeof(cfg), out, WL_IOCTL_LARGE_LEN);
        if (rc == 0) {
            uint32_t enabled = rd_u32(out + 0x04);
            printf("Status : %s\n", enabled ? "Enabled" : "Disabled");
            uint32_t max_sta = (WL_IOCTL_LARGE_LEN - 0x18u) / 24u;
            uint32_t nsta = rd_u32(out + 0x14);
            if (nsta > max_sta) nsta = max_sta;
            printf("Monitoring %u stations \n", nsta);
            printf("%-8s %-18s %8s   %-18s %-14s\n",
                   "AMT_Idx", "MAC_Address", "RSSI", "Chanspec", "Off-chan_Time");
            for (uint32_t i = 0; i < nsta; i++) {
                /* Each entry is 24 bytes starting at out + 0x18 (matches 0x33848..0x338f0):
                 * +0x00 u8 ea[6]
                 * +0x08 i32 rssi
                 * +0x0c i32 amt_idx (loaded from prev_ptr + 0x24 = entry + 0x0c)
                 * +0x10 u16 chanspec (loaded from prev_ptr + 0x28 = entry + 0x10)
                 * +0x14 u32 offchan_time
                 */
                const uint8_t *e = out + 0x18 + i * 24;
                int32_t amt_idx = rd_i32(e + 0x0c);
                int32_t rssi    = rd_i32(e + 0x08);
                uint16_t cs     = rd_u16(e + 0x10);
                uint32_t off_ms = rd_u32(e + 0x14);
                char cs_str[32];
                bcm_wl_chspec_to_str(cs, cs_str, sizeof(cs_str));
                printf("%-8d %02x:%02x:%02x:%02x:%02x:%02x  %5ddBm   0x%04x %-10s %10ums\n",
                       amt_idx, e[0], e[1], e[2], e[3], e[4], e[5],
                       rssi, (unsigned)cs, cs_str, off_ms);
            }
            printf("Counters : data %u, mngt %u, cntl %u\n",
                   rd_u32(out + 0x08), rd_u32(out + 0x0c), rd_u32(out + 0x10));
        }
        free(out);
        return rc;
    }

    if (strcasecmp(argv[1], "stats") == 0) {
        uint32_t op = 6;
        memcpy(cfg, &op, 4);
        if (argc < 3 || bcm_wl_parse_mac(argv[2], cfg + 0x04) < 0) {
            puts(" ERROR: no valid ether addr provided");
            return -EINVAL;
        }
        uint8_t out[256];
        memset(out, 0, sizeof(out));
        int rc = wl_ctx_iovar_getbuf(ctx, "sta_monitor", cfg, sizeof(cfg), out, sizeof(out));
        if (rc == 0 && rd_u32(out + 4) != 0) {
            printf("%02x:%02x:%02x:%02x:%02x:%02x : RSSI :%d \n",
                   out[8], out[9], out[10], out[11], out[12], out[13], rd_i32(out + 0x10));
        }
        return rc;
    }

    if (strcasecmp(argv[1], "status") == 0) {
        uint32_t op = 8;
        memcpy(cfg, &op, 4);
        uint8_t out[64];
        memset(out, 0, sizeof(out));
        int rc = wl_ctx_iovar_getbuf(ctx, "sta_monitor", cfg, sizeof(cfg), out, sizeof(out));
        if (rc == 0) printf("Status : %s\n", rd_u32(out + 4) ? "Enabled" : "Disabled");
        return rc;
    }

    uint32_t op = 0;
    if (strcasecmp(argv[1], "add") == 0) {
        op = 1;
        if (argc < 3 || bcm_wl_parse_mac(argv[2], cfg + 0x04) < 0) {
            puts(" ERROR: no valid ether addr provided");
            return -EINVAL;
        }
        if (argc >= 4) {
            uint16_t cs = 0;
            if (bcm_wl_chspec_from_str(argv[3], &cs) < 0) return -EINVAL;
            memcpy(cfg + 0x0e, &cs, 2);
            if (argc < 5) {
                puts(" ERROR: no valid off-channel time provided");
                return -EINVAL;
            }
            uint32_t ot = 0;
            if (wl_parse_u32(argv[4], &ot) < 0 || ot == 0) {
                puts(" ERROR: no valid off-channel time provided");
                return -EINVAL;
            }
            memcpy(cfg + 0x14, &ot, 4);
        }
    } else if (strcasecmp(argv[1], "del") == 0) {
        op = 0;
        if (argc < 3 || bcm_wl_parse_mac(argv[2], cfg + 0x04) < 0) {
            puts(" ERROR: no valid ether addr provided");
            return -EINVAL;
        }
    } else if (strcasecmp(argv[1], "enable") == 0) {
        op = 2;
    } else if (strcasecmp(argv[1], "disable") == 0) {
        op = 3;
    } else if (strcasecmp(argv[1], "reset_cnts") == 0) {
        op = 5;
    } else if (strcasecmp(argv[1], "monitor_time") == 0) {
        op = 7;
        uint32_t mt = 0;
        if (argc < 3 || wl_parse_u32(argv[2], &mt) < 0) {
            puts(" ERROR: Invalid monitor_time provided");
            return -EINVAL;
        }
        memcpy(cfg + 0x10, &mt, 4);
    } else {
        return -EINVAL;
    }

    memcpy(cfg + 0x00, &op, 4);
    return wl_ctx_iovar_set(ctx, "sta_monitor", cfg, sizeof(cfg));
}

/* =========================================================================
 * 12 & 13. he & twt (Broadcom XTLV subcmd dispatcher — verified 0x048dbc)
 * ========================================================================= */
typedef struct {
    const char *name;
    uint16_t    id;
    uint8_t     val_bytes; /* 1, 2, or 4 */
    uint8_t     read_only;
} xtlv_subcmd_t;

static int run_xtlv_subcmd(const wl_ctx_t *ctx, const char *iovar,
                           const xtlv_subcmd_t *tbl, size_t tbl_n,
                           int argc, char **argv) {
    /* Shortcut: `wl he` -> `wl he enab`; `wl he 0|1` -> `wl he enab 0|1` */
    const char *sub = "enab";
    const char *val_arg = NULL;
    if (argc >= 2) {
        if (strcmp(argv[1], "0") == 0 || strcmp(argv[1], "1") == 0 ||
            strcasecmp(argv[1], "on") == 0 || strcasecmp(argv[1], "off") == 0) {
            sub = "enab";
            val_arg = (strcasecmp(argv[1], "on") == 0) ? "1" :
                      (strcasecmp(argv[1], "off") == 0) ? "0" : argv[1];
        } else {
            sub = argv[1];
            if (argc >= 3) val_arg = argv[2];
        }
    }

    const xtlv_subcmd_t *e = NULL;
    for (size_t i = 0; i < tbl_n; i++) {
        if (strcmp(tbl[i].name, sub) == 0) {
            e = &tbl[i];
            break;
        }
    }
    if (!e) return -EINVAL;

    if (!val_arg) {
        /* GET: 6-byte request { u16 id, u16 len=0, u16 pad=0 } (matches 0xbc28c..0xbc2b8) */
        uint8_t req[6] = {0};
        memcpy(req, &e->id, 2);
        uint8_t resp[64] = {0};
        int rc = wl_ctx_iovar_getbuf(ctx, iovar, req, sizeof(req), resp, sizeof(resp));
        if (rc < 0) return rc;
        uint32_t v = 0;
        if (e->val_bytes == 1)      v = resp[0];
        else if (e->val_bytes == 2) v = rd_u16(resp);
        else                        v = rd_u32(resp);
        /* Stock `wl he` / `wl twt` prints "%d (0x%x)\n" for enab/features */
        printf("%u (0x%x)\n", v, v);
        return 0;
    }

    if (e->read_only) return -EOPNOTSUPP;
    int32_t iv = 0;
    if (wl_parse_i32(val_arg, &iv) < 0) return -EINVAL;

    /* SET: XTLV aligned to 4 bytes: { u16 id, u16 val_bytes, val[val_bytes], pad } */
    uint8_t pkt[16];
    memset(pkt, 0, sizeof(pkt));
    uint16_t len16 = e->val_bytes;
    memcpy(pkt + 0, &e->id, 2);
    memcpy(pkt + 2, &len16, 2);
    if (e->val_bytes == 1)      pkt[4] = (uint8_t)iv;
    else if (e->val_bytes == 2) { uint16_t v16 = (uint16_t)iv; memcpy(pkt + 4, &v16, 2); }
    else                        { uint32_t v32 = (uint32_t)iv; memcpy(pkt + 4, &v32, 4); }
    size_t pkt_len = (4u + e->val_bytes + 3u) & ~3u;
    return wl_ctx_iovar_set(ctx, iovar, pkt, pkt_len);
}

static int cmd_he(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Table directly extracted from stock 0x13b3bc */
    static const xtlv_subcmd_t tbl[] = {
        { "enab",            0,  1, 0 },
        { "features",        1,  4, 0 },
        { "testbed",         3,  1, 0 },
        { "bsr",             4,  1, 0 },
        { "cap",             7,  1, 0 },
        { "range_ext",       9,  1, 0 },
        { "rtsdurthresh",    10, 2, 0 },
        { "peduration",      11, 1, 0 },
        { "dynfrag",         13, 1, 0 },
        { "htc",             15, 4, 0 },
        { "fragtx",          17, 1, 0 },
        { "color_collision", 19, 1, 0 },
        { "muedca_support",  21, 1, 1 },
        { "sr_support",      22, 1, 1 },
        { "bfe_sts_cap",     23, 1, 1 },
        { "bfe_sts_bw160_cap", 24, 1, 1 },
    };
    return run_xtlv_subcmd(ctx, "he", tbl, sizeof(tbl) / sizeof(tbl[0]), argc, argv);
}

static int cmd_twt(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Table directly extracted from stock 0x13bb48 */
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        uint8_t req[20];
        memset(req, 0, sizeof(req));
        uint16_t id = 4, tlen = 12, ver = 2, hlen = 8;
        memcpy(req + 0, &id, 2);
        memcpy(req + 2, &tlen, 2);
        memcpy(req + 4, &ver, 2);
        memcpy(req + 6, &hlen, 2);
        if (argc >= 4 && strcmp(argv[2], "-a") == 0) {
            if (bcm_wl_parse_mac(argv[3], req + 8) < 0) return -EINVAL;
        }
        uint8_t resp[512];
        memset(resp, 0, sizeof(resp));
        int rc = wl_ctx_iovar_getbuf(ctx, "twt", req, 16, resp, sizeof(resp));
        if (rc < 0) return rc;
        uint8_t bcast = resp[0x0a], indiv = resp[0x0b];
        if (bcast == 0 && indiv == 0) {
            puts("\nNo TWT schedules available.");
            return 0;
        }
        printf("TWT schedules: broadcast=%u individual=%u\n", bcast, indiv);
        return 0;
    }
    static const xtlv_subcmd_t tbl[] = {
        { "enab", 0, 1, 0 },
    };
    return run_xtlv_subcmd(ctx, "twt", tbl, sizeof(tbl) / sizeof(tbl[0]), argc, argv);
}

/* =========================================================================
 * 14. chanim_stats (IOVAR "chanim_stats", v2/v3/v4, verified 0x99664..0x9a120)
 * ========================================================================= */
static int cmd_chanim_stats(const wl_ctx_t *ctx, int argc, char **argv) {
    int want_all = 0;
    int raw_chspec = 0;
    int decimals = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "all") == 0) {
            want_all = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            raw_chspec = 1;
        } else if (strcmp(argv[i], "-d") == 0) {
            int32_t d = 0;
            if (i + 1 >= argc || wl_parse_i32(argv[++i], &d) < 0 || d < 0 || d > 2) return -EINVAL;
            decimals = d;
        } else {
            return -EINVAL;
        }
    }

    /* First probe version with 92-byte header (matches stock 0x99a48..0x99ad8) */
    uint8_t probe_req[92];
    uint8_t probe_out[92];
    memset(probe_req, 0, sizeof(probe_req));
    memset(probe_out, 0, sizeof(probe_out));
    uint32_t probe_len = 92;
    memcpy(probe_req, &probe_len, 4);
    int rc = wl_ctx_iovar_getbuf(ctx, "chanim_stats", probe_req, sizeof(probe_req),
                                 probe_out, sizeof(probe_out));
    if (rc < 0) return rc;
    uint32_t probe_ver = rd_u32(probe_out + 4);
    if (probe_ver < 2 || probe_ver > 4) {
        fprintf(stderr, "chanim_stats: unsupported version %u\n", probe_ver);
        return -EOPNOTSUPP;
    }

    size_t buf_sz = want_all ? WL_IOCTL_MAXLEN : 92;
    uint8_t *buf = (uint8_t *)calloc(1, buf_sz);
    if (!buf) return -ENOMEM;

    uint8_t req[92];
    memset(req, 0, sizeof(req));
    uint32_t req_buflen = (uint32_t)buf_sz;
    uint32_t req_cnt = want_all ? 255u : 1u;
    memcpy(req + 0, &req_buflen, 4);
    memcpy(req + 8, &req_cnt, 4);

    rc = wl_ctx_iovar_getbuf(ctx, "chanim_stats", req, sizeof(req), buf, buf_sz);
    if (rc < 0) {
        free(buf);
        return rc;
    }

    uint32_t version = rd_u32(buf + 4);
    uint32_t count   = rd_u32(buf + 8);
    printf("version: %u \n", version);

    if (count == 0) {
        free(buf);
        return 0;
    }

    /* Version 4 (live HG6371F 17.10.251.3202, matches wl1_samples.txt lines 100-102) */
    if (want_all) {
        puts("CHAN Interference Measurement:");
        puts("Stats during last scan:");
    }
    puts("chspec\ttx\tinbss\tobss\tnocat\tnopkt\tdoze\ttxop\tgoodtx\tbadtx\tglitch\tbadplcp\tknoise\tidle\tbusy\ttimestamp");
    size_t stride = (version >= 4) ? 80u : 36u;
    uint32_t max_entries = (buf_sz > 12u) ? (uint32_t)((buf_sz - 12u) / stride) : 0u;
    if (count > max_entries) count = max_entries;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = buf + 12u + i * stride;
        uint16_t cs = rd_u16(e + 0x12);
        if (raw_chspec) {
            printf("0x%4x\t", (unsigned)cs);
        } else {
            char s[32];
            bcm_wl_chspec_to_str(cs, s, sizeof(s));
            printf("%s\t", s);
        }

        if (version >= 4) {
            uint32_t total_tm = rd_u32(e + 0x48);
            double denom = (double)total_tm * 10.0;
            uint32_t busy_sum = 0;
            for (int k = 0; k < 9; k++) {
                uint32_t v = rd_u32(e + 0x24 + (size_t)k * 4u);
                if (k < 5) busy_sum += v;
                double pct = (total_tm > 0) ? ((double)v / denom) : 0.0;
                if (pct > 100.0) pct = 100.0;
                if (decimals == 0) {
                    /* Matches stock Broadcom wl (%.0f on pct+0.5), clamped <= 100 */
                    double disp = pct + 0.5;
                    if (disp > 100.0) disp = 100.0;
                    printf("%.0f\t", disp);
                } else {
                    printf("%.*f\t", decimals, pct);
                }
            }
            double busy_pct = (total_tm > 0) ? ((double)busy_sum / denom) : 0.0;
            if (busy_pct > 100.0) busy_pct = 100.0;
            printf("%u\t%u\t%d\t%u\t%.*f\t%u\n",
                   rd_u32(e + 0x00), rd_u32(e + 0x04), (int)(int8_t)e[0x11],
                   (unsigned)e[0x20], decimals,
                   (decimals == 0) ? floor(busy_pct + 0.5) : busy_pct,
                   rd_u32(e + 0x14));
        } else {
            for (int k = 0; k < 9; k++) printf("%u\t", (unsigned)e[0x08 + k]);
            printf("%u\t%u\t%d\t%u\t0\t%u\n",
                   rd_u32(e + 0x00), rd_u32(e + 0x04), (int)(int8_t)e[0x11],
                   (unsigned)e[0x20], rd_u32(e + 0x14));
        }
    }

    if (want_all) putchar('\n');
    free(buf);
    return 0;
}

/* =========================================================================
 * 15. sta_info (IOVAR "sta_info", v4/v5/v7/v8, verified 0x46590..0x483a4)
 * ========================================================================= */
static uint32_t calc_median_rate(const uint8_t *arr) {
    /* Matches stock 0x3e09c: 16 uint32_t entries, sorted ascending, returns median */
    uint32_t vals[16];
    for (int i = 0; i < 16; i++) vals[i] = rd_u32(arr + i * 4);
    for (int i = 0; i < 16; i++) {
        for (int j = i + 1; j < 16; j++) {
            if (vals[j] < vals[i]) {
                uint32_t tmp = vals[i];
                vals[i] = vals[j];
                vals[j] = tmp;
            }
        }
    }
    return vals[8];
}

static void print_one_sta_info(const uint8_t *s, size_t max_len) {
    if (max_len < 160) return;
    uint16_t ver = rd_u16(s + 0x00);
    uint16_t len = rd_u16(s + 0x02);
    uint32_t flags = rd_u32(s + 0x08);
    uint32_t idle  = rd_u32(s + 0x0c);
    char mac_s[18];
    bcm_wl_format_mac(s + 0x10, mac_s, 0);

    printf("[VER %u] STA %s:\n", (unsigned)ver, mac_s);

    if (ver >= 5 && max_len >= 206) {
        uint16_t cs = (ver == 5) ? rd_u16(s + 200) : rd_u16(s + 204);
        char cs_s[32];
        bcm_wl_chspec_to_str(cs, cs_s, sizeof(cs_s));
        printf("\t chanspec %s (0x%x)\n", cs_s, (unsigned)cs);
    }

    uint16_t aid = rd_u16(s + 152) & 0x3fffu;
    printf("\t aid:%u \n", (unsigned)aid);
    printf("\t rateset ");
    print_rateset(s + 28, rd_u32(s + 0x18));
    printf("\n");
    printf("\t idle %u seconds\n", idle);
    printf("\t in network %u seconds\n", rd_u32(s + 0x2c));
    printf("\t state:%s%s%s\n",
           (flags & 0x08u) ? " AUTHENTICATED" : "",
           (flags & 0x10u) ? " ASSOCIATED" : "",
           (flags & 0x20u) ? " AUTHORIZED" : "");

    printf("\t flags 0x%x:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
           flags,
           (flags & (1u << 0))  ? " BRCM" : "",
           (flags & (1u << 1))  ? " WME" : "",
           (flags & (1u << 8))  ? " PS" : "",
           (flags & (1u << 2))  ? " No-ERP" : "",
           (flags & (1u << 9))  ? " APSD_BE" : "",
           (flags & (1u << 10)) ? " APSD_BK" : "",
           (flags & (1u << 11)) ? " APSD_VI" : "",
           (flags & (1u << 12)) ? " APSD_VO" : "",
           (flags & (1u << 13)) ? " N_CAP" : "",
           (flags & (1u << 20)) ? " VHT_CAP" : "",
           (flags & (1u << 22)) ? " HE_CAP" : "",
           (flags & (1u << 15)) ? " AMPDU" : "",
           (flags & (1u << 16)) ? " AMSDU" : "",
           (flags & (1u << 17)) ? " MIMO-PS" : "",
           (flags & (1u << 18)) ? " MIMO-PS-RTS" : "",
           (flags & (1u << 19)) ? " RIFS" : "",
           (flags & (1u << 21)) ? " WPS" : "",
           (flags & (1u << 23)) ? " GBL_RCLASS" : "",
           (flags & (1u << 27)) ? " MBO_CAP" : "",
           (flags & (1u << 30)) ? " EHT_CAP" : "");

    uint16_t ht_caps = rd_u16(s + 154);
    printf("\t HT caps 0x%x:%s%s%s%s%s%s%s%s%s\n",
           (unsigned)ht_caps,
           (ht_caps & 0x0001u) ? " LDPC" : "",
           (ht_caps & 0x0002u) ? " 40MHz" : " ",
           (ht_caps & 0x0010u) ? " GF" : "",
           (ht_caps & 0x0020u) ? " SGI20" : "",
           (ht_caps & 0x0040u) ? " SGI40" : "",
           (ht_caps & 0x0080u) ? " STBC-Tx" : "",
           (ht_caps & 0x0300u) ? " STBC-Rx" : "",
           (ht_caps & 0x0400u) ? " D-BlockAck" : "",
           (ht_caps & 0x4000u) ? " 40-Intl" : "");

    if (flags & (1u << 20)) {
        uint16_t vc = rd_u16(s + 156);
        printf("\t VHT caps 0x%x:%s%s%s%s%s%s%s%s%s%s%s\n",
               (unsigned)vc,
               (vc & 0x0001u) ? " LDPC" : "",
               (vc & 0x0002u) ? " SGI80" : "",
               (vc & 0x0004u) ? " SGI160" : "",
               (vc & 0x0008u) ? " STBC-Tx" : "",
               (vc & 0x0010u) ? " STBC-Rx" : "",
               (vc & 0x0020u) ? " SU-BFR" : "",
               (vc & 0x0040u) ? " SU-BFE" : "",
               (vc & 0x0080u) ? " MU-BFR" : "",
               (vc & 0x0100u) ? " MU-BFE" : "",
               (vc & 0x0200u) ? " TXOPPS" : "",
               (vc & 0x0400u) ? " VHT-HTC" : "");
    }

    if (ver >= 7 && (flags & (1u << 22)) && max_len >= 298) {
        uint16_t hc = rd_u16(s + 296);
        printf("\t HE caps 0x%x:%s%s%s%s%s%s%s\n",
               (unsigned)hc,
               (hc & 0x0001u) ? " LDPC" : "",
               (hc & 0x0002u) ? " STBC-Tx" : "",
               (hc & 0x0004u) ? " STBC-Rx" : "",
               (hc & 0x0008u) ? " HE-HTC" : "",
               (hc & 0x0010u) ? " SU-BFR" : "",
               (hc & 0x0020u) ? " SU&MU-BFE" : "",
               (hc & 0x0040u) ? " MU-BFR" : "");
    }

    if (ver >= 8 && (flags & (1u << 22)) && max_len >= 0x148) {
        static const char *bw_s[8] = { "20", "40", "80", "160", "320", "?", "?", "?" };
        uint16_t omi = rd_u16(s + 0x146);
        printf("\t OMI 0x%04x: %sMhz rx=%dss tx=%dss \n",
               (unsigned)omi, bw_s[(omi >> 6) & 7],
               (int)(omi & 0x0f) + 1, (int)((omi >> 12) & 0x0f) + 1);
        uint8_t twt = s[0x145];
        if (twt != 0) {
            printf("\t TWT info 0x%x:%s%s%s\n",
                   (unsigned)twt,
                   (twt & 1) ? " CAPABLE" : "",
                   (twt & 2) ? " NEGOTIATED" : "",
                   (twt & 4) ? " ACTIVE" : ((twt & 2) ? " PAUSED" : ""));
        }
    }

    if ((flags & (1u << 14)) && max_len >= 0xc8) {
        printf("\t tx total pkts: %u\n", rd_u32(s + 0x54));
        printf("\t tx total bytes: %llu\n", (unsigned long long)rd_u64(s + 0x60));
        printf("\t tx ucast pkts: %u\n", rd_u32(s + 0x34));
        printf("\t tx ucast bytes: %llu\n", (unsigned long long)rd_u64(s + 0x70));
        printf("\t tx mcast/bcast pkts: %u\n", rd_u32(s + 0x5c));
        printf("\t tx mcast/bcast bytes: %llu\n", (unsigned long long)rd_u64(s + 0x78));
        printf("\t tx failures: %u\n", rd_u32(s + 0x38));
        printf("\t rx data pkts: %u\n", rd_u32(s + 0x58));
        printf("\t rx data bytes: %llu\n", (unsigned long long)rd_u64(s + 0x68));
        printf("\t rx ucast pkts: %u\n", rd_u32(s + 0x3c));
        printf("\t rx ucast bytes: %llu\n", (unsigned long long)rd_u64(s + 0x80));
        printf("\t rx mcast/bcast pkts: %u\n", rd_u32(s + 0x40));
        printf("\t rx mcast/bcast bytes: %llu\n", (unsigned long long)rd_u64(s + 0x88));
        printf("\t rate of last tx pkt: %u kbps - %u kbps\n",
               rd_u32(s + 0x44), rd_u32(s + 0xc4));
        printf("\t rate of last rx pkt: %u kbps\n", rd_u32(s + 0x48));
        if (ver >= 8 && max_len >= 588) {
            printf("\t median tx rate: %u kbps\n", calc_median_rate(s + 460));
            printf("\t median rx rate: %u kbps\n", calc_median_rate(s + 524));
        }
        printf("\t rx decrypt succeeds: %u\n", rd_u32(s + 0x4c));
        printf("\t rx decrypt failures: %u\n", rd_u32(s + 0x50));
        printf("\t tx data pkts retried: %u\n", rd_u32(s + 0xa0));

        printf("\t per antenna rssi of last rx data frame:");
        for (int a = 0; a < 4; a++) printf(" %d", (int)(int8_t)s[168 + a]);
        putchar('\n');
        printf("\t per antenna average rssi of rx data frames:");
        for (int a = 0; a < 4; a++) printf(" %d", (int)(int8_t)s[144 + a]);
        putchar('\n');
        printf("\t per antenna noise floor:");
        for (int a = 0; a < 4; a++) printf(" %d", (int)(int8_t)s[148 + a]);
        putchar('\n');

        uint32_t tx_sent = rd_u32(s + 0xac);
        uint32_t tx_exh  = rd_u32(s + 0xa4);
        printf("\t tx total pkts sent: %u\n", tx_sent);
        printf("\t tx pkts retries: %u\n", rd_u32(s + 0xb0));
        printf("\t tx pkts retry exhausted: %u\n", tx_exh);
        printf("\t tx FW total pkts sent: %u\n", rd_u32(s + 0xb4));
        printf("\t tx FW pkts retries: %u\n", rd_u32(s + 0xb8));
        printf("\t tx FW pkts retry exhausted: %u\n", rd_u32(s + 0xbc));
        printf("\t rx total pkts retried: %u\n", rd_u32(s + 0xc0));
        if (ver >= 8) {
            printf("\t tx acked pkts: %u\n", tx_sent - tx_exh);
        }
    }

    if (ver >= 8 && len >= 325 && max_len >= 325) {
        printf("smoothed rssi: %d\n", (int)(int8_t)s[324]);
    }
}

static int cmd_sta_info(const wl_ctx_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        puts(" ERROR: missing arguments");
        return -EINVAL;
    }

    uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
    if (!buf) return -ENOMEM;

    int rc = 0;
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t mac[6];
    int is_all = (strcmp(argv[1], "all") == 0);
    if (!is_all) {
        if (bcm_wl_parse_mac(argv[1], mac) < 0) {
            puts(" ERROR: no valid ether addr provided");
            free(buf);
            return -EINVAL;
        }
        if (memcmp(mac, bcast, 6) == 0) is_all = 1;
    }

    if (is_all) {
        rc = wl_ctx_iovar_getbuf(ctx, "sta_info", bcast, 6, buf, WL_IOCTL_LARGE_LEN);
        if (rc == 0) {
            uint16_t count = rd_u16(buf + 4);
            uint16_t off   = rd_u16(buf + 8);
            if (count > 512) count = 512;
            size_t pos = off;
            for (uint16_t i = 0; i < count && pos + 4 <= WL_IOCTL_LARGE_LEN; i++) {
                uint16_t elen = rd_u16(buf + pos + 2);
                if (elen < 64 || elen > WL_IOCTL_LARGE_LEN - pos) break;
                print_one_sta_info(buf + pos, elen);
                pos += elen;
            }
        }
    } else {
        rc = wl_ctx_iovar_getbuf(ctx, "sta_info", mac, 6, buf, WL_IOCTL_LARGE_LEN);
        if (rc == 0) {
            uint16_t elen = rd_u16(buf + 2);
            if (elen == 0 || elen > WL_IOCTL_LARGE_LEN) elen = WL_IOCTL_LARGE_LEN;
            print_one_sta_info(buf, elen);
        }
    }

    free(buf);
    return rc;
}

/* =========================================================================
 * 16 & 18. status + scanresults (shared wl_bss_info_t v107..109 printer)
 * ========================================================================= */
static void print_bss_info(const uint8_t *bi, size_t max_len) {
    if (max_len < 120) return;
    uint32_t ver    = rd_u32(bi + 0x00);
    uint8_t  slen   = bi[0x12];
    char ssid_s[160];
    wl_format_ssid_escaped(bi + 0x13, slen, ssid_s, sizeof(ssid_s));
    printf("SSID: \"%s\"\n", ssid_s);

    uint16_t cap = rd_u16(bi + 0x10);
    const char *mode = ((cap & 3) == 1) ? "Managed" :
                       ((cap & 3) == 2) ? "Ad Hoc" : "<unknown>";
    printf("Mode: %s\t", mode);
    printf("RSSI: %d dBm\t", (int)rd_i16(bi + 0x4e));
    if (ver >= 109 && max_len >= 126) {
        printf("SNR: %d dB\t", (int)rd_i16(bi + 0x7c));
    }
    printf("noise: %d dBm\t", (int)(int8_t)bi[0x50]);

    uint16_t cs = rd_u16(bi + 0x48);
    char cs_s[32];
    bcm_wl_chspec_to_str(cs, cs_s, sizeof(cs_s));
    printf("Channel: %s\n", cs_s);

    char bssid_s[18];
    bcm_wl_format_mac(bi + 0x08, bssid_s, 0);
    printf("BSSID: %s\tCapability: ", bssid_s);
    if (cap & 0x0001u) printf("ESS ");
    if (cap & 0x0002u) printf("IBSS ");
    if (cap & 0x0004u) printf("Pollable ");
    if (cap & 0x0008u) printf("PollReq ");
    if (cap & 0x0010u) printf("WEP ");
    if (cap & 0x0020u) printf("ShortPre ");
    if (cap & 0x0040u) printf("PBCC ");
    if (cap & 0x0080u) printf("Agility ");
    if (cap & 0x0400u) printf("ShortSlot ");
    if (cap & 0x1000u) printf("RRM ");
    if (cap & 0x2000u) printf("CCK-OFDM ");
    putchar('\n');

    printf("Beacon Interval: %u msecs\t\n", (unsigned)rd_u16(bi + 0x0e));
    printf("Supported Rates: ");
    print_rateset(bi + 0x38, rd_u32(bi + 0x34));
    putchar('\n');

    if (ver >= 109 && max_len >= 0xa1) {
        uint8_t n_cap   = bi[0x51];
        uint8_t he_cap  = bi[0x52];
        uint8_t vht_cap = bi[0x61];
        if (he_cap)       puts("HE Capable:");
        else if (vht_cap) puts("VHT Capable:");
        else if (n_cap)   puts("HT Capable:");

        if (he_cap || vht_cap || n_cap) {
            printf("\tChanspec: %s channel %d %dMHz (0x%x)\n",
                   wl_chspec_band_str(cs), (int)(cs & 0xff),
                   wl_chspec_bw_mhz(cs), (unsigned)cs);
            printf("\tPrimary channel: %u\n", (unsigned)bi[0x58]);
            if (n_cap) {
                uint32_t nbss_cap = rd_u32(bi + 0x54);
                printf("\tHT Capabilities: %s%s%s\n",
                       (nbss_cap & (1u << 1)) ? "40MHz " : "",
                       (nbss_cap & (1u << 5)) ? "SGI20 " : "",
                       (nbss_cap & (1u << 6)) ? "SGI40 " : "");
                printf("\tSupported HT MCS :");
                int in_run = 0, run_start = 0;
                for (int m = 0; m < 128; m++) {
                    int set = (bi[0x64 + m / 8] >> (m & 7)) & 1;
                    if (set && !in_run) {
                        printf(" %d", m);
                        in_run = 1;
                        run_start = m;
                    } else if (!set && in_run) {
                        if (m - 1 > run_start) printf("-%d", m - 1);
                        in_run = 0;
                    }
                }
                if (in_run && 127 > run_start) printf("-127");
                putchar('\n');
            }
            if (vht_cap && max_len >= 0x84) {
                uint16_t rxmap   = rd_u16(bi + 0x5c);
                uint16_t txmap   = rd_u16(bi + 0x5e);
                uint16_t propmap = rd_u16(bi + 0x82);
                puts("\tSupported VHT MCS:");
                for (int nss = 1; nss <= 8; nss++) {
                    unsigned sh   = (unsigned)(nss - 1) * 2u;
                    unsigned tx   = (txmap >> sh) & 3u;
                    unsigned rx   = (rxmap >> sh) & 3u;
                    unsigned prop = (propmap >> sh) & 3u;
                    if (tx == 3 && rx == 3) continue;
                    const char *ts = (tx == 2) ? ((prop != 3) ? "0-11      " : "0-9 ") :
                                     (tx == 1) ? ((prop != 3) ? "0-8, 10-11" : "0-8 ") :
                                     (tx == 0) ? ((prop != 3) ? "0-7, 10-11" : "0-7 ") : "    --    ";
                    const char *rs = (rx == 2) ? ((prop != 3) ? "0-11      " : "0-9 ") :
                                     (rx == 1) ? ((prop != 3) ? "0-8, 10-11" : "0-8 ") :
                                     (rx == 0) ? ((prop != 3) ? "0-7, 10-11" : "0-7 ") : "    --    ";
                    printf("\t\tNSS%d Tx: %s  Rx: %s\n", nss, ts, rs);
                }
            }
            if (he_cap && max_len >= 0xa0) {
                const uint8_t *hm = NULL;
                if (rd_u16(bi + 0x94) == 0xffffu) {
                    puts("\tSupported HE MCS:");
                    hm = bi + 0x88;
                } else {
                    puts("\tNegotiated HE MCS:");
                    hm = bi + 0x94;
                }
                static const char *bw_lbl[3] = { "20/40/80 MHz", "160 MHz", "80+80 MHz" };
                for (int bw = 0; bw < 3; bw++) {
                    uint16_t txmap = rd_u16(hm + (size_t)bw * 4u + 0u);
                    uint16_t rxmap = rd_u16(hm + (size_t)bw * 4u + 2u);
                    int hdr = 0;
                    for (int nss = 1; nss <= 8; nss++) {
                        unsigned sh = (unsigned)(nss - 1) * 2u;
                        unsigned tx = (txmap >> sh) & 3u;
                        unsigned rx = (rxmap >> sh) & 3u;
                        if (tx == 3 && rx == 3) continue;
                        if (!hdr) {
                            printf("\t    %s:\n", bw_lbl[bw]);
                            hdr = 1;
                        }
                        const char *ts = (tx == 2) ? "0-11      " :
                                         (tx == 1) ? "0-9       " :
                                         (tx == 0) ? "0-7       " : "---       ";
                        const char *rs = (rx == 2) ? "0-11" :
                                         (rx == 1) ? "0-9"  :
                                         (rx == 0) ? "0-7"  : "---";
                        printf("\t\tNSS%d Tx: %s  Rx: %s\n", nss, ts, rs);
                    }
                }
            }
            if (he_cap && max_len >= 0x163) {
                uint8_t bcol = bi[0x162];
                printf("\tBSS Color: 0x%x\t\tPartial BSS Color: %s\n",
                       (unsigned)(bcol & 0x3fu), (bcol & 0x40u) ? "true" : "false");
                printf("\tBSS Color Disabled: %s\n", (bcol & 0x80u) ? "true" : "false");
            }
        }
        uint8_t qbss = bi[0xa0];
        printf("QBSS Channel Utilization: 0x%x (%u %%)\n",
               (unsigned)qbss, (unsigned)((qbss * 100u) / 255u));
    }
    putchar('\n');
}

static int cmd_status(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    uint8_t bssid[6] = {0};
    int rc = wl_ctx_ioctl(ctx, WLC_GET_BSSID, bssid, sizeof(bssid), 0);
    if (rc < 0) {
        if (rc == -ENODEV || rc == -EPERM || rc == -EACCES || rc == -ENAMETOOLONG) {
            return rc;
        }
        /* Matches stock 0x3d320: Not associated (propagate WLC_GET_SSID errors) */
        uint8_t ssid_buf[36] = {0};
        int src = wl_ctx_ioctl(ctx, WLC_GET_SSID, ssid_buf, sizeof(ssid_buf), 0);
        if (src < 0) return src;
        char ss[160];
        wl_format_ssid_escaped(ssid_buf + 4, rd_u32(ssid_buf), ss, sizeof(ss));
        printf("Not associated. Last associated with SSID: \"%s\"\n", ss);
        return 0;
    }

    uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
    if (!buf) return -ENOMEM;
    uint32_t buflen = WL_IOCTL_LARGE_LEN;
    memcpy(buf, &buflen, 4);

    rc = wl_ctx_ioctl(ctx, 136 /* WLC_GET_BSS_INFO */, buf, WL_IOCTL_LARGE_LEN, 0);
    if (rc == 0) {
        uint32_t ver = rd_u32(buf + 4);
        if (ver < 107 || ver > 109) {
            fprintf(stderr, "Unsupported bss_info_version %u\n", ver);
            free(buf);
            return -EINVAL;
        }
        uint32_t bi_len = rd_u32(buf + 8);
        size_t max_bi = WL_IOCTL_LARGE_LEN - 4u;
        if (bi_len >= 120u && bi_len < max_bi) max_bi = bi_len;
        print_bss_info(buf + 4, max_bi);
    }
    free(buf);
    return rc;
}

static int cmd_scan(const wl_ctx_t *ctx, int argc, char **argv) {
    /* Exact wl_scan_params_t / wl_scan_params_v2_t (verified stock 0x25cf4 + 0x4434c..0x446d0):
     * +0x00 u32 ssid_len
     * +0x04 u8  ssid[32]
     * +0x24 u8  bssid[6] = ff:ff:ff:ff:ff:ff
     * +0x2a i8  bss_type = 2 (DOT11_BSSTYPE_ANY)
     * +0x2b u8  scan_type = 0 (active) / 1 (passive)
     * +0x2c i32 nprobes = -1
     * +0x30 u32 reserved/scan_flags = 0 (4 bytes!)
     * +0x34 i32 active_time = -1
     * +0x38 i32 passive_time = -1
     * +0x3c i32 home_time = -1
     * +0x40 u32 channel_num (low16 = nch, high16 = nssid)
     * v1 (scan_ver != 2): +0x44 u16 channel_list[]  (fixed header = 68B)
     * v2 (scan_ver == 2): +0x44..0x53 (16B v2 ext), +0x54 u16 channel_list[] (fixed header = 84B)
     */
    uint8_t sv[16];
    memset(sv, 0, sizeof(sv));
    uint16_t scan_ver = 0;
    if (wl_ctx_iovar_get(ctx, "scan_ver", sv, sizeof(sv)) == 0) {
        scan_ver = rd_u16(sv + 4);
    }

    uint8_t params[256];
    memset(params, 0, sizeof(params));
    memset(params + 0x24, 0xff, 6);
    params[0x2a] = 2; /* DOT11_BSSTYPE_ANY */
    int32_t neg1 = -1;
    memcpy(params + 0x2c, &neg1, 4); /* nprobes */
    /* +0x30 stays 0 */
    memcpy(params + 0x34, &neg1, 4); /* active_time */
    memcpy(params + 0x38, &neg1, 4); /* passive_time */
    memcpy(params + 0x3c, &neg1, 4); /* home_time */

    uint16_t chans[64];
    int nch = 0;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--ssid") == 0) && i + 1 < argc) {
            const char *ss = argv[++i];
            size_t sl = strlen(ss);
            if (sl > 32) return -EINVAL;
            uint32_t sl32 = (uint32_t)sl;
            memcpy(params + 0x00, &sl32, 4);
            memcpy(params + 0x04, ss, sl);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            if (strcasecmp(t, "passive") == 0)      params[0x2b] = 1;
            else if (strcasecmp(t, "active") == 0)  params[0x2b] = 0;
            else return -EINVAL;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            uint16_t cs = 0;
            if (bcm_wl_chspec_from_str(argv[++i], &cs) < 0) return -EINVAL;
            if (nch < 64) chans[nch++] = cs;
        } else {
            return -EINVAL;
        }
    }

    size_t hdr_sz = (scan_ver == 2) ? 84u : 68u;
    if (nch > 0) {
        uint32_t cnum = (uint32_t)nch;
        memcpy(params + 0x40, &cnum, 4);
        memcpy(params + hdr_sz, chans, (size_t)nch * 2u);
    }
    size_t plen = hdr_sz + (size_t)nch * 2u;

    return wl_ctx_ioctl(ctx, WLC_SCAN, params, plen, 1);
}

static int cmd_scanresults(const wl_ctx_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    uint8_t *buf = (uint8_t *)calloc(1, WL_DUMP_BUF_LEN);
    if (!buf) return -ENOMEM;

    uint32_t buflen = WL_DUMP_BUF_LEN;
    memcpy(buf, &buflen, 4);

    int rc = wl_ctx_ioctl(ctx, WLC_SCAN_RESULTS, buf, WL_DUMP_BUF_LEN, 0);
    if (rc == 0) {
        /* wl_scan_results_t: { u32 buflen; u32 version; u32 count; wl_bss_info_t bss_info[]; } */
        uint32_t ver   = rd_u32(buf + 4);
        uint32_t count = rd_u32(buf + 8);
        if (ver >= 107 && ver <= 109 && count > 0) {
            size_t off = 12;
            for (uint32_t i = 0; i < count && off + 12 <= WL_DUMP_BUF_LEN; i++) {
                uint32_t b_len = rd_u32(buf + off + 4);
                if (b_len < 64 || b_len > WL_DUMP_BUF_LEN - off) break;
                print_bss_info(buf + off, b_len);
                off += b_len;
            }
        }
    }
    free(buf);
    return rc;
}

/* =========================================================================
 * 17. counters (IOVAR "counters", XTLV v30, exact offsets from 0x5e3e0..0x67340)
 * ========================================================================= */
typedef struct {
    uint16_t    off;
    const char *name;
} cnt_field_t;

static const cnt_field_t CNT_WLC_100[] = {
    { 0x000, "txframe" },       { 0x004, "txbyte" },        { 0x008, "txretrans" },
    { 0x00c, "txerror" },       { 0x014, "txprshort" },     { 0x018, "txserr" },
    { 0x01c, "txnobuf" },       { 0x020, "txnoassoc" },     { 0x028, "txchit" },
    { 0x02c, "txcmiss" },       { 0x034, "txphyerr" },      { 0x038, "txphycrs" },
    { 0x03c, "rxframe" },       { 0x040, "rxbyte" },        { 0x044, "rxerror" },
    { 0x048, "rxctl" },         { 0x04c, "rxnobuf" },       { 0x050, "rxnondata" },
    { 0x054, "rxbadds" },       { 0x058, "rxbadcm" },       { 0x05c, "rxfragerr" },
    { 0x060, "rxrunt" },        { 0x064, "rxgiant" },       { 0x068, "rxnoscb" },
    { 0x06c, "rxbadproto" },    { 0x070, "rxbadsrcmac" },   { 0x074, "rxbadda" },
    { 0x078, "rxfilter" },      { 0x0b0, "reset" },         { 0x0b4, "tbtt" },
    { 0x0b8, "txdmawar" },      { 0x0c0, "d11_txfrag" },    { 0x0c4, "d11_txmulti" },
    { 0x0c8, "txfail" },        { 0x0cc, "d11_txretry" },   { 0x0d0, "d11_txretrie" },
    { 0x0d4, "rxdup" },         { 0x0d8, "d11_txrts" },     { 0x0dc, "d11_txnocts" },
    { 0x0e0, "d11_txnoack" },   { 0x0e4, "d11_rxfrag" },    { 0x0e8, "d11_rxmulti" },
    { 0x0ec, "rxcrc" },         { 0x0f0, "d11_txfrmsnt" },  { 0x0f4, "rxundec" },
    { 0x0f8, "tkipmicfaill" },  { 0x0fc, "tkipcntrmsr" },   { 0x100, "tkipreplay" },
    { 0x104, "ccmpfmterr" },    { 0x108, "ccmpreplay" },    { 0x10c, "ccmpundec" },
    { 0x110, "fourwayfail" },   { 0x114, "wepundec" },      { 0x118, "wepicverr" },
    { 0x11c, "decsuccess" },    { 0x120, "tkipicverr" },    { 0x128, "txchanrej" },
    { 0x150, "rx1mbps" },       { 0x154, "rx2mbps" },       { 0x158, "rx5mbps5" },
    { 0x15c, "rx6mbps" },       { 0x160, "rx9mbps" },       { 0x164, "rx11mbps" },
    { 0x168, "rx12mbps" },      { 0x16c, "rx18mbps" },      { 0x170, "rx24mbps" },
    { 0x174, "rx36mbps" },      { 0x178, "rx48mbps" },      { 0x17c, "rx54mbps" },
    { 0x1a8, "txexptime" },     { 0x1ac, "txmpdu_sgi" },    { 0x1b0, "rxmpdu_sgi" },
    { 0x1b4, "txmpdu_stbc" },   { 0x1b8, "rxmpdu_stbc" },   { 0x1f0, "dma_hang" },
    { 0x1f4, "reinit" },        { 0x20c, "cso_passthrough" }, { 0x210, "cso_normal" },
    { 0x214, "chained" },       { 0x218, "chainedsz1" },    { 0x21c, "unchained" },
    { 0x220, "maxchainsz" },    { 0x224, "currchainsz" },   { 0x228, "pciereset" },
    { 0x22c, "cfgrestore" },    { 0x250, "rxrtry" },        { 0x254, "rxmpdu_mu" },
    { 0x258, "txbar" },         { 0x25c, "rxbar" },         { 0x260, "txpspoll" },
    { 0x264, "rxpspoll" },      { 0x268, "txnull" },        { 0x26c, "rxnull" },
    { 0x270, "txqosnull" },     { 0x274, "rxqosnull" },     { 0x278, "txassocreq" },
    { 0x27c, "rxassocreq" },    { 0x280, "txreassocreq" },  { 0x284, "rxreassocreq" },
    { 0x288, "txdisassoc" },    { 0x28c, "rxdisassoc" },    { 0x290, "txassocrsp" },
    { 0x294, "rxassocrsp" },    { 0x298, "txreassocrsp" },  { 0x29c, "rxreassocrsp" },
    { 0x2a0, "txauth" },        { 0x2a4, "rxauth" },        { 0x2a8, "txdeauth" },
    { 0x2ac, "rxdeauth" },      { 0x2b0, "txprobereq" },    { 0x2b4, "rxprobereq" },
    { 0x2b8, "txprobersp" },    { 0x2bc, "rxprobersp" },    { 0x2c0, "txaction" },
    { 0x2c4, "rxaction" },      { 0x2c8, "ampdu_wds" },     { 0x2cc, "txlost" },
    { 0x2d0, "txdatamcast" },   { 0x2d4, "txdatabcast" },   { 0x2f4, "d11_txbcast" },
    { 0x2f8, "txdropped" },     { 0x2fc, "d11_rxbcast" },   { 0x300, "rxdropped" },
};

static const cnt_field_t CNT_MCST_200[] = {
    { 0x000, "txallfrm" },      { 0x004, "txrtsfrm" },      { 0x008, "txctsfrm" },
    { 0x00c, "txackfrm" },      { 0x010, "txdnlfrm" },      { 0x014, "txbcnfrm" },
    { 0x030, "txampdu" },       { 0x034, "txmpdu" },        { 0x03c, "txphyerror" },
    { 0x040, "pktengrxducast" }, { 0x044, "pktengrxdmcast" }, { 0x048, "rxfrmtoolong" },
    { 0x04c, "rxfrmtooshrt" },  { 0x050, "rxanyerr" },      { 0x054, "rxbadfcs" },
    { 0x058, "rxbadplcp" },     { 0x05c, "rxcrsglitch" },   { 0x060, "rxstrt" },
    { 0x064, "rxdfrmucastmbss" }, { 0x068, "rxmfrmucastmbss" }, { 0x06c, "rxcfrmucast" },
    { 0x070, "rxrtsucast" },    { 0x074, "rxctsucast" },    { 0x078, "rxackucast" },
    { 0x07c, "rxdfrmocast" },   { 0x080, "rxmfrmocast" },   { 0x084, "rxcfrmocast" },
    { 0x088, "rxrtsocast" },    { 0x08c, "rxctsocast" },    { 0x090, "rxdfrmmcast" },
    { 0x094, "rxmfrmmcast" },   { 0x098, "rxcfrmmcast" },   { 0x09c, "rxbeaconmbss" },
    { 0x0a0, "rxdfrmucastobss" }, { 0x0a4, "rxbeaconobss" }, { 0x0a8, "rxrsptmout" },
    { 0x0ac, "bcntxcancl" },    { 0x0b0, "rxnodelim" },     { 0x0b4, "rxf0ovfl" },
    { 0x0c4, "pmqovfl" },       { 0x0dc, "txrtsfail" },     { 0x0e0, "txucast" },
    { 0x0e4, "txinrtstxop" },   { 0x0e8, "rxback" },        { 0x0ec, "txback" },
    { 0x0f0, "bphy_rxcrsglitch" }, { 0x0f8, "rxtoolate" },  { 0x0fc, "bphy_badplcp" },
};

static void print_cnt_fields(const uint8_t *body, uint16_t len,
                             const cnt_field_t *tbl, size_t n, int nz_only) {
    int col = 0;
    for (size_t i = 0; i < n; i++) {
        if ((uint32_t)tbl[i].off + 4u > len) continue;
        uint32_t v = rd_u32(body + tbl[i].off);
        if (nz_only && v == 0) continue;
        printf("%s %u ", tbl[i].name, v);
        if (++col % 6 == 0) putchar('\n');
    }
    if (col % 6 != 0) putchar('\n');
}

static int cmd_counters(const wl_ctx_t *ctx, int argc, char **argv) {
    int nz_only = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nz") == 0) nz_only = 1;
    }

    uint8_t *buf = (uint8_t *)calloc(1, WL_IOCTL_LARGE_LEN);
    if (!buf) return -ENOMEM;

    int rc = wl_ctx_iovar_get(ctx, "counters", buf, WL_IOCTL_LARGE_LEN);
    if (rc < 0) {
        free(buf);
        return rc;
    }

    uint16_t ver     = rd_u16(buf + 0);
    uint16_t datalen = rd_u16(buf + 2);
    if (datalen > WL_IOCTL_LARGE_LEN - 4) datalen = WL_IOCTL_LARGE_LEN - 4;
    printf("counters_version %2u\n", (unsigned)ver);
    printf("datalen %u\n", (unsigned)datalen);

    if (ver >= 30) {
        /* Walk 4-byte-aligned XTLV entries inside buf[4..4+datalen] */
        size_t off = 4;
        size_t end = 4u + datalen;
        while (off + 4 <= end) {
            uint16_t tid  = rd_u16(buf + off);
            uint16_t tlen = rd_u16(buf + off + 2);
            if (off + 4u + tlen > end) break;
            const uint8_t *body = buf + off + 4;
            if (tid == 0x0100) {
                print_cnt_fields(body, tlen, CNT_WLC_100,
                                 sizeof(CNT_WLC_100) / sizeof(CNT_WLC_100[0]), nz_only);
            } else if (tid == 0x0200 || tid == 0x0400) {
                print_cnt_fields(body, tlen, CNT_MCST_200,
                                 sizeof(CNT_MCST_200) / sizeof(CNT_MCST_200[0]), nz_only);
            } else if (tid == 0x0102 && tlen >= 4) {
                uint32_t v = rd_u32(body);
                if (!nz_only || v) printf("txbcnloss %u \n", v);
            } else if (tid == 0x0800 && tlen >= 24) {
                static const cnt_field_t f800[] = {
                    { 0x00, "macxsusp" }, { 0x04, "m2vmsg" }, { 0x08, "v2mmsg" },
                    { 0x0c, "mboxout" },  { 0x10, "musnd" },  { 0x14, "sfb2v" },
                };
                print_cnt_fields(body, tlen, f800, sizeof(f800) / sizeof(f800[0]), nz_only);
            }
            size_t step = (4u + tlen + 3u) & ~3u;
            if (step == 0) break;
            off += step;
        }
    } else {
        /* Legacy flat wl_cnt_t fallback */
        print_cnt_fields(buf + 4, datalen, CNT_WLC_100,
                         sizeof(CNT_WLC_100) / sizeof(CNT_WLC_100[0]), nz_only);
    }

    free(buf);
    return 0;
}

/* =========================================================================
 * Exported T1 Command Table
 * ========================================================================= */
const wl_cmd_entry_t WL_T1_CMDS[] = {
    { "assoclist",     cmd_assoclist,     "T1", "client",   "Get list of associated client MACs" },
    { "bw_cap",        cmd_bw_cap,        "T1", "radio",    "Get/Set per-band bandwidth cap (2g|5g [mask])" },
    { "chanim_stats",  cmd_chanim_stats,  "T1", "rf",       "Get channel interference & busy stats" },
    { "chanspec",      cmd_chanspec,      "T1", "radio",    "Get/Set channel spec (e.g. 6, 48/160)" },
    { "counters",      cmd_counters,      "T1", "diag",     "Get driver & ucode statistics counters" },
    { "country",       cmd_country,       "T1", "reg",      "Get/Set country code (e.g. CN/947) or list" },
    { "csimon",        cmd_csimon,        "T1", "csi",      "CSI monitor control (add/del/enable/disable/state)" },
    { "cur_etheraddr", cmd_cur_etheraddr, "T1", "iface",    "Get/Set current interface MAC address" },
    { "he",            cmd_he,            "T1", "wifi6",    "Get/Set 802.11ax HE mode and subcommands" },
    { "rssi",          cmd_rssi,          "T1", "rf",       "Get RSSI in dBm [client MAC]" },
    { "scan",          cmd_scan,          "T1", "scan",     "Initiate wireless scan [-s ssid] [-c ch] [-t passive]" },
    { "scanresults",   cmd_scanresults,   "T1", "scan",     "Display results of last wireless scan" },
    { "spect",         cmd_spect,         "T1", "reg",      "Get/Set 802.11h spectrum management mode (0..4)" },
    { "sta_info",      cmd_sta_info,      "T1", "client",   "Get detailed per-STA diagnostics (<mac>|all)" },
    { "sta_monitor",   cmd_sta_monitor,   "T1", "client",   "Off-channel/unassoc STA RSSI monitor (add/del/dump/...)" },
    { "status",        cmd_status,        "T1", "radio",    "Get current BSS status, channel, noise, caps" },
    { "twt",           cmd_twt,           "T1", "wifi6",    "Get/Set 802.11ax TWT mode and schedules" },
    { "txpwr1",        cmd_txpwr1,        "T1", "power",    "Get/Set transmit power ([-o] [-d|-q|-m] val)" },
    { "wme_ac",        cmd_wme_ac,        "T1", "qos",      "Get/Set WMM EDCA AC parameters (ap|sta [be|bk|vi|vo ...])" },
};
const size_t WL_T1_CMDS_COUNT = sizeof(WL_T1_CMDS) / sizeof(WL_T1_CMDS[0]);
