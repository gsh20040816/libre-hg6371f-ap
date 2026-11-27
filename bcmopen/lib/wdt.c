/*
 * wdt.c — Open-source replacement for Broadcom /bin/wdtctl
 *
 * Verified against /opt/bcm/libexec/wdtctl ARM32 disassembly:
 *   - Device: /dev/watchdog (O_WRONLY)
 *   - ioctls: WDIOC_SETPRETIMEOUT (0xC0045708, default 3s),
 *             WDIOC_SETTIMEOUT    (0xC0045706, [4..600], default 80s),
 *             WDIOC_SETOPTIONS    (0x80045704, WDIOS_DISABLECARD = 1)
 *   - Keepalive: write(fd, "", 1) every (timeout >> 2) seconds
 *   - timeleft: reads /sys/class/watchdog/<dev>/timeleft
 *   - SIGHUP: disables watchdog via WDIOC_SETOPTIONS, unlinks /var/run/wdtd.pid, exits 0
 *   - SIGKILL: never sends magic close 'V' so ap-rollback-guard hardware reset works
 */
#include "bcmopen.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#define WDT_DEV_PATH   "/dev/watchdog"
#define WDT_PID_FILE   "/var/run/wdtd.pid"
#define WDT_DAEMON_STR "wdtd"

#define WDT_IOCTL_SETOPTIONS    0x80045704UL
#define WDT_IOCTL_SETTIMEOUT    0xC0045706UL
#define WDT_IOCTL_SETPRETIMEOUT 0xC0045708UL
#define WDT_OPT_DISABLECARD     1

static int g_wd_fd = -1;

int bcm_wdt_open(int *out_fd) {
    if (!out_fd) return -1;
    int fd = open(WDT_DEV_PATH, O_WRONLY);
    if (fd < 0) return -errno;
    *out_fd = fd;
    return 0;
}

int bcm_wdt_start(int fd, int *timeout_sec, int pretimeout_sec) {
    if (fd < 0 || !timeout_sec) return -EINVAL;
    int pre = pretimeout_sec;
    (void)ioctl(fd, WDT_IOCTL_SETPRETIMEOUT, &pre);
    if (ioctl(fd, WDT_IOCTL_SETTIMEOUT, timeout_sec) < 0)
        return -errno;
    return 0;
}

int bcm_wdt_ping(int fd) {
    if (fd < 0) return -EINVAL;
    static const char nul = 0;
    if (write(fd, &nul, 1) < 0) return -errno;
    return 0;
}

int bcm_wdt_disable(int fd) {
    if (fd < 0) return -EINVAL;
    int opt = WDT_OPT_DISABLECARD;
    if (ioctl(fd, WDT_IOCTL_SETOPTIONS, &opt) < 0) return -errno;
    return 0;
}

int bcm_wdt_timeleft(int *out_sec) {
    DIR *d = opendir("/sys/class/watchdog");
    if (!d) return -errno;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/watchdog/%s/timeleft", ent->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        int val = 0;
        if (fscanf(fp, "%d", &val) == 1) {
            if (out_sec) *out_sec = val;
            found = 1;
        }
        fclose(fp);
        if (found) break;
    }
    closedir(d);
    return found ? 0 : -ENOENT;
}

static void wdt_usage(const char *prog) {
    printf("Usage: %s [-d] [-t <timeout>] start \n", prog);
    printf("       %s [-d] [-p <pid of wdtd daemon>] stop \n", prog);
    printf("       %s ping \n", prog);
    printf("       %s timeleft \n", prog);
    puts("                                                                        ");
    puts("options:                                                                ");
    printf("       -t : Specify a timeout in [%d-%d] seconds for the watchdog timer \n",
           BCM_WDT_MIN_TIMEOUT, BCM_WDT_MAX_TIMEOUT);
    puts("       -d : if used with start, start the wdtd daemon which pings the watchdog");
    puts("            every timeout/4 seconds.                                    ");
    puts("          : if used with stop, automatically find the pid of the wdtd daemon");
    puts("            and stop it, which also stops the watchdog timer itself.");
    puts("       -p : stop the wdtd daemon which has the specified pid            ");
    puts("            This is the old way, using -d stop is easier.  ");
    puts("                                                                        ");
    puts("Note:                                                                   ");
    puts("       The watchdog timer can be controlled using 2 mechanisms:         ");
    puts("       (1) Direct control: In this method the watchdog is  ");
    puts("           directly controlled by wdtctl                   ");
    puts("                                                                        ");
    printf("              %s -t 10 start                                            \n", prog);
    printf("              %s ping                                                   \n", prog);
    printf("              %s ping                                                   \n", prog);
    puts("              ...                                                       ");
    printf("              %s timeleft                                               \n", prog);
    printf("              %s stop                                                   \n", prog);
    puts("                                                                        ");
    puts("       (2) Daemonized: In this method the watchdog is pinged by the wdtd");
    puts("           daemon which runs every timeout/4 seconds                    ");
    printf("              %s -d -t 10 start                                         \n", prog);
    puts("              ...                                                       ");
    printf("              %s -d stop  (or... below)        \n", prog);
    printf("              %s -p <pid of wdtd daemon> stop  (this is the old way, using -d stop is easier) \n", prog);
}

static int wdt_open_cli(void) {
    if (g_wd_fd != -1) {
        printf("wdtctl: (internal error) wd_fd is already opened at %d\n", g_wd_fd);
        return g_wd_fd;
    }
    int fd = open(WDT_DEV_PATH, O_WRONLY);
    if (fd < 0) {
        if (errno == EBUSY) {
            printf("Could not open %s (EBUSY).  Is there a wdtd running?\n", WDT_DEV_PATH);
        } else {
            perror("watchdog");
        }
        return -1;
    }
    g_wd_fd = fd;
    return fd;
}

static void wdt_write_pidfile(void) {
    char buf[32];
    unlink(WDT_PID_FILE);
    int fd = open(WDT_PID_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        fprintf(stderr, "%s: could not open pid file!\n", WDT_DAEMON_STR);
        return;
    }
    int n = snprintf(buf, sizeof(buf), "%d", (int)getpid());
    if (n > 0) (void)write(fd, buf, (size_t)n);
    close(fd);
}

static int wdt_read_pidfile(void) {
    char buf[32];
    memset(buf, 0, sizeof(buf));
    int fd = open(WDT_PID_FILE, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    return atoi(buf);
}

static void wdt_sighup_handler(int signo, siginfo_t *si, void *ctx) {
    (void)ctx;
    unsigned pid = si ? (unsigned)si->si_pid : 0;
    if (signo == SIGHUP)
        fprintf(stderr, "%s: Recieved SIGHUP from pid %u\n", WDT_DAEMON_STR, pid);
    else
        fprintf(stderr, "%s: Recieved signal %d from pid %u\n", WDT_DAEMON_STR, signo, pid);
    (void)bcm_wdt_disable(g_wd_fd);
    unlink(WDT_PID_FILE);
    fprintf(stderr, "%s: stopped watchdog timer and exit.\n", WDT_DAEMON_STR);
    _exit(0);
}

static int wdt_daemonize(char *argv0, int timeout) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "%s: failed to fork/daemonize!\n", WDT_DAEMON_STR);
        return 1;
    }
    if (pid > 0) return 0; /* parent exits cleanly */

    fprintf(stderr, "%s started at pid %d (timeout=%d seconds)\n",
            WDT_DAEMON_STR, (int)getpid(), timeout);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = wdt_sighup_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);

    if (setsid() < 0) _exit(1);

    /* Match stock process title ("wdtd") safely within argv0 buffer and kernel comm */
#ifdef __linux__
    (void)prctl(PR_SET_NAME, WDT_DAEMON_STR, 0, 0, 0);
#endif
    size_t a0len = strlen(argv0);
    if (a0len >= strlen(WDT_DAEMON_STR)) {
        memset(argv0, 0, a0len);
        memcpy(argv0, WDT_DAEMON_STR, strlen(WDT_DAEMON_STR));
    }

    if (chdir("/") < 0) _exit(1);

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, 0);
        dup2(devnull, 1);
        if (devnull > 2) close(devnull);
    }

    wdt_write_pidfile();
    umask(0);

    unsigned interval = (unsigned)timeout >> 2;
    if (interval == 0) interval = 1;
    for (;;) {
        (void)bcm_wdt_ping(g_wd_fd);
        sleep(interval);
    }
}

int bcm_cli_wdtctl(int argc, char **argv) {
    g_wd_fd = -1;
    int timeout = BCM_WDT_DEF_TIMEOUT;
    int pretimeout = BCM_WDT_DEF_PRETIMEOUT;

    if (argc <= 1) {
        wdt_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[argc - 1];
    if (strcmp(cmd, "ping") == 0) {
        if (argc != 2) {
            wdt_usage(argv[0]);
            return 1;
        }
        if (wdt_open_cli() < 0) return 1;
        (void)bcm_wdt_ping(g_wd_fd);
        return 0;
    }
    if (strcmp(cmd, "timeleft") == 0) {
        if (argc != 2) {
            wdt_usage(argv[0]);
            return 1;
        }
        DIR *d = opendir("/sys/class/watchdog");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char path[256];
                snprintf(path, sizeof(path), "/sys/class/watchdog/%s/timeleft", ent->d_name);
                FILE *fp = fopen(path, "r");
                if (!fp) continue;
                char buf[64];
                size_t n = fread(buf, 1, sizeof(buf), fp);
                if (n > 0) (void)fwrite(buf, 1, n, stdout);
                fclose(fp);
            }
            closedir(d);
        }
        return 0;
    }

    int daemonize = 0;
    int target_pid = -1;
    int seen_opt = 0;
    int has_t = 0;
    int has_p = 0;

    if (argc > 2) {
        optind = 1;
        int opt;
        /* Parse options only before final command argument */
        while ((opt = getopt(argc - 1, argv, "dp:t:")) != -1) {
            switch (opt) {
            case 'd':
                daemonize = 1;
                seen_opt = 1;
                break;
            case 'p':
                target_pid = atoi(optarg);
                if (target_pid <= 0) {
                    wdt_usage(argv[0]);
                    return 1;
                }
                has_p = 1;
                seen_opt = 1;
                break;
            case 't':
                timeout = atoi(optarg);
                has_t = 1;
                if (timeout < BCM_WDT_MIN_TIMEOUT || timeout > BCM_WDT_MAX_TIMEOUT) {
                    printf("Invalid Timeout value %d!\n", timeout);
                    wdt_usage(argv[0]);
                    return 1;
                }
                seen_opt = 1;
                break;
            default:
                wdt_usage(argv[0]);
                return 1;
            }
        }
        if (!seen_opt) {
            wdt_usage(argv[0]);
            return 1;
        }
    }

    if (strcmp(cmd, "start") == 0) {
        if (has_p) {
            wdt_usage(argv[0]);
            return 1;
        }
        if (wdt_open_cli() < 0) return 1;
        if (bcm_wdt_start(g_wd_fd, &timeout, pretimeout) < 0) {
            perror("watchdog");
            return 1;
        }
        if (!daemonize) {
            printf("Started watchdog timer (%d seconds)\n", timeout);
            return 0;
        }
        for (int i = 1; i < argc; i++) {
            memset(argv[i], 0, strlen(argv[i]));
        }
        return wdt_daemonize(argv[0], timeout);
    }

    if (strcmp(cmd, "stop") == 0) {
        if (has_t) {
            wdt_usage(argv[0]);
            return 1;
        }
        if (target_pid > 0) {
            return kill((pid_t)target_pid, SIGHUP) == 0 ? 0 : 1;
        }
        if (daemonize) {
            int pid = wdt_read_pidfile();
            if (pid <= 0) {
                puts("Could not find pid to kill");
                return 1;
            }
            return kill((pid_t)pid, SIGHUP) == 0 ? 0 : 1;
        }
        if (wdt_open_cli() < 0) return 1;
        if (bcm_wdt_disable(g_wd_fd) < 0) {
            perror("watchdog");
            return 1;
        }
        puts("Stopped watchdog timer");
        return 0;
    }

    wdt_usage(argv[0]);
    return 1;
}
