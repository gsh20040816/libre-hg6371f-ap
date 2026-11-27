/*
 * test_m1.c — Host-side unit & ABI layout tests for M1 (7 small tools)
 */
#include "bcmopen.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        exit(1); \
    } \
    g_pass++; \
} while (0)

static void test_struct_layouts(void) {
    /* Verify exact binary offsets against ARM32 disassembly */
    CHECK(sizeof(bcm_fc_status_t) == 0x40, "sizeof(bcm_fc_status_t) == 0x40");
    CHECK(offsetof(bcm_fc_status_t, flow_refresh_timer_ms) == 0x00, "fc refresh_timer @0x00");
    CHECK(offsetof(bcm_fc_status_t, hw_activate_defer_rate) == 0x04, "fc hw_defer @0x04");
    CHECK(offsetof(bcm_fc_status_t, low_pkt_rate) == 0x06, "fc low_pkt_rate @0x06");
    CHECK(offsetof(bcm_fc_status_t, ucast_max) == 0x08, "fc ucast_max @0x08");
    CHECK(offsetof(bcm_fc_status_t, ucast_created) == 0x0c, "fc ucast_created @0x0c");
    CHECK(offsetof(bcm_fc_status_t, ucast_evicted) == 0x10, "fc ucast_evicted @0x10");
    CHECK(offsetof(bcm_fc_status_t, sw_activate_defer_count) == 0x14, "fc sw_defer @0x14");
    CHECK(offsetof(bcm_fc_status_t, mcast_max) == 0x18, "fc mcast_max @0x18");
    CHECK(offsetof(bcm_fc_status_t, uucast_max) == 0x24, "fc uucast_max @0x24");
    CHECK(offsetof(bcm_fc_status_t, tcp_idle_s) == 0x30, "fc tcp_idle_s @0x30");
    CHECK(offsetof(bcm_fc_status_t, udp_idle_s) == 0x32, "fc udp_idle_s @0x32");
    CHECK(offsetof(bcm_fc_status_t, l2_idle_s) == 0x34, "fc l2_idle_s @0x34");
    CHECK(offsetof(bcm_fc_status_t, mcast_idle_s) == 0x36, "fc mcast_idle_s @0x36");
    CHECK(offsetof(bcm_fc_status_t, other_idle_s) == 0x38, "fc other_idle_s @0x38");
    CHECK(offsetof(bcm_fc_status_t, uucast_idle_s) == 0x3a, "fc uucast_idle_s @0x3a");
    CHECK(offsetof(bcm_fc_status_t, flags0) == 0x3c, "fc flags0 @0x3c");
    CHECK(offsetof(bcm_fc_status_t, flags1) == 0x3d, "fc flags1 @0x3d");

    CHECK(sizeof(bcm_fc_flush_req_t) == 0x14, "sizeof(bcm_fc_flush_req_t) == 0x14");
    CHECK(offsetof(bcm_fc_flush_req_t, mac) == 0x04, "flush mac @0x04");
    CHECK(offsetof(bcm_fc_flush_req_t, ifindex) == 0x0c, "flush ifindex @0x0c");
    CHECK(offsetof(bcm_fc_flush_req_t, flowid) == 0x10, "flush flowid @0x10");

    CHECK(sizeof(bcm_fc_tuple_req_t) == 0x90, "sizeof(bcm_fc_tuple_req_t) == 0x90");
    CHECK(offsetof(bcm_fc_tuple_req_t, rxdstv4) == 0x1c, "tuple rxdstv4 @0x1c");
    CHECK(offsetof(bcm_fc_tuple_req_t, srcv4) == 0x28, "tuple srcv4 @0x28");
    CHECK(offsetof(bcm_fc_tuple_req_t, dstv6) == 0x40, "tuple dstv6 @0x40");
    CHECK(offsetof(bcm_fc_tuple_req_t, srcport) == 0x50, "tuple srcport @0x50");
    CHECK(offsetof(bcm_fc_tuple_req_t, srcmac) == 0x5a, "tuple srcmac @0x5a");
    CHECK(offsetof(bcm_fc_tuple_req_t, flags) == 0x88, "tuple flags @0x88");
    CHECK(offsetof(bcm_fc_tuple_req_t, query_id) == 0x8c, "tuple query_id @0x8c");
}

static void test_crc_and_real_metadata(void) {
    /* Standard IEEE CRC32 check vector: "123456789" -> 0xCBF43926 */
    CHECK(bcm_crc32("123456789", 9) == 0xcbf43926u, "IEEE CRC32 standard test vector");

    /* Real device backup metadata1.bin */
    FILE *fp = fopen("../backups/metadata1.bin", "rb");
    CHECK(fp != NULL, "open ../backups/metadata1.bin");
    uint8_t raw[BCM_META_VOL_BYTES];
    size_t n = fread(raw, 1, sizeof(raw), fp);
    fclose(fp);
    CHECK(n == BCM_META_VOL_BYTES, "metadata1.bin == 1280 bytes");

    uint32_t calc = bcm_crc32(raw + BCM_META_HDR_BYTES, BCM_META_CRC_LEN);
    CHECK(calc == 0x5dcd9e8du, "real metadata1.bin CRC32 == 0x5dcd9e8d");

    bcm_boot_meta_t m;
    CHECK(bcm_meta_parse(raw, n, &m) == 0, "parse real metadata1.bin");
    CHECK(m.committed == 2, "real metadata1 committed == 2");
    CHECK(m.valid[0] == 1 && m.valid[1] == 2, "real metadata1 valid == {1,2}");
    CHECK(m.seq[0] == 1 && m.seq[1] == 2, "real metadata1 seq == {1,2}");

    /* Corrupt 1 bit -> must reject */
    raw[0x15] ^= 0x01;
    CHECK(bcm_meta_parse(raw, n, &m) != 0, "reject corrupted CRC");

    /* Pack & roundtrip */
    bcm_boot_meta_t src = { .committed = 1, .valid = {1, 2}, .seq = {7, 8} };
    uint8_t packed[BCM_META_VOL_BYTES];
    CHECK(bcm_meta_pack(&src, packed, sizeof(packed)) == 0, "pack metadata");
    bcm_boot_meta_t dst;
    CHECK(bcm_meta_parse(packed, sizeof(packed), &dst) == 0, "roundtrip parse packed metadata");
    CHECK(dst.committed == 1 && dst.valid[0] == 1 && dst.valid[1] == 2 &&
          dst.seq[0] == 7 && dst.seq[1] == 8, "roundtrip values exact match");

    /* H11: VALID=%d,%d MUST always include both fields (1,0 / 0,2 / 0,0) */
    bcm_boot_meta_t v10 = { .committed = 1, .valid = {1, 0}, .seq = {999, 0} };
    CHECK(bcm_meta_pack(&v10, packed, sizeof(packed)) == 0, "pack VALID=1,0");
    CHECK(strstr((const char *)(packed + 0x0c + strlen("COMMITTED=1") + 1), "VALID=1,0") != NULL,
          "VALID=1,0 emitted verbatim");
    CHECK(bcm_meta_parse(packed, sizeof(packed), &dst) == 0 &&
          dst.valid[0] == 1 && dst.valid[1] == 0 && dst.seq[0] == 999 && dst.seq[1] == 0,
          "roundtrip VALID=1,0 + seq 999/0");

    bcm_boot_meta_t v02 = { .committed = 2, .valid = {0, 2}, .seq = {0, 999} };
    CHECK(bcm_meta_pack(&v02, packed, sizeof(packed)) == 0, "pack VALID=0,2");
    CHECK(strstr((const char *)(packed + 0x0c + strlen("COMMITTED=2") + 1), "VALID=0,2") != NULL,
          "VALID=0,2 emitted verbatim");
    CHECK(bcm_meta_parse(packed, sizeof(packed), &dst) == 0 &&
          dst.valid[0] == 0 && dst.valid[1] == 2, "roundtrip VALID=0,2");

    bcm_boot_meta_t v00 = { .committed = 0, .valid = {0, 0}, .seq = {-1, -1} };
    CHECK(bcm_meta_pack(&v00, packed, sizeof(packed)) == 0, "pack VALID=0,0");
    CHECK(strstr((const char *)(packed + 0x0c + strlen("COMMITTED=0") + 1), "VALID=0,0") != NULL,
          "VALID=0,0 emitted verbatim");

    /* MED-2: Reject out-of-range VALID=3,0 */
    uint8_t bad_valid[BCM_META_VOL_BYTES];
    memset(bad_valid, 0, sizeof(bad_valid));
    bad_valid[0] = 0x00; bad_valid[1] = 0x01;
    bad_valid[4] = 0x00; bad_valid[5] = 0x01;
    char *bp = (char *)(bad_valid + 0x0c);
    bp += sprintf(bp, "COMMITTED=1") + 1;
    bp += sprintf(bp, "VALID=3,0") + 1;
    bp += sprintf(bp, "SEQ=1,2") + 1;
    uint32_t bcrc = bcm_crc32(bad_valid + 0x0c, BCM_META_CRC_LEN);
    memcpy(bad_valid + 0x08, &bcrc, 4);
    CHECK(bcm_meta_parse(bad_valid, sizeof(bad_valid), &dst) != 0, "reject VALID=3,0");

    /* MED-2: Synthesize SEQ when SEQ= line omitted */
    uint8_t no_seq[BCM_META_VOL_BYTES];
    memset(no_seq, 0, sizeof(no_seq));
    no_seq[0] = 0x00; no_seq[1] = 0x01;
    no_seq[4] = 0x00; no_seq[5] = 0x01;
    char *np = (char *)(no_seq + 0x0c);
    np += sprintf(np, "COMMITTED=2") + 1;
    np += sprintf(np, "VALID=1,2") + 1;
    uint32_t ncrc = bcm_crc32(no_seq + 0x0c, BCM_META_CRC_LEN);
    memcpy(no_seq + 0x08, &ncrc, 4);
    CHECK(bcm_meta_parse(no_seq, sizeof(no_seq), &dst) == 0 &&
          dst.committed == 2 && dst.seq[0] == 0 && dst.seq[1] == 1,
          "synthesize SEQ when SEQ= absent");

    /* Insane size field -> reject */
    packed[0x04] = 0xff; packed[0x05] = 0x0f;
    CHECK(bcm_meta_parse(packed, sizeof(packed), &dst) != 0, "reject oversized metadata");
}

static void test_bdmf_long_cmd_safety(void) {
    /* C15: ensure huge -cmd args do not underflow or crash (fails cleanly opening /dev/bdmf_shell) */
    char big[2100];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char *argv[] = { "bdmf_shell", "-c", "7", "-cmd", big, "tail", NULL };
    int rc = bcm_cli_bdmf_shell(6, argv);
    CHECK(rc == 255, "bdmf_shell long -cmd safely truncated without crash");
}

static void test_wl_helpers(void) {
    /* Chanspec roundtrip & decode/encode tests (D11REV >= 40) */
    struct {
        const char *str;
        uint16_t    val;
    } cs_cases[] = {
        { "6",       0x1006u }, /* 2G 20MHz ch6 */
        { "1l",      0x1803u }, /* 2G 40MHz ch1 lower (center=3, sb=0) */
        { "11u",     0x1909u }, /* 2G 40MHz ch11 upper (center=9, sb=1) */
        { "36",      0xd024u }, /* 5G 20MHz ch36 */
        { "36l",     0xd826u }, /* 5G 40MHz ch36 lower (center=38, sb=0) */
        { "40u",     0xd926u }, /* 5G 40MHz ch40 upper (center=38, sb=1) */
        { "36/80",   0xe02au }, /* 5G 80MHz ch36 (center=42, sb=0) */
        { "149/80",  0xe09bu }, /* 5G 80MHz ch149 (center=155, sb=0) */
        { "48/160",  0xeb32u }, /* 5G 160MHz ch48 (center=50, sb=3) */
        { "36/160",  0xe832u }, /* 5G 160MHz ch36 (center=50, sb=0) */
    };
    for (size_t i = 0; i < sizeof(cs_cases) / sizeof(cs_cases[0]); i++) {
        uint16_t got = 0;
        CHECK(bcm_wl_chspec_from_str(cs_cases[i].str, &got) == 0, "chspec_from_str succeeds");
        CHECK(got == cs_cases[i].val, "chspec_from_str exact hex match");
        char out[32] = {0};
        CHECK(bcm_wl_chspec_to_str(cs_cases[i].val, out, sizeof(out)) == 0, "chspec_to_str succeeds");
        CHECK(strcmp(out, cs_cases[i].str) == 0, "chspec_to_str exact roundtrip");
    }

    /* Hex chanspec input & invalid chanspecs */
    uint16_t hx = 0;
    CHECK(bcm_wl_chspec_from_str("0xeb32", &hx) == 0 && hx == 0xeb32u, "chspec hex 0xeb32");
    CHECK(bcm_wl_chspec_from_str("", &hx) != 0, "reject empty chspec");
    CHECK(bcm_wl_chspec_from_str("notach", &hx) != 0, "reject garbage chspec");
    CHECK(bcm_wl_chspec_from_str("48/320", &hx) != 0, "reject unsupported 320MHz");
    CHECK(bcm_wl_chspec_from_str("14/40", &hx) != 0, "reject invalid 2G ch14 40MHz");
    CHECK(bcm_wl_chspec_from_str("2g36/40", &hx) != 0, "reject invalid 2g36/40");
    CHECK(bcm_wl_chspec_from_str("36u", &hx) != 0, "reject invalid 5G 36u");
    CHECK(bcm_wl_chspec_from_str("5g1u", &hx) != 0, "reject invalid 5g1u");

    /* MAC parse & format */
    uint8_t mac[6] = {0};
    CHECK(bcm_wl_parse_mac("98:ee:ca:15:57:48", mac) == 0, "parse lower MAC");
    CHECK(mac[0] == 0x98 && mac[1] == 0xee && mac[5] == 0x48, "parse MAC bytes");
    char mstr[18] = {0};
    bcm_wl_format_mac(mac, mstr, 0);
    CHECK(strcmp(mstr, "98:EE:CA:15:57:48") == 0, "format upper MAC");
    bcm_wl_format_mac(mac, mstr, 1);
    CHECK(strcmp(mstr, "98:ee:ca:15:57:48") == 0, "format lower MAC");
    CHECK(bcm_wl_parse_mac("98:ee:ca:15:57", mac) != 0, "reject short MAC");
    CHECK(bcm_wl_parse_mac("98:ee:ca:15:57:48:00", mac) != 0, "reject long MAC");
    CHECK(bcm_wl_parse_mac("zz:ee:ca:15:57:48", mac) != 0, "reject non-hex MAC");
    CHECK(bcm_wl_parse_mac("+1:+2:+3:+4:+5:+6", mac) != 0, "reject signed MAC octets");
}

int main(void) {
    test_struct_layouts();
    test_crc_and_real_metadata();
    test_bdmf_long_cmd_safety();
    test_wl_helpers();
    printf("ALL %d M1+M2 unit tests PASSED!\n", g_pass);
    return 0;
}
