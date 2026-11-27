/*
 * bdmf.c — Open-source replacement for Broadcom /bin/bdmf_shell + /opt/bcm/bin/bs
 *
 * Verified verbatim against /opt/bcm/libexec/bdmf_shell ARM32 disassembly:
 *   - Serial char device: /dev/bdmf_shell (O_RDWR)
 *     - ioctl 0x40045301: init session  (arg = int *session_id) -> printf("Session: %d\n", id)
 *     - ioctl 0x40045302: send command  (arg = struct { int id; int rc; char cmd[2048]; })
 *     - ioctl 0x40045303: close session (arg = int *session_id)
 *   - Also supports TCP (host:port) and UNIX domain socket sessions, -f script_file, -cmd
 */
#include "bcmopen.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define BDMF_SHELL_DEV    "/dev/bdmf_shell"
#define BDMF_IOCTL_INIT   0x40045301UL
#define BDMF_IOCTL_CMD    0x40045302UL
#define BDMF_IOCTL_CLOSE  0x40045303UL

typedef struct {
    int  session_id;
    int  rc;
    char cmd[2048];
} bdmf_cmd_req_t;

int bcm_bdmf_init_session(int *out_session_id) {
    if (!out_session_id) return -1;
    int fd = open(BDMF_SHELL_DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int sid = -1;
    int r = ioctl(fd, BDMF_IOCTL_INIT, &sid);
    close(fd);
    if (r < 0 || sid < 0) return -1;
    *out_session_id = sid;
    return 0;
}

int bcm_bdmf_send_cmd(int session_id, const char *cmd) {
    if (session_id < 0 || !cmd) return -1;
    int fd = open(BDMF_SHELL_DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    bdmf_cmd_req_t req;
    memset(&req, 0, sizeof(req));
    req.session_id = session_id;
    strncpy(req.cmd, cmd, sizeof(req.cmd) - 1);
    int r = ioctl(fd, BDMF_IOCTL_CMD, &req);
    close(fd);
    if (r < 0) return -1;
    return req.rc;
}

int bcm_bdmf_close_session(int session_id) {
    if (session_id < 0) return -1;
    int fd = open(BDMF_SHELL_DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = ioctl(fd, BDMF_IOCTL_CLOSE, &session_id);
    close(fd);
    return r < 0 ? -1 : 0;
}

static void bdmf_usage(void) {
    fputs("Usage:\n"
          "\tbdmf_shell server_addr:port [options]\n"
          "\tbdmf_shell domain_socket_addr [options]\n"
          "\tbdmf_shell [-c init | session_id] [-cmd command] [-f script_file] [-fg] [-close]\n"
          "\t\t-c : serial communication. Must be followed by \"init\" or session_id\n"
          "\t\t-cmd command : send BDMF shell command\n"
          "\t\t-f script_file : send script_file content\n"
          "\t\t-fg - interactive mode\n"
          "\t\t-close - close session opened by \"-c init\" - used with \"-c\"\n",
          stderr);
}

static void bdmf_drain_sock_nonblock(int sock_fd, int wait_ms) {
    char buf[1024];
    for (;;) {
        struct pollfd pfd = { .fd = sock_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, wait_ms);
        if (pr <= 0) break;
        ssize_t n = recv(sock_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        (void)fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
        wait_ms = 20;
    }
}

static void bdmf_drain_sock_eof(int sock_fd) {
    char buf[1024];
    for (;;) {
        struct pollfd pfd = { .fd = sock_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 2000);
        if (pr <= 0) break;
        ssize_t n = recv(sock_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        (void)fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
    }
}

static int bdmf_send_line(int dev_fd, int sock_fd, int sid, const char *line, int *last_rc) {
    if (dev_fd >= 0) {
        if (sid < 0) {
            fputs("send: Can't send command over serial. session_id is missing\n", stderr);
            return -1;
        }
        bdmf_cmd_req_t req;
        memset(&req, 0, sizeof(req));
        req.session_id = sid;
        strncpy(req.cmd, line, sizeof(req.cmd) - 1);
        int r = ioctl(dev_fd, BDMF_IOCTL_CMD, &req);
        if (r == 0 && last_rc) *last_rc = req.rc;
        return r;
    }
    size_t len = strlen(line);
    ssize_t w = send(sock_fd, line, len, 0);
    if (w != (ssize_t)len) {
        fputs("send: failed to send command\n", stderr);
        return -1;
    }
    bdmf_drain_sock_nonblock(sock_fd, 50);
    return 0;
}

int bcm_cli_bdmf_shell(int argc, char **argv) {
    if (argc <= 1) {
        bdmf_usage();
        return 255;
    }

    int is_serial = 0;
    int do_init = 0;
    int do_close = 0;
    int do_fg = 0;
    int sid = -1;
    const char *script_file = NULL;
    char cmd_buf[2048];
    cmd_buf[0] = '\0';
    int arg_i = 2;

    int sock_fd = -1;
    int dev_fd = -1;

    if (strcmp(argv[1], "-c") == 0) {
        if (argc == 2) {
            fputs("-c must be followed by \"init\" or numerical session_id\n", stderr);
            bdmf_usage();
            return 255;
        }
        is_serial = 1;
        arg_i = 3;
        if (strcmp(argv[2], "init") == 0) {
            do_init = 1;
        } else {
            char *endp = NULL;
            long v = strtol(argv[2], &endp, 0);
            if (argc <= 3 || v <= 0 || !endp || *endp != '\0') {
                fprintf(stderr, "Invalid session_id %s after -c or too few parameters\n", argv[2]);
                bdmf_usage();
                return 255;
            }
            sid = (int)v;
        }
    }

    for (int i = arg_i; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc) {
                fputs("Script file name expected after -f\n", stderr);
                return 255;
            }
            script_file = argv[++i];
        } else if (strcmp(argv[i], "-fg") == 0) {
            do_fg = 1;
        } else if (strcmp(argv[i], "-close") == 0) {
            if (!is_serial) {
                fputs("-close option can be used only for serial session\n", stderr);
                bdmf_usage();
                return 255;
            }
            do_close = 1;
        } else if (strcmp(argv[i], "-cmd") == 0) {
            if (i + 1 >= argc) {
                fputs("Command is expected after -cmd\n", stderr);
                bdmf_usage();
                return 255;
            }
            size_t used = 0;
            for (int j = i + 1; j < argc; j++) {
                size_t alen = strlen(argv[j]);
                if (used + 2 >= sizeof(cmd_buf)) break;
                size_t avail = sizeof(cmd_buf) - used - 2; /* reserve sep/nl + NUL */
                size_t copy = alen < avail ? alen : avail;
                memcpy(cmd_buf + used, argv[j], copy);
                used += copy;
                cmd_buf[used++] = (j + 1 < argc) ? ' ' : '\n';
            }
            cmd_buf[used] = '\0';
            break;
        }
    }

    if (is_serial) {
        dev_fd = open(BDMF_SHELL_DEV, O_RDWR | O_CLOEXEC);
        if (dev_fd < 0) {
            fprintf(stderr, "Can't open serial file %s\n", BDMF_SHELL_DEV);
            return 255;
        }
        if (do_init) {
            if (ioctl(dev_fd, BDMF_IOCTL_INIT, &sid) < 0) {
                perror("init session");
                close(dev_fd);
                return 255;
            }
            printf("Session: %d\n", sid);
        }
    } else {
        /* TCP host:port or UNIX domain socket */
        const char *colon = strchr(argv[1], ':');
        if (colon) {
            char host[256];
            size_t hlen = (size_t)(colon - argv[1]);
            if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
            memcpy(host, argv[1], hlen);
            host[hlen] = '\0';
            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || !res) {
                fprintf(stderr, "Unknown host: %s\n", host);
                return 253;
            }
            sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (sock_fd < 0) {
                perror("socket");
                freeaddrinfo(res);
                return 253;
            }
            if (connect(sock_fd, res->ai_addr, res->ai_addrlen) < 0) {
                perror("connect");
                freeaddrinfo(res);
                close(sock_fd);
                return 252;
            }
            freeaddrinfo(res);
        } else {
            struct sockaddr_un un;
            memset(&un, 0, sizeof(un));
            un.sun_family = AF_UNIX;
            strncpy(un.sun_path, argv[1], sizeof(un.sun_path) - 1);
            sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock_fd < 0) {
                perror("socket");
                return 253;
            }
            if (connect(sock_fd, (struct sockaddr *)&un, sizeof(un)) < 0) {
                perror("connect");
                close(sock_fd);
                return 252;
            }
        }
        /* Initial "\n" handshake matching bdmf_shell.asm:112c0..112e8 */
        if (send(sock_fd, "\n", 1, 0) <= 0) {
            fputs("failed to initiate conversation\n", stderr);
            close(sock_fd);
            return 255;
        }
        bdmf_drain_sock_nonblock(sock_fd, 50);
    }

    int rc = 0, cmd_rc = do_init ? sid : 0;
    if (cmd_buf[0] != '\0') {
        rc = bdmf_send_line(dev_fd, sock_fd, sid, cmd_buf, &cmd_rc);
    }
    if (rc == 0 && script_file) {
        FILE *fp = fopen(script_file, "r");
        if (!fp) {
            fprintf(stderr, "Can't open script file %s\n", script_file);
            rc = -1;
        } else {
            char line[2048];
            while (fgets(line, sizeof(line), fp)) {
                rc = bdmf_send_line(dev_fd, sock_fd, sid, line, &cmd_rc);
                if (rc != 0) break;
            }
            fclose(fp);
        }
    }
    if (rc == 0 && do_fg) {
        char line[2048];
        if (sock_fd >= 0) {
            for (;;) {
                struct pollfd pfds[2] = {
                    { .fd = STDIN_FILENO, .events = POLLIN },
                    { .fd = sock_fd,      .events = POLLIN }
                };
                int pr = poll(pfds, 2, -1);
                if (pr < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (pfds[1].revents & (POLLIN | POLLHUP)) {
                    char rbuf[1024];
                    ssize_t n = recv(sock_fd, rbuf, sizeof(rbuf), 0);
                    if (n <= 0) break;
                    (void)fwrite(rbuf, 1, (size_t)n, stdout);
                    fflush(stdout);
                }
                if (pfds[0].revents & (POLLIN | POLLHUP)) {
                    if (!fgets(line, sizeof(line), stdin)) break;
                    if (send(sock_fd, line, strlen(line), 0) <= 0) break;
                }
            }
        } else {
            while (fgets(line, sizeof(line), stdin)) {
                rc = bdmf_send_line(dev_fd, -1, sid, line, &cmd_rc);
                if (rc != 0) break;
            }
        }
    }
    if (sock_fd >= 0) {
        /* Send EOT '\x04' and drain remaining response matching bdmf_shell.asm:116f4 */
        const char eot = 0x04;
        (void)send(sock_fd, &eot, 1, 0);
        bdmf_drain_sock_eof(sock_fd);
    }
    if (do_close && is_serial) {
        if (sid < 0) {
            fputs("Can't close session. session_id is missing\n", stderr);
            rc = -1;
        } else if (ioctl(dev_fd, BDMF_IOCTL_CLOSE, &sid) < 0) {
            perror("close session");
        }
    }
    if (dev_fd >= 0) close(dev_fd);
    if (sock_fd >= 0) close(sock_fd);
    return (rc == 0) ? cmd_rc : 255;
}

int bcm_cli_bs(int argc, char **argv) {
    /* Equivalent to /opt/bcm/bin/bs: bdmf_shell -c "$(cat /var/bdmf_sh_id)" -cmd "$@" */
    char sid[32] = {0};
    FILE *fp = fopen("/var/bdmf_sh_id", "r");
    if (fp) {
        if (fgets(sid, sizeof(sid), fp)) {
            char *nl = strpbrk(sid, "\r\n");
            if (nl) *nl = '\0';
        }
        fclose(fp);
    }
    char **nargv = calloc((size_t)(argc + 4), sizeof(char *));
    if (!nargv) return 255;
    int k = 0;
    nargv[k++] = "bdmf_shell";
    nargv[k++] = "-c";
    nargv[k++] = sid;
    nargv[k++] = "-cmd";
    for (int i = 1; i < argc; i++) nargv[k++] = argv[i];
    int r = bcm_cli_bdmf_shell(k, nargv);
    free(nargv);
    return r;
}
