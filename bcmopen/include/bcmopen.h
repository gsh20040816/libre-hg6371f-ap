/*
 * bcmopen.h — Open-source C99 library ABI replacing Broadcom /opt/bcm tools
 * on HG6371F (BCM68252C, ARMv7 Linux 4.19.235, musl).
 *
 * Shared by:
 *   1. Multi-call static CLI `bcmctl` (wdtctl/fcctl/nvram/bcm_bootstate/
 *      bcm_flashutil/ethswctl/bdmf_shell/bs/wl/wlctl)
 *   2. Go management panel `ap-panel-srv` via CGO static link (zero fork)
 */
#ifndef BCMOPEN_H
#define BCMOPEN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. Hardware Watchdog (/dev/watchdog)
 * ========================================================================= */
#define BCM_WDT_MIN_TIMEOUT 4
#define BCM_WDT_MAX_TIMEOUT 600
#define BCM_WDT_DEF_TIMEOUT 80
#define BCM_WDT_DEF_PRETIMEOUT 3

int bcm_wdt_open(int *out_fd);
int bcm_wdt_start(int fd, int *timeout_sec, int pretimeout_sec);
int bcm_wdt_ping(int fd);
int bcm_wdt_disable(int fd);
int bcm_wdt_timeleft(int *out_sec);
int bcm_cli_wdtctl(int argc, char **argv);

/* =========================================================================
 * 2. Broadcom Packet Flow Cache (/dev/fcache)
 * ========================================================================= */
typedef struct {
    uint32_t flow_refresh_timer_ms;    /* +0x00 */
    int16_t  hw_activate_defer_rate;   /* +0x04 */
    int16_t  low_pkt_rate;             /* +0x06 */
    uint32_t ucast_max;                /* +0x08 */
    uint32_t ucast_created;            /* +0x0c */
    uint32_t ucast_evicted;            /* +0x10 */
    uint32_t sw_activate_defer_count;  /* +0x14 */
    uint32_t mcast_max;                /* +0x18 */
    uint32_t mcast_created;            /* +0x1c */
    uint32_t mcast_evicted;            /* +0x20 */
    uint32_t uucast_max;               /* +0x24 */
    uint32_t uucast_created;           /* +0x28 */
    uint32_t uucast_evicted;           /* +0x2c */
    uint16_t tcp_idle_s;               /* +0x30 */
    uint16_t udp_idle_s;               /* +0x32 */
    uint16_t l2_idle_s;                /* +0x34 */
    uint16_t mcast_idle_s;             /* +0x36 */
    uint16_t other_idle_s;             /* +0x38 */
    uint16_t uucast_idle_s;            /* +0x3a */
    uint8_t  flags0;                   /* +0x3c */
    uint8_t  flags1;                   /* +0x3d */
    uint8_t  pad[2];                   /* +0x3e..0x3f */
} bcm_fc_status_t;

/* flags0 bits (+0x3c) */
#define BCM_FC_F0_HW_IDLE_DEACT  0x01
#define BCM_FC_F0_MCAST_V4       0x02
#define BCM_FC_F0_MCAST_V6       0x04
#define BCM_FC_F0_IPV6_LEARN     0x08
#define BCM_FC_F0_FLOW_LEARN     0x10
#define BCM_FC_F0_OVS_LEARN      0x20
#define BCM_FC_F0_GRE_LEARN      0x40
#define BCM_FC_F0_UUCAST_LEARN   0x80

/* flags1 bits (+0x3d) */
#define BCM_FC_F1_ACCEL_L2L3     0x02
#define BCM_FC_F1_TCP_ACK_PRIO   0x04
#define BCM_FC_F1_HW_ACCEL       0x08
#define BCM_FC_F1_NOTIFY_HYBRID  0x10
#define BCM_FC_F1_4O6_FRAG       0x20
#define BCM_FC_F1_TOS_MFLOWS     0x40
#define BCM_FC_F1_L2TP_LEARN     0x80

typedef struct {
    uint32_t flags;
    uint8_t  mac[6];
    uint16_t pad;
    uint32_t ifindex;
    uint32_t flowid;
} bcm_fc_flush_req_t;

typedef struct {
    uint8_t  txtos;
    uint8_t  rxtos;
    uint8_t  intxsrcmac[6];
    uint8_t  intxdstmac[6];
    uint8_t  inrxsrcmac[6];
    uint8_t  inrxdstmac[6];
    uint16_t pad0;
    uint32_t rxdstv4;
    uint32_t srcipver;
    uint32_t dstipver;
    uint32_t srcv4;
    uint32_t dstv4;
    uint8_t  srcv6[16];
    uint8_t  dstv6[16];
    uint16_t srcport;
    uint16_t dstport;
    uint8_t  proto;
    uint8_t  pad1;
    uint16_t invid;
    uint16_t outvid;
    uint8_t  srcmac[6];
    uint8_t  dstmac[6];
    char     srcphy[16];
    char     dstphy[16];
    uint16_t pad2;
    uint32_t flags;
    uint32_t query_id;
} bcm_fc_tuple_req_t;

int bcm_fc_status(bcm_fc_status_t *out);
int bcm_fc_enable(void);
int bcm_fc_disable(void);
int bcm_fc_reset_stats(void);
int bcm_fc_flush(const bcm_fc_flush_req_t *req);
int bcm_fc_config(int ioctl_cmd, int val);
int bcm_fc_dump(uint32_t type, uint32_t a, uint32_t b, uint32_t c);
int bcm_fc_mcast_stats(uint64_t out[4]);
int bcm_cli_fcctl(int argc, char **argv);

/* =========================================================================
 * 3. Kernel & User NVRAM (AF_NETLINK proto=31 + file storage)
 * ========================================================================= */
int bcm_knvram_set(const char *key, const char *val);
int bcm_knvram_unset(const char *key);
int bcm_knvram_get(const char *key, char *out, size_t out_sz);
int bcm_knvram_getall(char *out, size_t out_sz);
int bcm_knvram_loadfile(const char *path);
int bcm_unvram_get(const char *key, char *out, size_t out_sz);
int bcm_unvram_set(const char *key, const char *val);
int bcm_unvram_unset(const char *key);
int bcm_cli_nvram(int argc, char **argv);

/* =========================================================================
 * 4. A/B Bootstate & Flash Ident (pureUBI metadata1/2 + UBI_IOCVOLUP)
 * ========================================================================= */
#define BCM_META_VOL_BYTES   0x500
#define BCM_META_SIZE_FIELD  0x100
#define BCM_META_HDR_BYTES   0x0c
#define BCM_META_CRC_LEN     (BCM_META_SIZE_FIELD - 4) /* 252 */

typedef struct {
    int committed; /* 0, 1, or 2 */
    int valid[2];  /* {0|1, 0|2} */
    int seq[2];    /* 0..999, or -1 if absent */
} bcm_boot_meta_t;

typedef struct {
    int  booted_part;      /* 1 or 2 */
    int  reboot_part;      /* 1 or 2 */
    int  state_char;       /* '5','6','7','8' */
    int  once_pending;     /* 0 or 1 */
    bcm_boot_meta_t meta;
    char img_tag[2][256];
} bcm_bootstate_t;

uint32_t bcm_crc32(const void *data, size_t len);
int bcm_meta_parse(const uint8_t *buf, size_t len, bcm_boot_meta_t *out);
int bcm_meta_pack(const bcm_boot_meta_t *m, uint8_t *buf, size_t len);
int bcm_bootstate_read(bcm_bootstate_t *out);
int bcm_bootstate_set_code(int code);          /* 1..8 */
int bcm_bootstate_commit(int part, int flag);  /* part=1|2, flag=0|1 */
int bcm_flash_get_ident(int partition, int *start_blk, int *end_blk,
                        const char *key, char *out, int out_sz);
int bcm_cli_bootstate(int argc, char **argv);
int bcm_cli_flashutil(int argc, char **argv);

/* =========================================================================
 * 5. Ethernet Switch Control (SIOCETHSWCTLOPS = 0x89fd)
 * ========================================================================= */
int bcm_cli_ethswctl(int argc, char **argv);

/* =========================================================================
 * 6. BDMF Shell (/dev/bdmf_shell + TCP/UNIX socket)
 * ========================================================================= */
int bcm_bdmf_init_session(int *out_session_id);
int bcm_bdmf_send_cmd(int session_id, const char *cmd);
int bcm_bdmf_close_session(int session_id);
int bcm_cli_bdmf_shell(int argc, char **argv);
int bcm_cli_bs(int argc, char **argv);

/* =========================================================================
 * 7. Wi-Fi Driver Control (wl / wlctl via SIOCDEVPRIVATE = 0x89f0)
 * ========================================================================= */
#define BCM_WL_MAGIC 0x14e46c77u

int bcm_wl_find_iface(char *out_ifname, size_t out_sz);
int bcm_wl_ioctl(const char *ifname, int cmd, void *buf, size_t len, int set);
int bcm_wl_iovar_get(const char *ifname, const char *name, void *out, size_t out_len);
int bcm_wl_iovar_set(const char *ifname, const char *name, const void *val, size_t val_len);
int bcm_wl_iovar_getbuf(const char *ifname, const char *name,
                        const void *param, size_t param_len,
                        void *out, size_t out_len);
int bcm_wl_iovar_setbuf(const char *ifname, const char *name,
                        const void *param, size_t param_len,
                        const void *val, size_t val_len);
int bcm_wl_iovar_getint(const char *ifname, const char *name, int32_t *out);
int bcm_wl_iovar_setint(const char *ifname, const char *name, int32_t val);
int bcm_wl_bsscfg_iovar_get(const char *ifname, int bsscfg_idx,
                            const char *name, void *out, size_t out_len);
int bcm_wl_bsscfg_iovar_set(const char *ifname, int bsscfg_idx,
                            const char *name, const void *val, size_t val_len);
int bcm_wl_bsscfg_iovar_getint(const char *ifname, int bsscfg_idx,
                               const char *name, int32_t *out);
int bcm_wl_bsscfg_iovar_setint(const char *ifname, int bsscfg_idx,
                               const char *name, int32_t val);
int bcm_wl_chspec_to_str(uint16_t chspec, char *out, size_t out_sz);
int bcm_wl_chspec_from_str(const char *str, uint16_t *out);
int bcm_wl_parse_mac(const char *str, uint8_t mac[6]);
void bcm_wl_format_mac(const uint8_t mac[6], char out[18], int lower);
int bcm_cli_wl(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif /* BCMOPEN_H */
