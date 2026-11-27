/*
 * flashutil.c — Open-source replacement for Broadcom /bin/bcm_flashutil
 *
 * Verified verbatim against /opt/bcm/libexec/bcm_flashutil (227 lines of ARM32 ASM):
 *   - argc <= 3: prints exact 3-line usage and returns -1 (255)
 *   - argc >= 4: calls bcm_flash_get_ident(atoi(argv[1]), &start, &end, argv[4], buf, 200)
 *     rc < 0  -> return -1 (255)
 *     rc == 0 -> puts("key not found"), return 0
 *     rc > 0  -> printf("\nkey size %d/%d found at block 0x%x:\n%.*s\n", ...)
 */
#include "bcmopen.h"

#include <stdio.h>
#include <stdlib.h>

int bcm_cli_flashutil(int argc, char **argv) {
    if (argc <= 3) {
        printf("usage: %s partition (1 = first : 2 = second : 3 = boot : 4 = non-boot,\n", argv[0]);
        puts("       start block (-1 = beginning), end block (-1 = end of partition),");
        puts("       key (optional, leave blank to see all ident values in image");
        return 255;
    }
    int part = atoi(argv[1]);
    int start_blk = atoi(argv[2]);
    int end_blk = atoi(argv[3]);
    const char *key = (argc > 4) ? argv[4] : NULL;
    char buf[201];
    int n = bcm_flash_get_ident(part, &start_blk, &end_blk, key, buf, 200);
    if (n < 0) return 255;
    if (n == 0) {
        puts("key not found");
        return 0;
    }
    printf("\nkey size %d/%d found at block 0x%x:\n%.*s\n", n, 200, start_blk, n, buf);
    return 0;
}
