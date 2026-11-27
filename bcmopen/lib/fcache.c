/*
 * fcache.c — Open-source replacement for Broadcom /bin/fcctl + libfcctl.so
 *
 * Verified against /opt/bcm/libexec/fcctl and /opt/bcm/lib/libfcctl.so:
 *   - Device: /dev/fcache (O_RDWR, opened and closed per ioctl)
 *   - ioctl numbers: raw integers 0x64..0x8b (not _IOC encoded)
 *   - Exact FcStatusInfo_t (0x40 bytes) layout and flags0/flags1 bit assignments
 *   - Multi-command chaining, full config stdout messages, exact flwstats layout
 */
#include "bcmopen.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define FCACHE_DEV "/dev/fcache"

/* Exact ioctl numbers verified from libfcctl.asm:0xd20..0xea4 */
enum {
    FC_IOCTL_STATUS                 = 0x64,
    FC_IOCTL_ENABLE                 = 0x65,
    FC_IOCTL_DISABLE                = 0x67,
    FC_IOCTL_FLUSH                  = 0x68,
    FC_IOCTL_CFG_DEFER              = 0x69,
    FC_IOCTL_CFG_MCAST              = 0x6a,
    FC_IOCTL_CFG_IPV6               = 0x6b,
    FC_IOCTL_RESET_STATS            = 0x6c,
    FC_IOCTL_CFG_MONITOR            = 0x6d,
    FC_IOCTL_CFG_TIMER              = 0x6e,
    FC_IOCTL_QRY_CREATE             = 0x6f,
    FC_IOCTL_QRY_GET                = 0x70,
    FC_IOCTL_QRY_DELETE             = 0x71,
    FC_IOCTL_QRY_CLEAR              = 0x72,
    FC_IOCTL_QRY_NUM                = 0x73,
    FC_IOCTL_QRY_DUMP               = 0x74,
    FC_IOCTL_POLL_GET               = 0x75,
    FC_IOCTL_POLL_SET               = 0x76,
    FC_IOCTL_CFG_GRE                = 0x77,
    FC_IOCTL_CFG_L2TP               = 0x78,
    FC_IOCTL_CFG_ACCEL_MODE         = 0x7a,
    FC_IOCTL_DUMP                   = 0x7b,
    FC_IOCTL_CFG_TCP_ACK_MFLOWS     = 0x7c,
    FC_IOCTL_CFG_HW_ACCEL           = 0x7d,
    FC_IOCTL_CFG_LOW_PKT_RATE       = 0x7e,
    FC_IOCTL_CFG_NOTIFY_PROC_MODE   = 0x7f,
    FC_IOCTL_CFG_SW_DEFER           = 0x80,
    FC_IOCTL_CFG_4O6_FRAG           = 0x81,
    FC_IOCTL_CFG_TOS_MFLOWS         = 0x82,
    FC_IOCTL_CFG_UUCAST             = 0x83,
    FC_IOCTL_CFG_IDLE_TCP           = 0x84,
    FC_IOCTL_CFG_IDLE_UDP           = 0x85,
    FC_IOCTL_CFG_IDLE_L2            = 0x86,
    FC_IOCTL_CFG_IDLE_MCAST         = 0x87,
    FC_IOCTL_CFG_IDLE_UUCAST        = 0x88,
    FC_IOCTL_CFG_IDLE_OTHER         = 0x89,
    FC_IOCTL_MCAST_WHITELIST_STATS  = 0x8a,
    FC_IOCTL_CFG_TEST               = 0x8b,
};

#define FC_ERR_INVALID_CMD  103
#define FC_ERR_BAD_PARAMS   104
#define FC_ERR_OUT_OF_RANGE 105

static int fc_dev_ioctl_ex(int cmd, void *arg, int verbose) {
    int fd = open(FCACHE_DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (verbose) fprintf(stderr, "open <%s> error no %d\n", FCACHE_DEV, errno);
        return -1;
    }
    int r = ioctl(fd, (unsigned long)cmd, arg);
    if (r == -1 && verbose) {
        fprintf(stderr, "fcDevIoctl <%d> error\n", cmd);
    }
    close(fd);
    return r;
}

static int fc_dev_ioctl(int cmd, void *arg) {
    return fc_dev_ioctl_ex(cmd, arg, 1);
}

int bcm_fc_status(bcm_fc_status_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    return fc_dev_ioctl_ex(FC_IOCTL_STATUS, out, 0);
}

int bcm_fc_enable(void) {
    return fc_dev_ioctl_ex(FC_IOCTL_ENABLE, NULL, 0);
}

int bcm_fc_disable(void) {
    return fc_dev_ioctl_ex(FC_IOCTL_DISABLE, NULL, 0);
}

int bcm_fc_reset_stats(void) {
    return fc_dev_ioctl_ex(FC_IOCTL_RESET_STATS, NULL, 0);
}

int bcm_fc_flush(const bcm_fc_flush_req_t *req) {
    if (!req) return -1;
    bcm_fc_flush_req_t copy = *req;
    return fc_dev_ioctl_ex(FC_IOCTL_FLUSH, &copy, 0);
}

int bcm_fc_config(int ioctl_cmd, int val) {
    return fc_dev_ioctl_ex(ioctl_cmd, (void *)(uintptr_t)val, 0);
}

int bcm_fc_dump(uint32_t type, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t req[4] = { type, a, b, c };
    return fc_dev_ioctl_ex(FC_IOCTL_DUMP, req, 0);
}

int bcm_fc_mcast_stats(uint64_t out[4]) {
    if (!out) return -1;
    memset(out, 0, 4 * sizeof(uint64_t));
    return fc_dev_ioctl_ex(FC_IOCTL_MCAST_WHITELIST_STATS, out, 0);
}

static int parse_mac6(const char *s, uint8_t mac[6]) {
    unsigned b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xff) return -1;
        mac[i] = (uint8_t)b[i];
    }
    return 0;
}

static void fc_print_usage(void) {
    puts("Flow Cache Control Utility:\n");
    puts("Proc FileSystem: /proc/fcache \n");
    puts("::: Usage:\n");
    puts(":::::: Flow Cache SW System :");
    puts("       fc status");
    puts("       fc enable");
    puts("       fc disable");
    puts("       fc flush");
    puts("                   [ --hw ]");
    puts("                   [ --silent ]");
    puts("           - Flush all flows matching any combination of:");
    puts("                   [ --if   <interface> ]");
    puts("                 | [ --flow <flowid>    ]");
    puts("                 | [ <--mac | --dstmac | --srcmac> <macaddr> ]");
    puts("       fc resetstats");
    puts("       fc mcaststats");
    puts("       fc flwstats <n|g|d|c|p> [args]");
    puts("       fc config <option> <value>");
    puts("       fc dump [ --flow <id> | --mcast-bitmap ... | --unknown-ucast-bitmap ... | --blog ]");
}

static const char *onoff(int cond) {
    return cond ? "Enabled" : "Disabled";
}

static int fc_cmd_status(const char *prog) {
    bcm_fc_status_t st;
    memset(&st, 0, sizeof(st));
    if (fc_dev_ioctl(FC_IOCTL_STATUS, &st) == -1) {
        fprintf(stderr, "%s: failed to get Flow Cache status\n", prog);
        return -1;
    }

    printf("\tFlow Refresh Timer Interval  = %d millisecs\n", (int)st.flow_refresh_timer_ms);
    printf("\tTCP Flow Idle Timer          = %d s\n", (int)st.tcp_idle_s);
    printf("\tUDP Flow Idle Timer          = %d s\n", (int)st.udp_idle_s);
    printf("\tL2 Flow Idle Timer           = %d s\n", (int)st.l2_idle_s);
    printf("\tMcast Flow Idle Timer        = %d s\n", (int)st.mcast_idle_s);
    printf("\tUnknown Ucast Flow Idle Timer= %d s\n", (int)st.uucast_idle_s);
    printf("\tOther Flow Idle Timer        = %d s\n", (int)st.other_idle_s);
    printf("\tPkt-HW Activate Deferral rate = %d\n", (int)st.hw_activate_defer_rate);
    printf("\tPkt-HW Idle Deactivate = %d\n", (int)(st.flags0 & BCM_FC_F0_HW_IDLE_DEACT));
    printf("\tPkt-SW Activate Deferral count = %d\n", (int)st.sw_activate_defer_count);
    printf("\tFlow Low Pkt Rate = %d\n", (int)st.low_pkt_rate);
    printf("\tAcceleration Mode: <%s>\n",
           (st.flags1 & BCM_FC_F1_ACCEL_L2L3) ? "L2 & L3" : "L3");

    /* Exact format string verified at fcctl VA 0x14400 (including trailing spaces) */
    printf("\tMCast Acceleration IPv4<%s> IPv6<%s>\n"
           "\tIPv6 Learning <%s>\n"
           "\tUnknown Ucast Learning <%s>\n"
           "\tL2TP Learning <%s>\n"
           "\tGRE Learning <%s>\n"
           "\t4o6 Fragmentation <%s>\n"
           "\tTCP Ack Prioritization <%s> \n"
           "\tToS Multi Flow <%s> \n"
           "\tNotify Processing Mode <%s> \n"
           "\tOVS Flow Learning <%s> \n"
           "\tHW Acceleration <%s> \n",
           onoff(st.flags0 & BCM_FC_F0_MCAST_V4),
           onoff(st.flags0 & BCM_FC_F0_MCAST_V6),
           onoff(st.flags0 & BCM_FC_F0_IPV6_LEARN),
           onoff(st.flags0 & BCM_FC_F0_UUCAST_LEARN),
           onoff(st.flags1 & BCM_FC_F1_L2TP_LEARN),
           onoff(st.flags0 & BCM_FC_F0_GRE_LEARN),
           onoff(st.flags1 & BCM_FC_F1_4O6_FRAG),
           onoff(st.flags1 & BCM_FC_F1_TCP_ACK_PRIO),
           onoff(st.flags1 & BCM_FC_F1_TOS_MFLOWS),
           (st.flags1 & BCM_FC_F1_NOTIFY_HYBRID) ? "Hybrid" : "Now",
           onoff(st.flags0 & BCM_FC_F0_OVS_LEARN),
           onoff(st.flags1 & BCM_FC_F1_HW_ACCEL));

    printf("\tFlow Ucast Learning %s  : Max<%u>, Active<%u>, Cumulative [ %u - %u ]\n",
           onoff(st.flags0 & BCM_FC_F0_FLOW_LEARN),
           st.ucast_max, st.ucast_created - st.ucast_evicted,
           st.ucast_created, st.ucast_evicted);
    printf("\tFlow Mcast Learning %s  : Max<%u>, Active<%u>, Cumulative [ %u - %u ]\n",
           onoff(st.flags0 & BCM_FC_F0_FLOW_LEARN),
           st.mcast_max, st.mcast_created - st.mcast_evicted,
           st.mcast_created, st.mcast_evicted);
    printf("\tFlow Unknown Ucast Learning %s  : Max<%u>, Active<%u>, Cumulative [ %u - %u ]\n",
           onoff(st.flags0 & BCM_FC_F0_UUCAST_LEARN),
           st.uucast_max, st.uucast_created - st.uucast_evicted,
           st.uucast_created, st.uucast_evicted);
    return 0;
}

static int fc_cmd_flush(const char *prog, int argc, char **argv) {
    bcm_fc_flush_req_t req;
    memset(&req, 0, sizeof(req));
    const char *ifname = NULL;
    int silent = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--hw") == 0) {
            req.flags |= 0x20;
        } else if (strcmp(argv[i], "--silent") == 0) {
            silent = 1;
        } else if (strcmp(argv[i], "--if") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: must specify interface name\n", prog);
                return FC_ERR_BAD_PARAMS;
            }
            ifname = argv[++i];
            unsigned idx = if_nametoindex(ifname);
            if (idx == 0) {
                fprintf(stderr, "%s: invalid interface name\n", prog);
                return FC_ERR_BAD_PARAMS;
            }
            req.flags |= 0x04;
            req.ifindex = idx;
        } else if (strcmp(argv[i], "--flow") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: incorrect number of parameters\n", prog);
                return FC_ERR_BAD_PARAMS;
            }
            int fid = atoi(argv[++i]);
            if (fid <= 0) {
                fprintf(stderr, "%s: invalid from flowid\n", prog);
                return FC_ERR_BAD_PARAMS;
            }
            req.flags |= 0x02;
            req.flowid = (uint32_t)fid;
        } else if (strcmp(argv[i], "--dstmac") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: incorrect number of parameters\n", prog);
                return FC_ERR_BAD_PARAMS;
            }
            const char *ms = argv[++i];
            if (parse_mac6(ms, req.mac) < 0) {
                printf("ERROR:  Please express '%s' in MAC address format (i.e. '02:10:18:73:81:04').\n", ms);
                return FC_ERR_BAD_PARAMS;
            }
            req.flags = (req.flags & ~0x10u) | 0x08u;
        } else if (strcmp(argv[i], "--srcmac") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: incorrect number of parameters\n", prog);
                return FC_ERR_BAD_PARAMS;
            }
            const char *ms = argv[++i];
            if (parse_mac6(ms, req.mac) < 0) {
                printf("ERROR:  Please express '%s' in MAC address format (i.e. '02:10:18:73:81:04').\n", ms);
                return FC_ERR_BAD_PARAMS;
            }
            req.flags = (req.flags & ~0x08u) | 0x10u;
        } else if (strcmp(argv[i], "--mac") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: incorrect number of parameters\n", prog);
                return FC_ERR_BAD_PARAMS;
            }
            const char *ms = argv[++i];
            if (parse_mac6(ms, req.mac) < 0) {
                printf("ERROR:  Please express '%s' in MAC address format (i.e. '02:10:18:73:81:04').\n", ms);
                return FC_ERR_BAD_PARAMS;
            }
            req.flags |= 0x18u;
        } else {
            fprintf(stderr, "%s: invalid option [%s]\n", prog, argv[i]);
            return FC_ERR_INVALID_CMD;
        }
    }
    /* Stock fcctl.asm: only sets bit 0 (flush all) when no flags at all were set;
     * --hw alone sends flags = 0x20. */
    if (req.flags == 0) req.flags = 0x01;

    if (!silent) {
        puts("Broadcom Packet Flow Cache flushing the flows");
        if (req.flags & 0x02) printf("flow %d\n", (int)req.flowid);
        if ((req.flags & 0x04) && ifname) printf("if %s\n", ifname);
        if (req.flags & 0x10)
            printf("src mac %02x:%02x:%02x:%02x:%02x:%02x\n",
                   req.mac[0], req.mac[1], req.mac[2], req.mac[3], req.mac[4], req.mac[5]);
        if (req.flags & 0x08)
            printf("dst mac %02x:%02x:%02x:%02x:%02x:%02x\n",
                   req.mac[0], req.mac[1], req.mac[2], req.mac[3], req.mac[4], req.mac[5]);
        fputc('\n', stderr);
    }

    if (bcm_fc_flush(&req) == -1) {
        fprintf(stderr, "%s: failed to flush Flow Cache entries ", prog);
        return -1;
    }
    return 0;
}

static int fc_cmd_config(const char *prog, int argc, char **argv) {
    if (argc == 0) {
        fprintf(stderr, "%s: No options specified\n", prog);
        return FC_ERR_BAD_PARAMS;
    }
    const char *opt = argv[0];

    if (strcmp(opt, "--test") == 0) {
        if (argc < 2) {
            fprintf(stderr, "%s: must specify test mode action: 0 (disabled), or 1 (enabled) and adjust length\n", prog);
            return FC_ERR_BAD_PARAMS;
        }
        int mode = atoi(argv[1]);
        int p1 = 0, p2 = 0;
        if (mode != 0) {
            if (mode != 1 || argc != 4) return FC_ERR_BAD_PARAMS;
            p1 = atoi(argv[2]);
            p2 = atoi(argv[3]);
            if (p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255) {
                fprintf(stderr, "%s: adjust length can be between: 1 and 255\n", prog);
                return FC_ERR_BAD_PARAMS;
            }
        }
        int packed = ((p2 & 0xff) << 16) | ((p1 & 0xff) << 8) | (mode & 0xff);
        if (bcm_fc_config(FC_IOCTL_CFG_TEST, packed) == -1) {
            fprintf(stderr, "%s: failed to configure test mode\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache test mode <%d>\n", packed & 0xff);
        return 0;
    }

    if (argc != 2) {
        if (argc > 2) fprintf(stderr, "%s: Too many options specified\n", prog);
        else fprintf(stderr, "%s: incorrect number of parameters\n", prog);
        return FC_ERR_BAD_PARAMS;
    }
    int v = atoi(argv[1]);

    if (strcmp(opt, "--mcast") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_MCAST, v) == -1) {
            fprintf(stderr, "%s: failed to configure Multicast support\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache acceleration of multicast flows IPv4<%s> IPv6<%s>\n",
               (v & 1) ? "ENABLED" : "DISABLED", (v & 2) ? "ENABLED" : "DISABLED");
        return 0;
    }
    if (strcmp(opt, "--ipv6") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_IPV6, v) == -1) {
            fprintf(stderr, "%s: failed to configure IPv6 support\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Learning of IPv6 flows <%s>\n",
               v ? "ENABLED" : "DISABLED");
        return 0;
    }
    if (strcmp(opt, "--4o6-frag") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_4O6_FRAG, v) == -1) {
            fprintf(stderr, "%s: failed to configure 4o6 frag support\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Learning of 4o6 frag <%s>\n",
               v ? "ENABLED" : "DISABLED");
        return 0;
    }
    if (strcmp(opt, "--defer") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_DEFER, v) == -1) {
            fprintf(stderr, "%s: failed to configure defer value\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache HW Activate Deferral rate set to <%d>\n", v);
        return 0;
    }
    if (strcmp(opt, "--low-pkt-rate") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_LOW_PKT_RATE, v) == -1) {
            fprintf(stderr, "%s: failed to configure low-pkt-rate value\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Low Pkt Rate set to <%d>\n", v);
        return 0;
    }
    if (strcmp(opt, "--monitor") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_MONITOR, v) == -1) {
            fprintf(stderr, "%s: failed to configure monitor support\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Idle Deactivate monitor %s\n",
               v ? "ENABLED" : "DISABLED");
        return 0;
    }
    if (strcmp(opt, "--sw-defer") == 0) {
        if (v < 0 || v > 65535) {
            fprintf(stderr, "%s: sw-defer value<%d> out of range <0..65535> \n", prog, v);
            return FC_ERR_OUT_OF_RANGE;
        }
        if (bcm_fc_config(FC_IOCTL_CFG_SW_DEFER, v) == -1) {
            fprintf(stderr, "%s: failed to configure sw_defer value\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache SW Activate Deferral count set to <%d>\n", v);
        return 0;
    }
    if (strcmp(opt, "--timer") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_TIMER, v) == -1) {
            fprintf(stderr, "%s: failed to configure monitor support\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Setting Flow Timer Interval to %d ms\n", v);
        return 0;
    }
    if (strcmp(opt, "--idle-timer-tcp") == 0) {
        if (v < 0 || v > 65535) {
            fprintf(stderr, "%s: must specify valid TCP flow idle timer value <0..65535> sec\n", prog);
            return FC_ERR_BAD_PARAMS;
        }
        if (bcm_fc_config(FC_IOCTL_CFG_IDLE_TCP, v) == -1) {
            fprintf(stderr, "%s: failed to configure TCP flow idle timer\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Setting TCP flow idle timer to %d s\n", v);
        return 0;
    }
    if (strcmp(opt, "--idle-timer-udp") == 0) {
        if (v < 0 || v > 65535) {
            fprintf(stderr, "%s: must specify valid UDP flow idle timer value <0..65535> sec\n", prog);
            return FC_ERR_BAD_PARAMS;
        }
        if (bcm_fc_config(FC_IOCTL_CFG_IDLE_UDP, v) == -1) {
            fprintf(stderr, "%s: failed to configure UDP flow idle timer\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Setting UDP flow idle timer to %d s\n", v);
        return 0;
    }
    if (strcmp(opt, "--idle-timer-l2") == 0) {
        if (v < 0 || v > 65535) {
            fprintf(stderr, "%s: must specify valid L2 flow idle timer value <0..65535> sec\n", prog);
            return FC_ERR_BAD_PARAMS;
        }
        if (bcm_fc_config(FC_IOCTL_CFG_IDLE_L2, v) == -1) {
            fprintf(stderr, "%s: failed to configure L2 flow idle timer\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Setting L2 flow idle timer to %d s\n", v);
        return 0;
    }
    if (strcmp(opt, "--idle-timer-mcast") == 0) {
        if (v < 0 || v > 65535) {
            fprintf(stderr, "%s: must specify valid mcast flow idle timer value <0..65535> sec\n", prog);
            return FC_ERR_BAD_PARAMS;
        }
        if (bcm_fc_config(FC_IOCTL_CFG_IDLE_MCAST, v) == -1) {
            fprintf(stderr, "%s: failed to configure mcast flow idle timer\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Setting mcast flow idle timer to %d s\n", v);
        return 0;
    }
    if (strcmp(opt, "--idle-timer-other") == 0) {
        if (v < 0 || v > 65535) {
            fprintf(stderr, "%s: must specify valid other flow idle timer value <0..65535> sec\n", prog);
            return FC_ERR_BAD_PARAMS;
        }
        if (bcm_fc_config(FC_IOCTL_CFG_IDLE_OTHER, v) == -1) {
            fprintf(stderr, "%s: failed to configure other flow idle timer\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Setting other flow idle timer to %d s\n", v);
        return 0;
    }
    if (strcmp(opt, "--idle-timer-unknown-ucast") == 0) {
        if (v < 0 || v > 65535) {
            fprintf(stderr, "%s: must specify valid unknown ucast flow idle timer value <0..65535> sec\n", prog);
            return FC_ERR_BAD_PARAMS;
        }
        if (bcm_fc_config(FC_IOCTL_CFG_IDLE_UUCAST, v) == -1) {
            fprintf(stderr, "%s: failed to configure unknown ucast idle timer\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Setting unknown ucast flow idle timer to %d s\n", v);
        return 0;
    }
    if (strcmp(opt, "--gre") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_GRE, v) == -1) {
            fprintf(stderr, "%s: failed to configure GRE support\n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache Learning of GRE flows<%s>\n",
               v ? "ENABLED" : "DISABLED");
        return 0;
    }
    if (strcmp(opt, "--l2tp") == 0) {
        if (v >= 1) v = 1;
        if (bcm_fc_config(FC_IOCTL_CFG_L2TP, v) == -1) {
            fprintf(stderr, "%s: failed to configure L2TP support\n", prog);
            return -1;
        }
        puts(v ? "Broadcom Packet Flow Cache Learning of L2TP flows ENABLED"
               : "Broadcom Packet Flow Cache Learning of L2TP flows DISABLED");
        return 0;
    }
    if (strcmp(opt, "--accel-mode") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_ACCEL_MODE, v) == -1) {
            fprintf(stderr, "%s: failed to configure acceleration mode \n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache acceleration mode <%s>\n",
               v ? "L2 & L3" : "L3");
        return 0;
    }
    if (strcmp(opt, "--tcp-ack-mflows") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_TCP_ACK_MFLOWS, v) == -1) {
            fprintf(stderr, "%s: failed to configure TCP ACK Multi-Flow \n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache TCP ACK Multi-Flow <%s>\n",
               v ? "Enabled" : "Disabled");
        return 0;
    }
    if (strcmp(opt, "--tos-mflows") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_TOS_MFLOWS, v) == -1) {
            fprintf(stderr, "%s: failed to configure ToS Multi-Flow \n", prog);
            return -1;
        }
        return 0;
    }
    if (strcmp(opt, "--hw-accel") == 0) {
        int norm = (v != 0) ? 1 : 0;
        if (bcm_fc_config(FC_IOCTL_CFG_HW_ACCEL, norm) == -1) {
            fprintf(stderr, "%s: failed to %s hw acceleration \n", prog, norm ? "enable" : "disable");
            fprintf(stderr, "%s: failed to configure Flow Cache \n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache HW acceleration <%s>\n",
               norm ? "ENABLED" : "DISABLED");
        return 0;
    }
    if (strcmp(opt, "--unknown-ucast") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_UUCAST, v) == -1) {
            fprintf(stderr, "%s: failed to configure unknown ucast flow acceleration \n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache unknown ucast flow acceleration <%s>\n",
               v ? "Enabled" : "Disabled");
        return 0;
    }
    if (strcmp(opt, "--notify-proc-mode") == 0) {
        if (bcm_fc_config(FC_IOCTL_CFG_NOTIFY_PROC_MODE, v) == -1) {
            fprintf(stderr, "%s: failed to configure blog notify processing mode \n", prog);
            return -1;
        }
        printf("Broadcom Packet Flow Cache blog notify processing mode set to <%d>\n", v);
        return 0;
    }

    fprintf(stderr, "%s: invalid option [%s]\n", prog, opt);
    return FC_ERR_INVALID_CMD;
}

static void fc_print_tuple(const bcm_fc_tuple_req_t *q) {
    char ipbuf[INET6_ADDRSTRLEN];
    /* IPv4 fields stored in network byte order (verified at fcctl.asm:0x1303c..0x130c8) */
    if (q->flags & 0x1000) {
        if (inet_ntop(AF_INET, &q->srcv4, ipbuf, sizeof(ipbuf))) printf("srcv4=%s\n", ipbuf);
    }
    if (q->flags & 0x8000) {
        if (inet_ntop(AF_INET, &q->rxdstv4, ipbuf, sizeof(ipbuf))) printf("rxdstv4=%s\n", ipbuf);
    }
    if (q->flags & 0x0800) {
        if (inet_ntop(AF_INET, &q->dstv4, ipbuf, sizeof(ipbuf))) printf("dstv4=%s\n", ipbuf);
    }
    if (q->flags & 0x0400) {
        if (inet_ntop(AF_INET6, q->srcv6, ipbuf, sizeof(ipbuf))) printf("srcv6=%s\n", ipbuf);
    }
    if (q->flags & 0x0200) {
        if (inet_ntop(AF_INET6, q->dstv6, ipbuf, sizeof(ipbuf))) printf("dstv6=%s\n", ipbuf);
    }
    if (q->flags & 0x0100) printf("srcport=%d ", (int)ntohs(q->srcport));
    if (q->flags & 0x0080) printf("dstport=%d ", (int)ntohs(q->dstport));
    if (q->flags & 0x0040) printf("proto=%d ", (int)q->proto);
    if (q->flags & 0x0020) printf("invid=%d ", (int)ntohs(q->invid));
    if (q->flags & 0x0010) printf("outvid=%d ", (int)ntohs(q->outvid));
    if (q->flags & 0x400000) printf("txtos=%d ", (int)q->txtos);
    if (q->flags & 0x100000) printf("rxtos=%d ", (int)q->rxtos);
    if (q->flags & 0x800000) printf("txdscp=%d ", (int)q->txtos);
    if (q->flags & 0x200000) printf("rxdscp=%d ", (int)q->rxtos);
    if (q->flags & 0x0008)
        printf("srcmac=%02x:%02x:%02x:%02x:%02x:%02x ",
               q->srcmac[0], q->srcmac[1], q->srcmac[2], q->srcmac[3], q->srcmac[4], q->srcmac[5]);
    if (q->flags & 0x0004)
        printf("dstmac=%02x:%02x:%02x:%02x:%02x:%02x ",
               q->dstmac[0], q->dstmac[1], q->dstmac[2], q->dstmac[3], q->dstmac[4], q->dstmac[5]);
    if (q->flags & 0x20000)
        printf("inrxsrcmac=%02x:%02x:%02x:%02x:%02x:%02x ",
               q->inrxsrcmac[0], q->inrxsrcmac[1], q->inrxsrcmac[2],
               q->inrxsrcmac[3], q->inrxsrcmac[4], q->inrxsrcmac[5]);
    if (q->flags & 0x10000)
        printf("inrxdstmac=%02x:%02x:%02x:%02x:%02x:%02x ",
               q->inrxdstmac[0], q->inrxdstmac[1], q->inrxdstmac[2],
               q->inrxdstmac[3], q->inrxdstmac[4], q->inrxdstmac[5]);
    if (q->flags & 0x80000)
        printf("intxsrcmac=%02x:%02x:%02x:%02x:%02x:%02x ",
               q->intxsrcmac[0], q->intxsrcmac[1], q->intxsrcmac[2],
               q->intxsrcmac[3], q->intxsrcmac[4], q->intxsrcmac[5]);
    if (q->flags & 0x40000)
        printf("intxdstmac=%02x:%02x:%02x:%02x:%02x:%02x ",
               q->intxdstmac[0], q->intxdstmac[1], q->intxdstmac[2],
               q->intxdstmac[3], q->intxdstmac[4], q->intxdstmac[5]);
    if (q->flags & 0x0002) printf("srcphy=%s ", q->srcphy);
    if (q->flags & 0x0001) printf("dstphy=%s ", q->dstphy);
    if (q->flags & 0x4000) printf("%s", q->srcipver ? "srcipver=v6 " : "srcipver=v4 ");
    if (q->flags & 0x2000) printf("%s", q->dstipver ? "dstipver=v6 " : "dstipver=v4 ");
}

static int parse_v4_addr(const char *val, uint32_t *out) {
    /* Network byte order verified at libfcctl.asm:0x1308..0x13b8 */
    struct in_addr a4;
    if (inet_pton(AF_INET, val, &a4) != 1) {
        printf("ERROR:  Please express '%s' in IPv4 dotted decimal format.\n", val);
        return -1;
    }
    *out = a4.s_addr;
    return 0;
}

static int parse_mac_err(const char *val, uint8_t mac[6]) {
    if (parse_mac6(val, mac) < 0) {
        printf("ERROR:  Please express '%s' in MAC address format (i.e. '02:10:18:73:81:04').\n", val);
        return -1;
    }
    return 0;
}

static int fc_set_tuple_param(const char *field, const char *val, bcm_fc_tuple_req_t *q) {
    if (strcasecmp(field, "dstphy") == 0) {
        strncpy(q->dstphy, val, 15); q->flags |= 0x0001; return 0;
    } else if (strcasecmp(field, "srcphy") == 0) {
        strncpy(q->srcphy, val, 15); q->flags |= 0x0002; return 0;
    } else if (strcasecmp(field, "dstmac") == 0) {
        if (parse_mac_err(val, q->dstmac) < 0) return -1;
        q->flags |= 0x0004; return 0;
    } else if (strcasecmp(field, "srcmac") == 0) {
        if (parse_mac_err(val, q->srcmac) < 0) return -1;
        q->flags |= 0x0008; return 0;
    } else if (strcasecmp(field, "outvid") == 0) {
        /* Host-order 16-bit verified at libfcctl.asm:0x119c */
        q->outvid = (uint16_t)strtol(val, NULL, 0); q->flags |= 0x0010; return 0;
    } else if (strcasecmp(field, "invid") == 0) {
        q->invid = (uint16_t)strtol(val, NULL, 0); q->flags |= 0x0020; return 0;
    } else if (strcasecmp(field, "proto") == 0) {
        q->proto = (uint8_t)strtol(val, NULL, 0); q->flags |= 0x0040; return 0;
    } else if (strcasecmp(field, "dstport") == 0) {
        /* Host-order 16-bit verified at libfcctl.asm:0x1124 */
        q->dstport = (uint16_t)strtol(val, NULL, 0); q->flags |= 0x0080; return 0;
    } else if (strcasecmp(field, "srcport") == 0) {
        /* Host-order 16-bit verified at libfcctl.asm:0x10f0 */
        q->srcport = (uint16_t)strtol(val, NULL, 0); q->flags |= 0x0100; return 0;
    } else if (strcasecmp(field, "dstv6") == 0) {
        if (inet_pton(AF_INET6, val, q->dstv6) != 1) {
            printf("ERROR:  Please express '%s' in IPv6 format.\n", val);
            return -1;
        }
        q->flags |= 0x0200; return 0;
    } else if (strcasecmp(field, "srcv6") == 0) {
        if (inet_pton(AF_INET6, val, q->srcv6) != 1) {
            printf("ERROR:  Please express '%s' in IPv6 format.\n", val);
            return -1;
        }
        q->flags |= 0x0400; return 0;
    } else if (strcasecmp(field, "dstv4") == 0) {
        if (parse_v4_addr(val, &q->dstv4) < 0) return -1;
        q->flags |= 0x0800; return 0;
    } else if (strcasecmp(field, "srcv4") == 0) {
        if (parse_v4_addr(val, &q->srcv4) < 0) return -1;
        q->flags |= 0x1000; return 0;
    } else if (strcasecmp(field, "dstipver") == 0) {
        /* Verified at libfcctl.asm:0x16f0..0x1744 */
        if (strcasecmp(val, "v4") == 0) q->dstipver = 0;
        else if (strcasecmp(val, "v6") == 0) q->dstipver = 1;
        else {
            printf("ERROR:  Please express '%s' as 'v4' or 'v6'.\n", val);
            return -1;
        }
        q->flags |= 0x2000; return 0;
    } else if (strcasecmp(field, "srcipver") == 0) {
        /* Verified at libfcctl.asm:0x1680..0x16e4 */
        if (strcasecmp(val, "v4") == 0) q->srcipver = 0;
        else if (strcasecmp(val, "v6") == 0) q->srcipver = 1;
        else {
            printf("ERROR:  Please express '%s' as 'v4' or 'v6'.\n", val);
            return -1;
        }
        q->flags |= 0x4000; return 0;
    } else if (strcasecmp(field, "rxdstv4") == 0) {
        if (parse_v4_addr(val, &q->rxdstv4) < 0) return -1;
        q->flags |= 0x8000; return 0;
    } else if (strcasecmp(field, "inrxdstmac") == 0) {
        if (parse_mac_err(val, q->inrxdstmac) < 0) return -1;
        q->flags |= 0x10000; return 0;
    } else if (strcasecmp(field, "inrxsrcmac") == 0) {
        if (parse_mac_err(val, q->inrxsrcmac) < 0) return -1;
        q->flags |= 0x20000; return 0;
    } else if (strcasecmp(field, "intxdstmac") == 0) {
        if (parse_mac_err(val, q->intxdstmac) < 0) return -1;
        q->flags |= 0x40000; return 0;
    } else if (strcasecmp(field, "intxsrcmac") == 0) {
        if (parse_mac_err(val, q->intxsrcmac) < 0) return -1;
        q->flags |= 0x80000; return 0;
    } else if (strcasecmp(field, "rxtos") == 0) {
        q->rxtos = (uint8_t)strtol(val, NULL, 0); q->flags |= 0x100000;
        if ((q->flags & 0x300000) == 0x300000) return -2;
        return 0;
    } else if (strcasecmp(field, "rxdscp") == 0) {
        q->rxtos = (uint8_t)strtol(val, NULL, 0); q->flags |= 0x200000;
        if ((q->flags & 0x300000) == 0x300000) return -2;
        return 0;
    } else if (strcasecmp(field, "txtos") == 0) {
        q->txtos = (uint8_t)strtol(val, NULL, 0); q->flags |= 0x400000;
        if ((q->flags & 0xc00000) == 0xc00000) return -2;
        return 0;
    } else if (strcasecmp(field, "txdscp") == 0) {
        q->txtos = (uint8_t)strtol(val, NULL, 0); q->flags |= 0x800000;
        if ((q->flags & 0xc00000) == 0xc00000) return -2;
        return 0;
    }
    fprintf(stderr, "ERROR:  Please use a valid field name instead of '%s'\n", field);
    return -1;
}

static int fc_cmd_flwstats(const char *prog, int argc, char **argv) {
    if (argc < 1) return FC_ERR_BAD_PARAMS;
    if (strcmp(argv[0], "n") == 0) {
        if (argc < 3 || ((argc - 1) & 1) != 0) {
            fprintf(stderr, "%s: Unmatched field/value pair.\n", prog);
            return FC_ERR_BAD_PARAMS;
        }
        bcm_fc_tuple_req_t q;
        memset(&q, 0, sizeof(q));
        for (int i = 1; i + 1 < argc; i += 2) {
            int r = fc_set_tuple_param(argv[i], argv[i + 1], &q);
            if (r == -2) {
                puts("ERROR:  either tos or dscp can be selected but not both");
                return -2;
            }
            if (r < 0) return -1;
        }
        if (fc_dev_ioctl(FC_IOCTL_QRY_CREATE, &q) < 0) return -1;
        printf("  New query created.  ID=%u\n", q.query_id);
        fc_print_tuple(&q);
        fputs("\n\n", stdout);
        return 0;
    }
    if (strcmp(argv[0], "g") == 0 || strcmp(argv[0], "d") == 0 || strcmp(argv[0], "c") == 0) {
        char op = argv[0][0];
        if (argc != 2) return FC_ERR_BAD_PARAMS;
        /* Verified from fcctl.asm 0x13e18..0x13fa8: handle @ +0x00, rxpkts @ +0x08, rtpdrop @ +0x0c, rxbytes @ +0x10 */
        uint8_t buf[0x90];
        memset(buf, 0, sizeof(buf));
        uint32_t id = ((op == 'd' || op == 'c') && strcasecmp(argv[1], "all") == 0)
                      ? 256u : (uint32_t)atoi(argv[1]);
        memcpy(buf + 0x00, &id, 4);
        int cmd = (op == 'g') ? FC_IOCTL_QRY_GET : (op == 'd') ? FC_IOCTL_QRY_DELETE : FC_IOCTL_QRY_CLEAR;
        int r = fc_dev_ioctl(cmd, buf);
        if (r != 0) {
            fprintf(stderr, "%s: No query with ID=%u (err %d).\n", prog, id, r);
            return r;
        }
        if (op == 'g') {
            uint32_t rxpkts = 0, rtpdrop = 0;
            uint64_t rxbytes = 0;
            memcpy(&rxpkts, buf + 0x08, 4);
            memcpy(&rtpdrop, buf + 0x0c, 4);
            memcpy(&rxbytes, buf + 0x10, 8);
            printf("  Query results: rxpkts=%lu, rxbytes=%llu, rxrtppacketsdropped=%lu\n",
                   (unsigned long)rxpkts, (unsigned long long)rxbytes, (unsigned long)rtpdrop);
        }
        return 0;
    }
    if (strcmp(argv[0], "p") == 0) {
        int num = fc_dev_ioctl(FC_IOCTL_QRY_NUM, NULL);
        if (num < 0) {
            fprintf(stderr, "%s: Error %d when dump all query info.\n", prog, num);
            return -1;
        }
        if (num == 0) {
            puts("FLWSTATS: No active queries.");
            return 0;
        }
        /* Verified from fcctl.asm 0x139f8..0x13b88: 0x19c-byte header + entry ptr at +0x198 */
        uint8_t *entries = calloc((size_t)num, 168);
        if (!entries) return -1;
        uint8_t dump_req[0x1a0];
        memset(dump_req, 0, sizeof(dump_req));
        memcpy(dump_req + 0x00, &num, 4);
        uintptr_t eptr = (uintptr_t)entries;
        memcpy(dump_req + 0x198, &eptr, sizeof(eptr));
        int r = fc_dev_ioctl(FC_IOCTL_QRY_DUMP, dump_req);
        if (r != 0) {
            fprintf(stderr, "%s: Error %d ioctl fail when dump all query info.\n", prog, r);
            free(entries);
            return r;
        }
        puts("  Query ID:     Packets:               Bytes:RTP Pkts Lost:Flag:     Tuple");
        puts("  --------:------------:--------------------:-------------:----:----------");
        for (int i = 0; i < num; i++) {
            const uint8_t *e = entries + (size_t)i * 168;
            /* Exact byte offsets verified at fcctl.asm:0x13b48..0x13b88 */
            uint8_t qid = e[0x00];
            uint8_t flg = e[0x01];
            uint32_t pkts = 0;
            int32_t rtpdrop = 0;
            uint64_t bytes = 0;
            memcpy(&pkts, e + 0x08, 4);
            memcpy(&rtpdrop, e + 0x0c, 4);
            memcpy(&bytes, e + 0x10, 8);
            printf("  %8d:%12lu:%20llu:%13d:%4d: ",
                   (int)qid, (unsigned long)pkts, (unsigned long long)bytes, rtpdrop, (int)flg);
            bcm_fc_tuple_req_t tup;
            memcpy(&tup, e + 0x18, sizeof(tup));
            fc_print_tuple(&tup);
            putchar('\n');
        }
        puts("\n  prevSt Array Dump:");
        puts("  ------------------");
        for (int i = 0; i < 25; i++) {
            const uint8_t *ps = dump_req + 0x08 + (size_t)i * 16;
            uint32_t pkts = 0, lost = 0;
            uint64_t bytes = 0;
            memcpy(&pkts, ps + 0x00, 4);
            memcpy(&lost, ps + 0x04, 4);
            memcpy(&bytes, ps + 0x08, 8);
            printf("  prevSt[%d].rx_packets: %lu prevSt[%d].rx_bytes: %llu prevSt[%d].rx_rtp_packets_lost: %lu \n",
                   i, (unsigned long)pkts, i, (unsigned long long)bytes, i, (unsigned long)lost);
        }
        free(entries);
        return 0;
    }
    fprintf(stderr, "%s: invalid option [%s]\n", prog, argv[0]);
    return FC_ERR_INVALID_CMD;
}

static int fc_cmd_dump(const char *prog, int argc, char **argv) {
    if (argc < 1) return FC_ERR_BAD_PARAMS;
    /* Exact type numbers verified at fcctl.asm 0x13594..0x13818 & libfcctl.asm 0xedc..0xf60 */
    if (strcmp(argv[0], "--flow") == 0 || strcmp(argv[0], "--blog") == 0) {
        if (argc != 2) {
            fprintf(stderr, "%s: must specify flow id \n", prog);
            return FC_ERR_BAD_PARAMS;
        }
        int fid = atoi(argv[1]);
        if (fid < 0) {
            fprintf(stderr, "%s: invalid flow id \n", prog);
            return FC_ERR_BAD_PARAMS;
        }
        uint32_t type = (strcmp(argv[0], "--blog") == 0) ? 2u : 1u;
        return bcm_fc_dump(type, (uint32_t)fid, 0, 0);
    }
    if (strcmp(argv[0], "--mcast-bitmap") == 0) {
        if (argc == 1) return bcm_fc_dump(3, 0xffffffffu, 0xffffffffu, 0);
        if (argc == 4)
            return bcm_fc_dump(3, (uint32_t)atoi(argv[1]),
                               (uint32_t)atoi(argv[2]),
                               (uint32_t)atoi(argv[3]));
        return FC_ERR_BAD_PARAMS;
    }
    if (strcmp(argv[0], "--unknown-ucast-bitmap") == 0) {
        if (argc == 1) return bcm_fc_dump(4, 0xffffffffu, 0xffffffffu, 0);
        if (argc == 4)
            return bcm_fc_dump(4, (uint32_t)atoi(argv[1]),
                               (uint32_t)atoi(argv[2]),
                               (uint32_t)atoi(argv[3]));
        return FC_ERR_BAD_PARAMS;
    }
    fprintf(stderr, "%s: invalid option [%s]\n", prog, argv[0]);
    return FC_ERR_INVALID_CMD;
}

static int is_fc_cmd(const char *s) {
    return strcmp(s, "status") == 0 || strcmp(s, "enable") == 0 ||
           strcmp(s, "disable") == 0 || strcmp(s, "resetstats") == 0 ||
           strcmp(s, "flush") == 0 || strcmp(s, "config") == 0 ||
           strcmp(s, "flwstats") == 0 || strcmp(s, "dump") == 0 ||
           strcmp(s, "mcaststats") == 0;
}

static int fc_run_one(const char *prog, const char *cmd, int argc, char **argv) {
    if (strcmp(cmd, "status") == 0) {
        return fc_cmd_status(prog);
    } else if (strcmp(cmd, "enable") == 0) {
        if (bcm_fc_enable() == -1) {
            fprintf(stderr, "%s: failed to enable Flow Cache\n", prog);
            return -1;
        }
        puts("Broadcom Packet Flow Cache learning via BLOG enabled.");
        return 0;
    } else if (strcmp(cmd, "disable") == 0) {
        if (bcm_fc_disable() == -1) {
            fprintf(stderr, "%s: failed to disable Flow Cache\n", prog);
            return -1;
        }
        puts("Broadcom Packet Flow Cache learning via BLOG disabled.");
        return 0;
    } else if (strcmp(cmd, "resetstats") == 0) {
        if (bcm_fc_reset_stats() == -1) {
            fprintf(stderr, "%s: failed to reset Flow Cache stats\n", prog);
            return -1;
        }
        puts("Broadcom Packet Flow Cache Reset Stats.");
        return 0;
    } else if (strcmp(cmd, "flush") == 0) {
        return fc_cmd_flush(prog, argc, argv);
    } else if (strcmp(cmd, "config") == 0) {
        return fc_cmd_config(prog, argc, argv);
    } else if (strcmp(cmd, "flwstats") == 0) {
        return fc_cmd_flwstats(prog, argc, argv);
    } else if (strcmp(cmd, "dump") == 0) {
        return fc_cmd_dump(prog, argc, argv);
    } else if (strcmp(cmd, "mcaststats") == 0) {
        uint64_t s[4] = {0};
        int r = bcm_fc_mcast_stats(s);
        if (r == 0) {
            printf("fcCtlMcastWhitelistStatsHandler: rx_pkt=%lld, rx_byte=%lld, dropped_pkt=%lld, dropped_byte=%lld\n",
                   (long long)s[0], (long long)s[1], (long long)s[2], (long long)s[3]);
        }
        return r == 0 ? 0 : -1;
    }

    fprintf(stderr, "%s: invalid command\n", prog);
    return FC_ERR_INVALID_CMD;
}

int bcm_cli_fcctl(int argc, char **argv) {
    const char *prog = argv[0];
    if (argc <= 1) {
        fc_print_usage();
        return 0;
    }

    /* Support chaining multiple commands in one invocation (e.g. `fcctl disable flush enable`) */
    int i = 1;
    int rc = 0;
    while (i < argc) {
        const char *cmd = argv[i++];
        if (!is_fc_cmd(cmd)) {
            fprintf(stderr, "%s: invalid command\n", prog);
            return FC_ERR_INVALID_CMD;
        }
        int start = i;
        if (strcmp(cmd, "status") == 0 || strcmp(cmd, "enable") == 0 ||
            strcmp(cmd, "disable") == 0 || strcmp(cmd, "resetstats") == 0 ||
            strcmp(cmd, "mcaststats") == 0) {
            /* 0-arg commands: consume no trailing args so chained commands work */
        } else {
            while (i < argc && !is_fc_cmd(argv[i])) i++;
        }
        rc = fc_run_one(prog, cmd, i - start, argv + start);
        if (rc != 0) return rc;
    }
    return rc;
}
