/*
 * wl_internal.h — Internal shared definitions for bcmopen wl implementation
 * (Driver 17.10.251.3202 on BCM68252C / BCM6756, ARMv7 Linux 4.19.235).
 */
#ifndef BCMOPEN_WL_INTERNAL_H
#define BCMOPEN_WL_INTERNAL_H

#include "bcmopen.h"

#include <stddef.h>
#include <stdint.h>

/* Linux SIOCDEVPRIVATE */
#ifndef SIOCDEVPRIVATE
#define SIOCDEVPRIVATE 0x89f0
#endif

/* Broadcom wl ioctl buffer sizes */
#define WL_IOCTL_MEDLEN     1536
#define WL_IOCTL_MAXLEN     8192
#define WL_IOCTL_LARGE_LEN  16384
#define WL_DUMP_BUF_LEN     261120  /* 255 * 1024, matches stock wl_dump */

/* Core Broadcom WLC ioctl numbers */
#define WLC_GET_MAGIC               0
#define WLC_GET_VERSION             1
#define WLC_UP                      2
#define WLC_DOWN                    3
#define WLC_GET_BSSID               23
#define WLC_SET_BSSID               24
#define WLC_GET_SSID                25
#define WLC_SET_SSID                26
#define WLC_RESTART                 27
#define WLC_GET_CHANNEL             29
#define WLC_SET_CHANNEL             30
#define WLC_GET_RADIO               37
#define WLC_SET_RADIO               38
#define WLC_GET_PHYTYPE             39
#define WLC_SCAN                    50
#define WLC_SCAN_RESULTS            51
#define WLC_DISASSOC                52
#define WLC_GET_MACLIST             69
#define WLC_SET_MACLIST             70
#define WLC_GET_COUNTRY             83
#define WLC_SET_COUNTRY             84
#define WLC_GET_REVINFO             98
#define WLC_GET_MACMODE             105
#define WLC_SET_MACMODE             106
#define WLC_GET_MONITOR             107
#define WLC_SET_MONITOR             108
#define WLC_GET_AP                  117
#define WLC_SET_AP                  118
#define WLC_SCB_AUTHORIZE           121
#define WLC_SCB_DEAUTHORIZE         122
#define WLC_GET_RSSI                127
#define WLC_GET_PHY_NOISE           135
#define WLC_GET_BANDLIST            140
#define WLC_GET_BAND                141
#define WLC_SET_BAND                142
#define WLC_SCB_DEAUTHENTICATE      143
#define WLC_GET_ASSOCLIST           159
#define WLC_GET_UP                  162
#define WLC_SCB_DEAUTHENTICATE_FOR_REASON 201
#define WLC_GET_VALID_CHANNELS      217
#define WLC_GET_FRAMEBURST          218
#define WLC_SET_FRAMEBURST          219
#define WLC_GET_PWRPERCENT          220
#define WLC_SET_PWRPERCENT          221
#define WLC_SET_SPECT_MANAGMENT     244
#define WLC_GET_SPECT_MANAGMENT     245
#define WLC_GET_VAR                 262
#define WLC_SET_VAR                 263

/* D11REV >= 40 chanspec bit definitions */
#define WL_CHANSPEC_CHAN_MASK       0x00ffu
#define WL_CHANSPEC_CTL_SB_SHIFT    8u
#define WL_CHANSPEC_CTL_SB_MASK     0x0700u
#define WL_CHANSPEC_BW_SHIFT        11u
#define WL_CHANSPEC_BW_MASK         0x3800u
#define WL_CHANSPEC_BW_5            0x0000u
#define WL_CHANSPEC_BW_10           0x0800u
#define WL_CHANSPEC_BW_20           0x1000u
#define WL_CHANSPEC_BW_40           0x1800u
#define WL_CHANSPEC_BW_80           0x2000u
#define WL_CHANSPEC_BW_160          0x2800u
#define WL_CHANSPEC_BW_8080         0x3000u
#define WL_CHANSPEC_BAND_SHIFT      14u
#define WL_CHANSPEC_BAND_MASK       0xc000u
#define WL_CHANSPEC_BAND_2G         0x0000u
#define WL_CHANSPEC_BAND_6G         0x4000u
#define WL_CHANSPEC_BAND_4G         0x8000u
#define WL_CHANSPEC_BAND_5G         0xc000u

/* Broadcom ioctl header passed via ifr.ifr_data on SIOCDEVPRIVATE (0x89f0) */
typedef struct {
    uint32_t cmd;
    void    *buf;
    uint32_t len;
    uint8_t  set;
    uint8_t  pad[3];
    uint32_t used;
    uint32_t needed;
} wl_ioctl_t;

/* Per-invocation CLI context */
typedef struct {
    char ifname[32];
    int  bsscfg_idx;   /* -1 = not specified (default primary BSS) */
    const char *prog;  /* argv[0] for error reporting */
} wl_ctx_t;

/* Custom command table entry */
typedef int (*wl_cmd_fn_t)(const wl_ctx_t *ctx, int argc, char **argv);

typedef struct {
    const char  *name;
    wl_cmd_fn_t  fn;
    const char  *tier;
    const char  *group;
    const char  *help;
} wl_cmd_entry_t;

/* Context wrappers (honor ctx->bsscfg_idx when >= 0) */
int wl_ctx_ioctl(const wl_ctx_t *ctx, int cmd, void *buf, size_t len, int set);
int wl_ctx_iovar_get(const wl_ctx_t *ctx, const char *name, void *out, size_t out_len);
int wl_ctx_iovar_set(const wl_ctx_t *ctx, const char *name, const void *val, size_t val_len);
int wl_ctx_iovar_getbuf(const wl_ctx_t *ctx, const char *name,
                        const void *param, size_t param_len,
                        void *out, size_t out_len);
int wl_ctx_iovar_setbuf(const wl_ctx_t *ctx, const char *name,
                        const void *param, size_t param_len,
                        const void *val, size_t val_len);
int wl_ctx_iovar_getint(const wl_ctx_t *ctx, const char *name, int32_t *out);
int wl_ctx_iovar_setint(const wl_ctx_t *ctx, const char *name, int32_t val);

/* Shared parsing & formatting helpers */
int  wl_parse_i32(const char *s, int32_t *out);
int  wl_parse_u32(const char *s, uint32_t *out);
int  wl_parse_onoff_i32(const char *s, int32_t *out);
void wl_print_varint(int32_t val);
void wl_print_err(const wl_ctx_t *ctx, int rc);
int  wl_chspec_primary_chan(uint16_t chspec);
int  wl_chspec_bw_mhz(uint16_t chspec);
const char *wl_chspec_band_str(uint16_t chspec);
void wl_format_ssid_escaped(const uint8_t *ssid, uint32_t len, char *out, size_t out_sz);

/* Custom command tables defined in wl_t1.c and wl_t2.c */
extern const wl_cmd_entry_t WL_T1_CMDS[];
extern const size_t         WL_T1_CMDS_COUNT;

extern const wl_cmd_entry_t WL_T2_CMDS[];
extern const size_t         WL_T2_CMDS_COUNT;

#endif /* BCMOPEN_WL_INTERNAL_H */
