/*
 * main.c — Multi-call dispatcher for bcmctl
 *
 * Dispatches by basename(argv[0]) (or explicit `bcmctl <applet> ...`):
 *   wdtctl | fcctl | nvram | bcm_bootstate | bcm_flashutil |
 *   ethswctl | bdmf_shell | bs | wl | wlctl
 */
#include "bcmopen.h"

#include <stdio.h>
#include <string.h>

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? (s + 1) : p;
}

static int dispatch(const char *app, int argc, char **argv, int *found) {
    *found = 1;
    if (strcmp(app, "wdtctl") == 0)        return bcm_cli_wdtctl(argc, argv);
    if (strcmp(app, "fcctl") == 0)         return bcm_cli_fcctl(argc, argv);
    if (strcmp(app, "nvram") == 0)         return bcm_cli_nvram(argc, argv);
    if (strcmp(app, "bcm_bootstate") == 0) return bcm_cli_bootstate(argc, argv);
    if (strcmp(app, "bcm_flashutil") == 0) return bcm_cli_flashutil(argc, argv);
    if (strcmp(app, "ethswctl") == 0)      return bcm_cli_ethswctl(argc, argv);
    if (strcmp(app, "bdmf_shell") == 0)    return bcm_cli_bdmf_shell(argc, argv);
    if (strcmp(app, "bs") == 0)            return bcm_cli_bs(argc, argv);
    if (strcmp(app, "wl") == 0 ||
        strcmp(app, "wlctl") == 0)         return bcm_cli_wl(argc, argv);
    *found = 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc <= 0 || !argv || !argv[0]) {
        fprintf(stderr, "bcmctl: invalid invocation\n");
        return 1;
    }

    int found = 0;
    const char *prog = base_name(argv[0]);
    int rc = dispatch(prog, argc, argv, &found);
    if (found) return rc;

    if (argc >= 2 && argv[1]) {
        rc = dispatch(argv[1], argc - 1, argv + 1, &found);
        if (found) return rc;
    }

    fprintf(stderr, "bcmctl: multi-call open-source Broadcom userspace tool\n");
    fprintf(stderr, "applets: wdtctl fcctl nvram bcm_bootstate bcm_flashutil ethswctl bdmf_shell bs wl wlctl\n");
    return 1;
}
