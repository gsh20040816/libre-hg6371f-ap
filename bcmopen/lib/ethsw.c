/*
 * ethsw.c — Open-source replacement for Broadcom /bin/ethswctl + libethswctl.so
 *
 * Verified against /opt/bcm/libexec/ethswctl & /opt/bcm/lib/libethswctl.so:
 *   - Control interface: AF_INET DGRAM socket bound via SIOCGIFINDEX (0x8933)
 *     probing "bcmsw" then "eth0" (ethswctl_init @ 0x2b00)
 *   - Switch ioctl:  SIOCETHSWCTLOPS = 0x89fd, ifr_data -> ethswctl_data_t (456 B)
 *   - Clear MIB:     SIOCSCLEARMIBCNTR = 0x89f2
 *   - WAN ioctls:    SIOCSWANPORT = 0x89f9, SIOCGWANPORT = 0x89fa
 *   - Extended list: SIOCIFREQ_EXT = 0x89ff, ifr_data -> ifreq_ext_t (24 B on ARM32)
 */
#include "bcmopen.h"

#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define BCM_SIOCSCLEARMIBCNTR  0x89f2UL
#define BCM_SIOCGQUERYNUMPORTS 0x89f7UL
#define BCM_SIOCSWANPORT       0x89f9UL
#define BCM_SIOCGWANPORT       0x89faUL
#define BCM_SIOCETHCTLOPS      0x89fbUL
#define BCM_SIOCETHSWCTLOPS    0x89fdUL
#define BCM_SIOCIFREQ_EXT      0x89ffUL
#ifndef SIOCGIFINDEX
#define SIOCGIFINDEX          0x8933UL
#endif

/* Exact Broadcom ETHSW* op codes verified from libethswctl.so */
enum {
    ETHSW_OP_MIBDUMP          = 0,
    ETHSW_OP_PORT_CREATE      = 1,
    ETHSW_OP_PORT_DELETE      = 2,
    ETHSW_OP_GPON_MCAST_GEM   = 3,
    ETHSW_OP_SWITCHING        = 4,
    ETHSW_OP_PAUSE            = 7,
    ETHSW_OP_COSQ_SCHED       = 11,
    ETHSW_OP_RX_RATE_CTRL     = 14,
    ETHSW_OP_TX_RATE_CTRL     = 15,
    ETHSW_OP_TEST             = 16,
    ETHSW_OP_COSQ_PRIO_METHOD = 18,
    ETHSW_OP_INFO             = 21,
    ETHSW_OP_LINKSTATUS       = 22,
    ETHSW_OP_PORTCTRL         = 23,
    ETHSW_OP_PORT_LOOPBACK    = 24,
    ETHSW_OP_PHYMODE          = 25,
    ETHSW_OP_IFNAME           = 30,
    ETHSW_OP_UNIT_PORT        = 32,
    ETHSW_OP_DOS_CTRL         = 33,
    ETHSW_OP_SNOOP_CTRL       = 34,
    ETHSW_OP_SOFT_SWITCHING   = 36,
    ETHSW_OP_OAM_IDX          = 58,
    ETHSW_OP_BOND_CHK         = 62,
};

enum {
    ETHSW_TYPE_DISABLE = 0,
    ETHSW_TYPE_ENABLE  = 1,
    ETHSW_TYPE_GET     = 2,
    ETHSW_TYPE_SET     = 3,
    ETHSW_TYPE_HELP    = 7,
};

/*
 * Exact 456-byte binary layout of struct ethswctl_data verified from libethswctl.so:
 *   +0x00  int32_t  op
 *   +0x08  int32_t  unit
 *   +0x0c  int32_t  port
 *   +0x14  int32_t  sub_port
 *   +0x18  int32_t  addressing_flag
 *   +0x1c  int32_t  type
 *   +0x20  union payload (speed/val8/dos[15]/snoop/test_type/port_cfg/sched_mode)
 *   +0x24  switching / duplex / oam_idx / snoop_buf_len
 *   +0x28  snoop_buf_ptr / loopback_speed / createport is_wan
 *   +0x30  createport ifname[16] / bondchk ifname2[16]
 *   +0x34  uint32_t phycfg (phymode SET capability bitmask)
 *   +0x38  int32_t  ret_val
 *   +0x3c  int32_t  val
 *   +0x40  bondchk err_buf ptr
 *   +0x48  bondchk err_len (in) / status (out) | cosqsched weights[4] (0x48..0x57)
 *   +0x74  int32_t  cosqpriomethod pkt_type_mask
 *   +0xa8  int32_t  txratectrl queue (-1 or 0..3)
 *   +0xac  int32_t  test sub_param
 *   +0xb4  uint32_t limit_kbps
 *   +0xb8  uint32_t burst_kbits
 *   +0xe0  uint32_t portmap (ETHSW_OP_UNIT_PORT)
 *   +0xf0  char     ifname[16]
 */
#define ETHSW_DATA_BYTES 456

typedef union {
    uint8_t raw[ETHSW_DATA_BYTES];
    struct {
        int32_t  op;              /* +0x00 */
        int32_t  rsv04;           /* +0x04 */
        int32_t  unit;            /* +0x08 */
        int32_t  port;            /* +0x0c */
        int32_t  rsv10;           /* +0x10 */
        int32_t  sub_port;        /* +0x14 */
        int32_t  addressing_flag; /* +0x18 */
        int32_t  type;            /* +0x1c */
        int32_t  param0;          /* +0x20 */
        int32_t  param1;          /* +0x24 */
        int32_t  param2;          /* +0x28 */
        uint8_t  pad2c[0x08];     /* +0x2c..+0x33 */
        uint32_t phycfg;          /* +0x34 */
        int32_t  ret_val;         /* +0x38 */
        int32_t  val;             /* +0x3c */
        uint8_t  pad40[0x34];     /* +0x40..+0x73 */
        int32_t  pkt_type_mask;   /* +0x74 */
        uint8_t  pad78[0x30];     /* +0x78..+0xa7 */
        int32_t  queue;           /* +0xa8 */
        int32_t  test_param;      /* +0xac */
        uint32_t rsvb0;           /* +0xb0 */
        uint32_t limit_kbps;      /* +0xb4 */
        uint32_t burst_kbits;     /* +0xb8 */
        uint8_t  padbc[0x24];     /* +0xbc..+0xdf */
        uint32_t portmap;         /* +0xe0 */
        uint8_t  pade4[0x0c];     /* +0xe4..+0xef */
        char     ifname[16];      /* +0xf0..+0xff */
    } f;
} ethswctl_data_t;

_Static_assert(sizeof(ethswctl_data_t) == ETHSW_DATA_BYTES, "ethswctl_data_t must be 456 bytes");
_Static_assert(offsetof(ethswctl_data_t, f.phycfg) == 0x34, "phycfg @ 0x34");
_Static_assert(offsetof(ethswctl_data_t, f.pkt_type_mask) == 0x74, "pkt_type_mask @ 0x74");
_Static_assert(offsetof(ethswctl_data_t, f.queue) == 0xa8, "queue @ 0xa8");
_Static_assert(offsetof(ethswctl_data_t, f.test_param) == 0xac, "test_param @ 0xac");
_Static_assert(offsetof(ethswctl_data_t, f.portmap) == 0xe0, "portmap @ 0xe0");
_Static_assert(offsetof(ethswctl_data_t, f.ifname) == 0xf0, "ifname @ 0xf0");

typedef struct {
    uint32_t opcode;    /* +0x00: 0=WANOnly, 1=WanPreferred, 2=LANOnly, 3=LanWan */
    uint32_t pad04;     /* +0x04 */
    char    *stringBuf; /* +0x08 (4B on ARM32 + pad0c, or 8B on LP64) */
#if UINTPTR_MAX == 0xffffffffu
    uint32_t pad0c;     /* +0x0c */
#endif
    uint32_t bufLen;    /* +0x10 */
    uint32_t pad14;     /* +0x14 */
} ifreq_ext_t;

_Static_assert(sizeof(ifreq_ext_t) == 24, "ifreq_ext_t must be 24 bytes");

static int ethsw_init(struct ifreq *ifr) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("ethswctl_init: socket open error");
        return -1;
    }
    memset(ifr, 0, sizeof(*ifr));
    strcpy(ifr->ifr_name, "bcmsw");
    if (ioctl(fd, SIOCGIFINDEX, ifr) >= 0) return fd;

    strcpy(ifr->ifr_name, "eth0");
    if (ioctl(fd, SIOCGIFINDEX, ifr) >= 0) return fd;

    close(fd);
    fputs("ethswctl_init: neither bcmsw nor eth0 exist\n", stderr);
    return -1;
}

static int ethsw_ioctl(ethswctl_data_t *e) {
    struct ifreq ifr;
    int fd = ethsw_init(&ifr);
    if (fd < 0) {
        fputs("ethswctl_init failed. \n", stderr);
        return -1;
    }
    ifr.ifr_data = (void *)e;
    int r = ioctl(fd, BCM_SIOCETHSWCTLOPS, &ifr);
    if (r != 0 && !((e->f.op == ETHSW_OP_DOS_CTRL || e->f.op == ETHSW_OP_SNOOP_CTRL) && r == 16)) {
        if (r == -EOPNOTSUPP)
            fputs("ioctl not supported!\n", stderr);
        else if (e->f.op == ETHSW_OP_IFNAME || e->f.op == ETHSW_OP_PORTCTRL ||
                 e->f.op == ETHSW_OP_PHYMODE || e->f.op == ETHSW_OP_TEST ||
                 e->f.op == ETHSW_OP_PORT_LOOPBACK)
            fputs("ioctl command return error!\n", stderr);
        else
            fprintf(stderr, "ioctl command return error %d!\n", r);
    }
    close(fd);
    return r;
}

static int ethsw_fail(void) {
    fputs("Failure\n", stderr);
    return 255;
}

static int ethsw_get_wan_lan_list(uint32_t opcode, char *out, size_t out_sz) {
    struct ifreq ifr;
    int fd = ethsw_init(&ifr);
    if (fd < 0) return -1;

    char buf[1090];
    memset(buf, 0, sizeof(buf));
    ifreq_ext_t ext;
    memset(&ext, 0, sizeof(ext));
    ext.opcode = opcode;
    ext.stringBuf = buf;
    ext.bufLen = (uint32_t)sizeof(buf);
    ifr.ifr_data = (void *)&ext;

    int r = ioctl(fd, BCM_SIOCIFREQ_EXT, &ifr);
    int err = errno;
    close(fd);
    if (r < 0) {
        fprintf(stderr, "getWanLanAttrList: SIOCIFREQ_EXT opcode=0x%x failed, errno=%d\n",
                opcode, err);
        return -2;
    }
    size_t slen = strlen(buf);
    if (slen >= out_sz) return -3;
    memcpy(out, buf, slen + 1);
    return 0;
}

static void ethsw_usage(void) {
    fputs("Usage:\n"
          "    ethswctl {-c|--command} hw-switching {-o|--operation} <enable|disable>\n"
          "    ethswctl {-c|--command} softswitch [-i] <if_name> [-o] <enable|disable>\n"
          "    ethswctl {-c|--command} mibdump [-n <unit>] {-p|--port} <port> {-a|--all}\n"
          "    ethswctl {-c|--command} bondchk -i if_name1 -j if_name2\n"
          "    ethswctl {-c|--command} pause [-n <unit>] -p <port> [-v <val>]\n"
          "    ethswctl {-c|--command} portctrl [-n <unit>] {-p|--port} <port> [-v <val>]\n"
          "    ethswctl {-c|--command} portloopback [-n <unit>] {-p|--port} <port[.sub_port]> [-v <val>] [-s <speed>]\n"
          "    ethswctl {-c|--command} phymode [-n <unit>] {-p|--port} <port> [-y <speed>] [-z <duplex>]\n"
          "    ethswctl {-c|--command} test -t <type> [-x <param>] [-v <val>] [-s]\n"
          "    ethswctl {-c|--command} dosctrl [-n <unit>] -v <value>\n"
          "    ethswctl {-c|--command} snoopctrl [-n <unit>] -v <value>\n"
          "    ethswctl {-c|--command} setlinkstatus [-n <unit>] -p port -x linkstatus -y speed -z duplex\n"
          "    ethswctl {-c|--command} wan [ -i if_name -o <enable|disable> ]\n"
          "    ethswctl {-c|--command} getwanonly\n"
          "    ethswctl {-c|--command} getwanpreferred\n"
          "    ethswctl {-c|--command} getlanonly\n"
          "    ethswctl {-c|--command} getlanwan\n"
          "    ethswctl {-c|--command} rxratectrl [-n <unit>] -p port [-x limit-kbps -y burst-kbits]\n"
          "    ethswctl {-c|--command} txratectrl [-n <unit>] -p port [-x limit-kbps -y burst-kbits]\n"
          "    ethswctl {-c|--command} cosqpriomethod [-n <unit>] -p port [-v <val>]\n"
          "    ethswctl {-c|--command} cosqsched -v [val] -t <sp_endq> -w <weight1> -x <weight2>\n"
          "    ethswctl {-c|--command} oamindex -v <oam index> -t <0=unit/port; 1=rdpa_if>]\n"
          "    ethswctl {-c|--command} createport -p <port> -w <is_wan> -i <name>\n"
          "    ethswctl {-c|--command} deleteport -p <port>\n"
          "    ethswctl {-c|--command} gponmcastgemset -p <mcast_gem_idx>\n"
          "    ethswctl {-c|--command} keepalive -x <timeout(sec)> -y <retries> -z <iterations> -v <sleep(sec)>\n"
          "\n"
          "Usage: ethswctl <command> [arguments...]\n"
          "  getifname : Get the interface name for the given port\n"
          "  ethswctl getifname <unit> <port>\n"
          "      NOTE : if only one argument; unit is assumed 0\n\n\n",
          stderr);
}

static int ethsw_cmd_getifname(int argc, char **argv) {
    int unit = 0, port = 0;
    if (argc == 3) {
        port = atoi(argv[2]);
    } else if (argc == 4) {
        unit = atoi(argv[2]);
        port = atoi(argv[3]);
    } else {
        fputs("incorrect number of arguments\n", stderr);
        return 255;
    }
    ethswctl_data_t e;
    memset(&e, 0, sizeof(e));
    e.f.op = ETHSW_OP_IFNAME;
    e.f.unit = unit;
    e.f.port = port;
    e.f.type = ETHSW_TYPE_GET;
    if (ethsw_ioctl(&e) != 0) return 255;
    e.f.ifname[15] = '\0';
    printf("The interface name for unit %d port %d is %s\n", unit, port, e.f.ifname);
    return 0;
}

/* Exact mapping verified at libethswctl.asm:0x5188..0x532c (ETHSWUNITPORT = 32) */
static int ethsw_map_ifname_to_up(const char *ifname, int *unit, int *port) {
    ethswctl_data_t e;
    memset(&e, 0, sizeof(e));
    e.f.op = ETHSW_OP_UNIT_PORT;
    e.f.type = ETHSW_TYPE_GET;
    strncpy(e.f.ifname, ifname, 15);
    if (ethsw_ioctl(&e) != 0) return -1;
    *unit = e.f.unit;
    uint32_t pm = e.f.portmap;
    *port = pm ? __builtin_ctz(pm) : 0;
    return 0;
}

/* Exact implementation of libethswctl.so bcm_get_phyid (0x9210..0x9558) */
static int ethsw_get_phyid(int unit, int port, int *sub_port_io) {
    ethswctl_data_t e;
    memset(&e, 0, sizeof(e));
    e.f.op = ETHSW_OP_IFNAME;
    e.f.unit = unit;
    e.f.port = port;
    e.f.type = ETHSW_TYPE_GET;
    if (ethsw_ioctl(&e) != 0) {
        fprintf(stderr, "No such port, unit %d, port %d\n", unit, port);
        return -1;
    }
    e.f.ifname[15] = '\0';
    const char *ifname = e.f.ifname;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fputs("ethswctl_open_socket failed. \n", stderr);
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        close(fd);
        fputs("ethswctl_open_socket failed. \n", stderr);
        return -1;
    }

    int32_t portmap = 0;
    ifr.ifr_data = (void *)&portmap;
    if (ioctl(fd, BCM_SIOCGQUERYNUMPORTS, &ifr) < 0) {
        fprintf(stderr, "Error: Interface %s ioctl SIOCGQUERYNUMPORTS error!\n", ifname);
        close(fd);
        return -1;
    }

    int sp = sub_port_io ? *sub_port_io : -1;
    if (portmap < 0) {
        close(fd);
        return -1;
    }
    if (portmap == 0) {
        if (sp != -1) {
            fprintf(stderr, "Interface %s has no sub port\n", ifname);
            close(fd);
            return -1;
        }
    } else if (sp == -1) {
        int cnt = __builtin_popcount((unsigned)portmap);
        if (cnt > 1) {
            char list[128] = {0};
            int seen = 0;
            for (int bit = 0; bit < 32; bit++) {
                if (!(((unsigned)portmap >> bit) & 1u)) continue;
                if (seen > 0) strcat(list, (seen == cnt - 1) ? " or " : ", ");
                char num[16];
                snprintf(num, sizeof(num), "%d", bit);
                strcat(list, num);
                seen++;
            }
            fprintf(stderr, "Error: %s has sub ports (%s), specify one of them by \"port <subport>\"\n",
                    ifname, list);
            close(fd);
            return -1;
        }
        sp = __builtin_ctz((unsigned)portmap);
    } else {
        if (sp < 0 || sp >= 32 || !(((unsigned)portmap >> sp) & 1u)) {
            fprintf(stderr, "Specified SubPort %d is not interface %s's member port with map 0x%x\n",
                    sp, ifname, (unsigned)portmap);
            close(fd);
            return -1;
        }
    }

    /* SIOCETHCTLOPS (0x89fb) op=22 (ETHGETPHYID): sub_port @ +0x18, phyid out @ +0x1c */
    uint8_t ethctl[116];
    memset(ethctl, 0, sizeof(ethctl));
    int32_t op_phyid = 22;
    int32_t sp32 = sp;
    memcpy(ethctl + 0x00, &op_phyid, 4);
    memcpy(ethctl + 0x18, &sp32, 4);
    ifr.ifr_data = (void *)ethctl;
    int r = ioctl(fd, BCM_SIOCETHCTLOPS, &ifr);
    close(fd);
    if (r != 0) return -1;

    int32_t phyid = -1;
    memcpy(&phyid, ethctl + 0x1c, 4);
    if (phyid >= 0 && sub_port_io) *sub_port_io = sp;
    return phyid;
}

static uint32_t phymode_speed_to_cfg(int speed, int duplex) {
    /* Exact bitmask verified at libethswctl.asm:0x8e70..0x8eb8 & .rodata:0xc084 */
    uint32_t cfg = 0x200;
    switch (speed) {
    case 0:     cfg = 0x200; break;
    case 10:    cfg = 0x002; break;
    case 100:   cfg = 0x008; break;
    case 1000:  cfg = 0x020; break;
    case 2500:  cfg = 0x040; break;
    case 5000:  cfg = 0x080; break;
    case 10000: cfg = 0x100; break;
    default:    cfg = 0x200; break;
    }
    if ((speed == 10 || speed == 100 || speed == 1000) && duplex == 0)
        cfg >>= 1;
    return cfg;
}

int bcm_cli_ethswctl(int argc, char **argv) {
    if (argc <= 1) {
        fputs("more than 1 or no exclusive option is entered\n", stderr);
        ethsw_usage();
        return 254;
    }

    /* Positional command mode (e.g. `ethswctl getifname [unit] <port>`) */
    if (argv[1][0] != '-') {
        if (strcmp(argv[1], "getifname") == 0) {
            return ethsw_cmd_getifname(argc, argv);
        }
        fprintf(stderr, "invalid command [%s]\n", argv[1]);
        return 255;
    }

    const char *cmd = NULL;
    const char *op = NULL;
    const char *ifname = NULL;
    const char *ifname2 = NULL;
    int unit = 0, has_unit = 0, port = 0, has_port = 0, sub_port = -1, all_flag = 0;
    int has_val = 0, has_x = 0, has_y = 0, has_z = 0, has_t = 0, has_w = 0, has_s = 0, has_q = 0, has_h = 0;
    int val = 0, param_x = 0, param_y = 0, param_z = 0, param_t = 0, param_w = 0, param_s = 0, param_q = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            has_h = 1;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--command") == 0) {
            if (i + 1 >= argc) return 254;
            cmd = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--operation") == 0) {
            if (i + 1 >= argc) return 254;
            op = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) return 254;
            unit = (int)strtol(argv[++i], NULL, 0);
            has_unit = 1;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) return 254;
            const char *ps = argv[++i];
            const char *dot = strchr(ps, '.');
            port = (int)strtol(ps, NULL, 0);
            has_port = 1;
            if (dot) sub_port = (int)strtol(dot + 1, NULL, 0);
        } else if (strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc) return 254;
            ifname = argv[++i];
        } else if (strcmp(argv[i], "-j") == 0) {
            if (i + 1 >= argc) return 254;
            ifname2 = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            if (i + 1 >= argc) return 254;
            val = (int)strtol(argv[++i], NULL, 0);
            has_val = 1;
        } else if (strcmp(argv[i], "-x") == 0) {
            if (i + 1 >= argc) return 254;
            param_x = (int)strtol(argv[++i], NULL, 0);
            has_x = 1;
        } else if (strcmp(argv[i], "-y") == 0) {
            if (i + 1 >= argc) return 254;
            param_y = (int)strtol(argv[++i], NULL, 0);
            has_y = 1;
        } else if (strcmp(argv[i], "-z") == 0) {
            if (i + 1 >= argc) return 254;
            param_z = (int)strtol(argv[++i], NULL, 0);
            has_z = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc) return 254;
            param_t = (int)strtol(argv[++i], NULL, 0);
            has_t = 1;
        } else if (strcmp(argv[i], "-w") == 0) {
            if (i + 1 >= argc) return 254;
            param_w = (int)strtol(argv[++i], NULL, 0);
            has_w = 1;
        } else if (strcmp(argv[i], "-q") == 0) {
            if (i + 1 >= argc) return 254;
            param_q = (int)strtol(argv[++i], NULL, 0);
            has_q = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            has_s = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                param_s = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            all_flag = 1;
        }
    }

    if (!cmd || (has_h && strcmp(cmd, "snoopctrl") != 0)) {
        fputs("more than 1 or no exclusive option is entered\n", stderr);
        ethsw_usage();
        return 254;
    }

    ethswctl_data_t e;
    memset(&e, 0, sizeof(e));
    e.f.unit = unit;
    e.f.port = port;
    (void)has_w; (void)has_q;

    if (strcmp(cmd, "hw-switching") == 0) {
        e.f.op = ETHSW_OP_SWITCHING;
        if (!op) {
            e.f.type = ETHSW_TYPE_GET;
            if (ethsw_ioctl(&e) != 0 || e.f.ret_val != 0) return ethsw_fail();
            puts(e.f.param1 ? "Enabled " : "Disabled ");
            puts("Success.");
            return 0;
        }
        if (strcmp(op, "enable") == 0) e.f.type = ETHSW_TYPE_ENABLE;
        else if (strcmp(op, "disable") == 0) e.f.type = ETHSW_TYPE_DISABLE;
        else return 254;
        if (ethsw_ioctl(&e) != 0 || e.f.ret_val != 0) return ethsw_fail();
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "softswitch") == 0) {
        e.f.op = ETHSW_OP_SOFT_SWITCHING;
        if (!ifname || !op) {
            e.f.type = ETHSW_TYPE_GET;
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
            printf("Soft switching enabled port map is %x\n", (unsigned)e.f.param1);
            puts("Success.");
            return 0;
        }
        if (strcmp(op, "enable") == 0) e.f.type = ETHSW_TYPE_ENABLE;
        else if (strcmp(op, "disable") == 0) e.f.type = ETHSW_TYPE_DISABLE;
        else return 254;
        int u = 0, p = 0;
        if (ethsw_map_ifname_to_up(ifname, &u, &p) != 0) return ethsw_fail();
        e.f.unit = u;
        e.f.port = p;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "mibdump") == 0) {
        e.f.op = ETHSW_OP_MIBDUMP;
        e.f.unit = has_unit ? unit : (has_port ? 0 : -1);
        e.f.port = has_port ? (has_s ? (port | 0x80) : port) : -1;
        e.f.type = all_flag ? 1 : 0;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "pause") == 0) {
        if (!has_port) {
            fputs("Port(-p) should be specified\n", stderr);
            return 254;
        }
        e.f.op = ETHSW_OP_PAUSE;
        if (has_val) {
            if (val < 0 || val > 7) return 254;
            e.f.type = ETHSW_TYPE_SET;
            e.f.val = val;
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
            puts("Success.");
            return 0;
        }
        e.f.type = ETHSW_TYPE_GET;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        int v8 = e.f.param0 & 0xff;
        printf("The val8 = %d\n", v8);
        /* Exact mapping verified at ethswctl.asm 0x124a8..0x12568 */
        switch (v8) {
        case 2: puts("Both Tx and Rx Pause Enabled"); break;
        case 3: puts("Tx Pause Enabled"); break;
        case 4: puts("Rx Pause Enabled"); break;
        case 5: puts("Both Tx and Rx PFC Enabled"); break;
        case 6:
            puts("Tx PFC Enabled");
            puts("Current Run Time PFC timers:");
            for (int c = 0; c < 8; c++) {
                uint32_t t = 0;
                memcpy(&t, e.raw + 0x24 + c * 4, 4);
                printf("    Class %d: 0x%x\n", c, t);
            }
            break;
        case 7: puts("Rx PFC Enabled"); break;
        default: puts("Pause Disabled"); break;
        }
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "portctrl") == 0) {
        e.f.op = ETHSW_OP_PORTCTRL;
        if (has_val) {
            e.f.type = ETHSW_TYPE_SET;
            e.f.val = val;
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
            /* Stock ethswctl reads back and prints current Rx/Tx state after SET */
        }
        e.f.type = ETHSW_TYPE_GET;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        /* ret_val: bit 0 = Rx disable, bit 1 = Tx disable (verified from ethswctl.asm 0x12600) */
        printf("Unit %d Port %d: Rx is %s, Tx is %s\n",
               unit, port,
               (e.f.ret_val & 1) ? "Disabled" : "Enabled",
               (e.f.ret_val & 2) ? "Disabled" : "Enabled");
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "portloopback") == 0) {
        if (!has_port) {
            fputs("Port(-p) should be specified\n", stderr);
            return 254;
        }
        int resolved_sp = sub_port;
        if (ethsw_get_phyid(unit, port, &resolved_sp) < 0) {
            fputs("    ethswctl {-c|--command} portloopback [-n <unit>] {-p|--port} <port[.sub_port]> [-v <val>] [-s <speed>]\n"
                  "          val:  1-Enable, 0-Disable\n"
                  "          speed: speed in the Mbps; If omitted, maximum interface speed will be used\n"
                  "          sub_port for -p: needed if PHY is behind a crossbar with multiple member ports\n",
                  stderr);
            return 0;
        }
        e.f.op = ETHSW_OP_PORT_LOOPBACK;
        e.f.sub_port = resolved_sp;
        e.f.addressing_flag = 2;
        if (has_val) {
            e.f.type = ETHSW_TYPE_SET;
            e.f.val = val;
            e.f.param2 = param_s;
            if (ethsw_ioctl(&e) != 0) {
                fputs("Loopback set failed.\n", stderr);
                return ethsw_fail();
            }
            puts("Success.");
            return 0;
        }
        e.f.type = ETHSW_TYPE_GET;
        if (ethsw_ioctl(&e) != 0) {
            fputs("Loopback get failed.\n", stderr);
            return ethsw_fail();
        }
        if (e.f.ret_val)
            printf("Unit %d Port %d loopback is enabled at speed %dMbps\n", unit, port, e.f.param2);
        else
            printf("Unit %d Port %d loopback is disabled\n", unit, port);
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "phymode") == 0) {
        if (!has_port) {
            fputs("Port(-p) should be specified\n", stderr);
            return 254;
        }
        e.f.op = ETHSW_OP_PHYMODE;
        e.f.addressing_flag = 0;
        if (has_y || has_z) {
            if (!has_y || (param_y != 0 && param_y != 10 && param_y != 100 &&
                           param_y != 1000 && param_y != 2500 && param_y != 5000 && param_y != 10000)) {
                fputs("Invalid speed\n", stderr);
                return 254;
            }
            if ((param_y != 0 && !has_z) || (has_z && param_z != 0 && param_z != 1)) {
                fputs("Invalid duplex\n", stderr);
                return 254;
            }
            e.f.type = ETHSW_TYPE_SET;
            e.f.param0 = param_y;
            e.f.param1 = param_z;
            e.f.phycfg = phymode_speed_to_cfg(param_y, param_z);
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
            puts("Success.");
            return 0;
        }
        e.f.type = ETHSW_TYPE_GET;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        /* On link-down, kernel returns ioctl==0 and ret_val==-1; stock prints "Down\nSuccess.\n" */
        int speed = (e.f.ret_val == -1) ? 0 : e.f.param0;
        const char *spd = NULL;
        switch (speed) {
        case 10000: spd = "10G"; break;
        case 5000:  spd = "5G"; break;
        case 2500:  spd = "2.5G"; break;
        case 1000:  spd = "1G"; break;
        case 100:   spd = "100M"; break;
        case 10:    spd = "10M"; break;
        default:    break;
        }
        if (!spd) {
            puts("Down");
        } else if (e.f.param1 == 0) {
            printf("%s HD\n", spd);
        } else if (e.f.param1 == 1) {
            printf("%s FD\n", spd);
        } else {
            printf("%s\n", spd);
        }
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "setlinkstatus") == 0) {
        if (!has_port || !has_x || !has_y || !has_z) {
            fputs("    ethswctl {-c|--command} setlinkstatus [-n <unit>] -p port -x linkstatus -y speed -z duplex\n", stderr);
            return 254;
        }
        /* Verified at libethswctl.asm:0x83b4..0x83c8: speed (-y) @ +0x20, duplex (-z) @ +0x24 */
        e.f.op = ETHSW_OP_LINKSTATUS;
        e.f.type = ETHSW_TYPE_SET;
        e.f.param0 = param_y;
        e.f.param1 = param_z;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "dosctrl") == 0) {
        e.f.op = ETHSW_OP_DOS_CTRL;
        if (has_val) {
            e.f.type = ETHSW_TYPE_SET;
            for (int b = 0; b < 15; b++) e.raw[0x20 + b] = (uint8_t)((val >> b) & 1);
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
            puts("Success.");
            return 0;
        }
        e.f.type = ETHSW_TYPE_GET;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        uint8_t *d = e.raw + 0x20;
        printf(" DA_SA=%d; IP_LAN=%d; TCP_BLAT=%d; UDP_BLAT=%d; TCP_NULL=%d;\n",
               d[0], d[1], d[2], d[3], d[4]);
        printf(" TCP_XMAS=%d; TCP_SYNFIN=%d; TCP_SYNERR=%d; TCP_SHORTHDR=%d;\n",
               d[5], d[6], d[7], d[8]);
        printf(" TCP_FRAGERR=%d; ICMPv4_FRAG=%d; ICMPv6_FRAG=%d; ICMPv4_LONGPING=%d;\n",
               d[9], d[10], d[11], d[12]);
        printf(" ICMPv6_LONGPING=%d;DOS_LRN=%d\n\n", d[13], d[14]);
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "snoopctrl") == 0) {
        e.f.op = ETHSW_OP_SNOOP_CTRL;
        if (has_h) {
            char sbuf[1024];
            memset(sbuf, 0, sizeof(sbuf));
            e.f.type = ETHSW_TYPE_HELP;
            /* Verified at libethswctl.asm:0x9ea8..0x9ec0: buf_len @ +0x24, buf_ptr @ +0x28 */
            uint32_t slen = (uint32_t)sizeof(sbuf);
            uint32_t sptr = (uint32_t)(uintptr_t)sbuf;
            memcpy(e.raw + 0x24, &slen, 4);
            memcpy(e.raw + 0x28, &sptr, 4);
            if (ethsw_ioctl(&e) != 0) return 0;
            fputs(sbuf, stderr);
            return 0;
        }
        if (has_val) {
            e.f.type = ETHSW_TYPE_SET;
            e.f.param0 = val;
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
            puts("Success.");
            return 0;
        }
        char sbuf[1024];
        memset(sbuf, 0, sizeof(sbuf));
        e.f.type = ETHSW_TYPE_GET;
        /* Verified at libethswctl.asm:0x9d4c..0x9d64: buf_len @ +0x24, buf_ptr @ +0x28 */
        uint32_t slen = (uint32_t)sizeof(sbuf);
        uint32_t sptr = (uint32_t)(uintptr_t)sbuf;
        memcpy(e.raw + 0x24, &slen, 4);
        memcpy(e.raw + 0x28, &sptr, 4);
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        printf("    val=0x%x\n%s\n", (unsigned)e.f.param0, sbuf);
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "getwanonly") == 0 || strcmp(cmd, "getwanpreferred") == 0 ||
        strcmp(cmd, "getlanonly") == 0 || strcmp(cmd, "getlanwan") == 0) {
        uint32_t opc = 0;
        const char *hdr = "WAN Only";
        if (strcmp(cmd, "getwanpreferred") == 0) { opc = 1; hdr = "WAN preferred"; }
        else if (strcmp(cmd, "getlanonly") == 0) { opc = 2; hdr = "LAN Only"; }
        else if (strcmp(cmd, "getlanwan") == 0)  { opc = 3; hdr = "LAN or WAN"; }
        char list[1090];
        if (ethsw_get_wan_lan_list(opc, list, sizeof(list)) != 0) return ethsw_fail();
        printf("%s:%s\n", hdr, list);
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "wan") == 0) {
        if (ifname && op) {
            struct ifreq ifr;
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return ethsw_fail();
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
                close(fd);
                return ethsw_fail();
            }
            int32_t en;
            if (strcmp(op, "enable") == 0) en = 1;
            else if (strcmp(op, "disable") == 0) en = 0;
            else {
                close(fd);
                ethsw_usage();
                return 254;
            }
            memcpy((uint8_t *)&ifr + 0x10, &en, 4);
            int r = ioctl(fd, BCM_SIOCSWANPORT, &ifr);
            close(fd);
            if (r < 0) return ethsw_fail();
            puts("Success.");
            return 0;
        }
        struct ifreq ifr;
        int fd = ethsw_init(&ifr);
        if (fd < 0) return ethsw_fail();
        char names[665];
        memset(names, 0, sizeof(names));
        ethswctl_data_t we;
        memset(&we, 0, sizeof(we));
        uint32_t nptr = (uint32_t)(uintptr_t)names;
        uint32_t nlen = (uint32_t)sizeof(names);
        memcpy(we.raw + 0x20, &nptr, 4);
        memcpy(we.raw + 0x28, &nlen, 4);
        ifr.ifr_data = (void *)&we;
        int r = ioctl(fd, BCM_SIOCGWANPORT, &ifr);
        close(fd);
        if (r < 0) return ethsw_fail();
        printf("Interface(s) %s marked WAN\n", names);
        puts(names);
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "rxratectrl") == 0 || strcmp(cmd, "txratectrl") == 0) {
        if (!has_port) {
            fputs("Port(-p) should be specified\n", stderr);
            return 254;
        }
        if (has_x != has_y) {
            fputs("Both -x and -y must be specified\n", stderr);
            return 254;
        }
        int is_rx = (strcmp(cmd, "rxratectrl") == 0);
        e.f.op = is_rx ? ETHSW_OP_RX_RATE_CTRL : ETHSW_OP_TX_RATE_CTRL;
        if (has_x && has_y) {
            if (!is_rx) e.f.queue = param_q; /* default -1, verified at libethswctl.asm:0x4678 */
            e.f.type = ETHSW_TYPE_SET;
            e.f.limit_kbps = (uint32_t)param_x;
            e.f.burst_kbits = (uint32_t)param_y;
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
            puts("Success.");
            return 0;
        }
        if (!is_rx) e.f.queue = -1; /* stock ethswctl.asm:0x1339c always passes -1 on GET */
        e.f.type = ETHSW_TYPE_GET;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        if (is_rx && e.f.limit_kbps == 0)
            printf("Port %d ingress rate control is disabled\n", port);
        else
            printf("Port %d %s rate is %d kbps and burst size is %d kbits\n",
                   port, is_rx ? "ingress" : "egress", (int)e.f.limit_kbps, (int)e.f.burst_kbits);
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "cosqpriomethod") == 0) {
        e.f.op = ETHSW_OP_COSQ_PRIO_METHOD;
        e.f.pkt_type_mask = param_t; /* verified at libethswctl.asm:0x6b24,0x6c70 */
        if (has_val) {
            if (val < 0 || val > 7) return 254;
            e.f.type = ETHSW_TYPE_SET;
            e.f.val = val;
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
            puts("Success.");
            return 0;
        }
        e.f.type = ETHSW_TYPE_GET;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        static const char *const qos_names[4] = {
            "PortBasedQoS", "MACBasedQoS", "802.1PQoS", "DiffServeQoS"
        };
        int m = e.f.ret_val;
        const char *mstr = (m >= 0 && m < 3) ? qos_names[m] : qos_names[3];
        if (m <= 1) printf("QoS Method: %s\n", mstr);
        else        printf("Port %d QoS Method: %s\n", port, mstr);
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "cosqsched") == 0) {
        /* Verified at libethswctl.asm:0x6540..0x6760 & ethswctl.asm:0x13520..0x13648 */
        e.f.op = ETHSW_OP_COSQ_SCHED;
        e.f.port = 0; /* stock ethswctl.asm:0x13554..0x135b8 always passes port=0 */
        if (has_val) {
            e.f.type = ETHSW_TYPE_SET;
            uint16_t mode16 = (uint16_t)val;
            uint16_t sp16 = (uint16_t)param_t;
            /* Exact weight mapping verified at ethswctl.asm:0x13574..0x135a8: {-x, -y, -z, -w} */
            int32_t w[4] = { param_x, param_y, param_z, param_w };
            memcpy(e.raw + 0x20, &mode16, 2);
            memcpy(e.raw + 0x22, &sp16, 2);
            memcpy(e.raw + 0x48, w, sizeof(w));
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
            puts("Success.");
            return 0;
        }
        e.f.type = ETHSW_TYPE_GET;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        uint16_t mode16 = 0, sp16 = 0;
        int32_t w[4] = {0};
        memcpy(&mode16, e.raw + 0x20, 2);
        memcpy(&sp16, e.raw + 0x22, 2);
        memcpy(w, e.raw + 0x48, sizeof(w));
        if (mode16 == 0) {
            puts("Strict Priority Scheduling");
        } else if (mode16 == 1) {
            puts("Weighted Round Robin Scheduling");
            printf("Weights: %d %d %d %d\n", w[0], w[1], w[2], w[3]);
        } else if (mode16 == 2) {
            puts("SP+WRR combo scheduling");
            printf("SP from Queue3 to Queue%d and WRR for remaining queues\n", (int)sp16);
            printf("Weights: %d %d %d %d\n", w[0], w[1], w[2], w[3]);
        }
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "oamindex") == 0) {
        if (!has_val || !has_t) {
            fputs("    ethswctl {-c|--command} oamindex -v <oam index> -t <0=unit/port; 1=rdpa_if>]\n", stderr);
            return 254;
        }
        e.f.op = ETHSW_OP_OAM_IDX;
        e.f.type = ETHSW_TYPE_GET; /* verified at libethswctl.asm:0x5380..0x53a0 */
        e.f.param1 = val;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        printf("oam_index %d maps to unit=%d, port=%d\n", val, e.f.unit, e.f.port);
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "gponmcastgemset") == 0) {
        if (!has_port) {
            fputs("    ethswctl {-c|--command} gponmcastgemset -p <mcast_gem_idx>\n", stderr);
            return 254;
        }
        e.f.op = ETHSW_OP_GPON_MCAST_GEM;
        e.f.unit = 0;
        e.f.port = 0;
        e.f.val = port;
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        printf("port %d set as gpon mcast gem idx\n", port);
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "createport") == 0 || strcmp(cmd, "deleteport") == 0) {
        int is_create = (strcmp(cmd, "createport") == 0);
        if (!has_port) {
            fputs(is_create
                  ? "    ethswctl {-c|--command} createport -p <port> -w <is_wan> -i <name>\n"
                  : "    ethswctl {-c|--command} deleteport -p <port>\n",
                  stderr);
            return 254;
        }
        /* Verified at libethswctl.asm:0x2db0..0x2ed4: zeroed struct; port @ +0x20, is_wan @ +0x28, ifname @ +0x30 */
        memset(&e, 0, sizeof(e));
        e.f.op = is_create ? ETHSW_OP_PORT_CREATE : ETHSW_OP_PORT_DELETE;
        memcpy(e.raw + 0x20, &port, 4);
        if (is_create) {
            memcpy(e.raw + 0x28, &param_w, 4);
            if (ifname) strncpy((char *)(e.raw + 0x30), ifname, 15);
        }
        if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        if (is_create) {
            e.raw[0x3f] = '\0';
            printf("port %d created as %s with name %s\n", port,
                   param_w ? "WAN" : "LAN", (const char *)(e.raw + 0x30));
        } else {
            printf("port %d deleted\n", port);
        }
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "test") == 0) {
        if (!has_t) {
            fputs("Type (-t option) should be specified\n", stderr);
            return 254;
        }
        if (param_t == 0 && !has_x) {
            fputs("Parameter (-x option) should be specified for type 0\n", stderr);
            return 254;
        }
        e.f.op = ETHSW_OP_TEST;
        e.f.param0 = param_t;
        if (has_val) {
            e.f.type = ETHSW_TYPE_SET;
            e.f.val = val;
            if (param_t == 0) e.f.test_param = param_x;
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
            puts("Success.");
            return 0;
        }
        e.f.type = ETHSW_TYPE_GET;
        if (param_t == 0) e.f.test_param = param_x;
        else if (param_t == 3) e.f.test_param = has_s ? (param_x | 0x80) : param_x;
        if (param_t == 2) {
            /* Verified at libethswctl.asm:0x4fec..0x5028: also issue SIOCSCLEARMIBCNTR (0x89f2) */
            struct ifreq ifr;
            int fd = ethsw_init(&ifr);
            if (fd < 0) {
                fputs("ethswctl_init failed. \n", stderr);
                return ethsw_fail();
            }
            ifr.ifr_data = (void *)&e;
            int r = ioctl(fd, BCM_SIOCETHSWCTLOPS, &ifr);
            if (r != 0) {
                fputs("ioctl command return error!\n", stderr);
                close(fd);
                return ethsw_fail();
            }
            r = ioctl(fd, BCM_SIOCSCLEARMIBCNTR, &ifr);
            close(fd);
            if (r != 0) {
                fputs("ioctl SIOCSCLEARMIBCNTR return error!\n", stderr);
                return ethsw_fail();
            }
        } else {
            if (ethsw_ioctl(&e) != 0) return ethsw_fail();
        }
        printf("ret val is %d\n", e.f.ret_val);
        puts("Success.");
        return 0;
    }

    if (strcmp(cmd, "bondchk") == 0) {
        if (!ifname || !ifname2) {
            fputs("    ethswctl {-c|--command} bondchk -i if_name1 -j if_name2\n", stderr);
            return 254;
        }
        /* Verified at libethswctl.asm:0xa300..0xa388:
         * kernel writes bstatus @ +0x48 and err_buf @ +0x40 even when ioctl() returns -1 */
        struct ifreq ifr;
        int fd = ethsw_init(&ifr);
        if (fd < 0) {
            fputs("ethswctl_init failed. \n", stderr);
            return ethsw_fail();
        }
        char err_buf[256];
        memset(err_buf, 0, sizeof(err_buf));
        e.f.op = ETHSW_OP_BOND_CHK;
        e.f.type = ETHSW_TYPE_GET;
        strncpy((char *)(e.raw + 0x20), ifname, 15);
        strncpy((char *)(e.raw + 0x30), ifname2, 15);
        uint32_t eptr = (uint32_t)(uintptr_t)err_buf;
        int32_t elen = (int32_t)sizeof(err_buf);
        memcpy(e.raw + 0x40, &eptr, 4);
        memcpy(e.raw + 0x48, &elen, 4);
        ifr.ifr_data = (void *)&e;
        int r = ioctl(fd, BCM_SIOCETHSWCTLOPS, &ifr);
        if (r == -EOPNOTSUPP) fputs("ioctl not supported!\n", stderr);
        close(fd);
        int32_t bstatus = 0;
        memcpy(&bstatus, e.raw + 0x48, 4);
        if (bstatus != 0) {
            err_buf[sizeof(err_buf) - 1] = '\0';
            printf("Error: %s\n", err_buf);
            fputs("Failure\n", stderr);
            return bstatus;
        }
        puts("Success.");
        return 0;
    }

    fprintf(stderr, "ethswctl: unsupported or unimplemented command '%s'\n", cmd);
    return ethsw_fail();
}
