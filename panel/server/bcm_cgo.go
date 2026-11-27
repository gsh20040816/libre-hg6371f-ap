//go:build linux && cgo

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../../bcmopen/include
#cgo LDFLAGS: -lbcmopen -lm

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bcmopen.h"

#define GO_WLC_UP                   2
#define GO_WLC_DOWN                 3
#define GO_WLC_GET_RSSI             127
#define GO_WLC_GET_PHY_NOISE        135
#define GO_WLC_GET_ASSOCLIST        159
#define GO_WLC_GET_UP               162
#define GO_WLC_GET_FRAMEBURST       218
#define GO_WLC_SET_FRAMEBURST       219
#define GO_WLC_GET_PWRPERCENT       220
#define GO_WLC_SET_PWRPERCENT       221
#define GO_WLC_SET_SPECT_MANAGMENT  244
#define GO_WLC_GET_SPECT_MANAGMENT  245
#define GO_WL_IOCTL_LARGE_LEN       16384
#define GO_WL_DUMP_BUF_LEN          261120

static int bcm_go_wl_get_i32(const char *iface, int cmd, int32_t *out) {
    int32_t v = 0;
    int rc = bcm_wl_ioctl(iface, cmd, &v, sizeof(v), 0);
    if (rc == 0) *out = v;
    return rc;
}

static int bcm_go_wl_set_i32(const char *iface, int cmd, int32_t val) {
    return bcm_wl_ioctl(iface, cmd, &val, sizeof(val), 1);
}

static int bcm_go_wl_void(const char *iface, int cmd) {
    return bcm_wl_ioctl(iface, cmd, NULL, 0, 1);
}

static int bcm_go_wl_rssi_mac(const char *iface, const char *mac_str, int32_t *out) {
    uint8_t scb[12];
    memset(scb, 0, sizeof(scb));
    if (bcm_wl_parse_mac(mac_str, scb + 4) < 0) return -EINVAL;
    int rc = bcm_wl_ioctl(iface, GO_WLC_GET_RSSI, scb, sizeof(scb), 0);
    if (rc == 0) {
        int32_t v = 0;
        memcpy(&v, scb, 4);
        *out = v;
    }
    return rc;
}

static int bcm_go_wl_rssi_self(const char *iface, int32_t *out) {
    int32_t v = 0;
    int rc = bcm_wl_ioctl(iface, GO_WLC_GET_RSSI, &v, sizeof(v), 0);
    if (rc == 0) *out = (int32_t)(int8_t)(v & 0xff);
    return rc;
}

static int bcm_go_wl_assoclist(const char *iface, char **out_text) {
    *out_text = NULL;
    uint8_t *buf = (uint8_t *)calloc(1, GO_WL_IOCTL_LARGE_LEN);
    if (!buf) return -ENOMEM;

    uint32_t max_cnt = 0xaaa;
    memcpy(buf, &max_cnt, 4);

    int rc = bcm_wl_ioctl(iface, GO_WLC_GET_ASSOCLIST, buf, GO_WL_IOCTL_LARGE_LEN, 0);
    if (rc < 0) {
        free(buf);
        return rc;
    }

    uint32_t count = 0;
    memcpy(&count, buf, 4);
    if (count > 0xaaa) count = 0xaaa;

    size_t cap = (size_t)count * 28u + 1u;
    char *txt = (char *)malloc(cap);
    if (!txt) {
        free(buf);
        return -ENOMEM;
    }
    size_t pos = 0;
    for (uint32_t i = 0; i < count; i++) {
        char mac_s[18];
        bcm_wl_format_mac(buf + 4u + (size_t)i * 6u, mac_s, 0);
        int n = snprintf(txt + pos, cap - pos, "assoclist %s\n", mac_s);
        if (n > 0 && (size_t)n < cap - pos) pos += (size_t)n;
    }
    txt[pos] = '\0';
    free(buf);
    *out_text = txt;
    return 0;
}

static int bcm_go_wl_bw_cap(const char *iface, uint32_t band, uint32_t cap, int is_set, uint32_t *out_cap) {
    uint32_t req[2] = { band, cap };
    if (is_set && cap != 0) {
        return bcm_wl_iovar_set(iface, "bw_cap", req, sizeof(req));
    }
    uint32_t res = 0;
    int rc = bcm_wl_iovar_getbuf(iface, "bw_cap", req, sizeof(req), &res, sizeof(res));
    if (rc == 0) *out_cap = res;
    return rc;
}

static int bcm_go_wl_chanspec_get(const char *iface, uint16_t *out_cs, char *out_str, size_t out_sz) {
    int32_t val = 0;
    int rc = bcm_wl_iovar_getint(iface, "chanspec", &val);
    if (rc < 0) return rc;
    uint16_t cs = (uint16_t)(val & 0xffffu);
    *out_cs = cs;
    return bcm_wl_chspec_to_str(cs, out_str, out_sz);
}

static int bcm_go_wl_chanspec_set(const char *iface, const char *spec, uint16_t *out_cs) {
    uint16_t cs = 0;
    int rc = bcm_wl_chspec_from_str(spec, &cs);
    if (rc < 0) return rc;
    rc = bcm_wl_iovar_setint(iface, "chanspec", (int32_t)cs);
    if (rc == 0) *out_cs = cs;
    return rc;
}

static int bcm_go_wl_dump(const char *iface, const char *param, char **out_text) {
    *out_text = NULL;
    char *buf = (char *)calloc(1, GO_WL_DUMP_BUF_LEN);
    if (!buf) return -ENOMEM;

    int rc;
    if (param && param[0]) {
        rc = bcm_wl_iovar_getbuf(iface, "dump", param, strlen(param) + 1u, buf, GO_WL_DUMP_BUF_LEN - 1u);
    } else {
        rc = bcm_wl_iovar_get(iface, "dump", buf, GO_WL_DUMP_BUF_LEN - 1u);
    }
    if (rc < 0) {
        free(buf);
        return rc;
    }
    buf[GO_WL_DUMP_BUF_LEN - 1u] = '\0';
    *out_text = buf;
    return 0;
}
*/
import "C"

import (
	"fmt"
	"strings"
	"unsafe"
)

// wlVarIntSet lists verified int32 IOVAR commands handled directly in-process.
var wlVarIntSet = map[string]bool{
	"wme_apsd":       true,
	"assoc_rssi_th":  true,
	"atf":            true,
	"ampdu_rts":      true,
	"phy_rxdesens":   true,
	"txbf":           true,
	"mu_features":    true,
	"wmf_bss_enable": true,
	"phy_tempsense":  true,
}

// bcmTryExec intercepts high-frequency Broadcom CLI calls and executes them
// directly in-process via statically linked libbcmopen.a (zero fork/exec).
// Returns handled=false for any command that should fall back to /usr/local/sbin/<tool>.
func bcmTryExec(name string, args []string) (string, bool) {
	switch name {
	case "fcctl":
		if len(args) == 1 && args[0] == "status" {
			return bcmExecFCStatus(), true
		}
		return "", false

	case "bcm_bootstate":
		if len(args) == 0 {
			return bcmExecBootstateRead(), true
		}
		if len(args) == 1 {
			if code, ok := parseBootstateCode(args[0]); ok {
				_ = C.bcm_bootstate_set_code(C.int(code))
				return "", true
			}
		}
		return "", false

	case "wl", "wlctl":
		if len(args) < 3 || args[0] != "-i" || args[1] == "" {
			return "", false
		}
		return bcmExecWl(args[1], args[2], args[3:])

	default:
		return "", false
	}
}

func bcmExecFCStatus() string {
	var st C.bcm_fc_status_t
	if C.bcm_fc_status(&st) != 0 {
		return ""
	}
	hw := (st.flags1 & C.BCM_FC_F1_HW_ACCEL) != 0
	fl := (st.flags0 & C.BCM_FC_F0_FLOW_LEARN) != 0
	return formatFCStatus(hw, fl, uint32(st.ucast_max), uint32(st.ucast_created), uint32(st.ucast_evicted))
}

func bcmExecBootstateRead() string {
	var st C.bcm_bootstate_t
	if C.bcm_bootstate_read(&st) != 0 {
		return ""
	}
	valid := [2]int{int(st.meta.valid[0]), int(st.meta.valid[1])}
	seq := [2]int{int(st.meta.seq[0]), int(st.meta.seq[1])}
	tags := [2]string{
		C.GoString(&st.img_tag[0][0]),
		C.GoString(&st.img_tag[1][0]),
	}
	return formatBootstateText(
		int(st.booted_part),
		int(st.reboot_part),
		int(st.state_char),
		int(st.meta.committed),
		valid, seq, tags,
	)
}

func bcmExecWl(iface, sub string, rest []string) (string, bool) {
	cIf := C.CString(iface)
	defer C.free(unsafe.Pointer(cIf))

	switch sub {
	case "isup":
		if len(rest) != 0 {
			return "", false
		}
		var v C.int32_t
		if C.bcm_go_wl_get_i32(cIf, C.GO_WLC_GET_UP, &v) != 0 {
			return "", true
		}
		return fmt.Sprintf("%d\n", int32(v)), true

	case "up":
		if len(rest) != 0 {
			return "", false
		}
		_ = C.bcm_go_wl_void(cIf, C.GO_WLC_UP)
		return "", true

	case "down":
		if len(rest) != 0 {
			return "", false
		}
		_ = C.bcm_go_wl_void(cIf, C.GO_WLC_DOWN)
		return "", true

	case "noise":
		if len(rest) != 0 {
			return "", false
		}
		var v C.int32_t
		if C.bcm_go_wl_get_i32(cIf, C.GO_WLC_GET_PHY_NOISE, &v) != 0 {
			return "", true
		}
		return fmt.Sprintf("%d\n", int32(v)), true

	case "rssi":
		if len(rest) == 0 {
			var v C.int32_t
			if C.bcm_go_wl_rssi_self(cIf, &v) != 0 {
				return "", true
			}
			return fmt.Sprintf("%d\n", int32(v)), true
		}
		if len(rest) == 1 {
			cMac := C.CString(rest[0])
			defer C.free(unsafe.Pointer(cMac))
			var v C.int32_t
			if C.bcm_go_wl_rssi_mac(cIf, cMac, &v) != 0 {
				return "", true
			}
			return fmt.Sprintf("%d\n", int32(v)), true
		}
		return "", false

	case "assoclist":
		if len(rest) != 0 {
			return "", false
		}
		var txt *C.char
		if C.bcm_go_wl_assoclist(cIf, &txt) != 0 || txt == nil {
			return "", true
		}
		defer C.free(unsafe.Pointer(txt))
		return C.GoString(txt), true

	case "chanspec":
		if len(rest) == 0 {
			var cs C.uint16_t
			var buf [32]C.char
			if C.bcm_go_wl_chanspec_get(cIf, &cs, &buf[0], C.size_t(len(buf))) != 0 {
				return "", true
			}
			return fmt.Sprintf("%s (0x%x)\n", C.GoString(&buf[0]), uint16(cs)), true
		}
		if len(rest) == 1 && !strings.HasPrefix(rest[0], "-") {
			cSpec := C.CString(rest[0])
			defer C.free(unsafe.Pointer(cSpec))
			var cs C.uint16_t
			if C.bcm_go_wl_chanspec_set(cIf, cSpec, &cs) != 0 {
				return "", true
			}
			return fmt.Sprintf("Chanspec set to 0x%x\n", uint16(cs)), true
		}
		return "", false

	case "dump":
		param := ""
		if len(rest) > 0 {
			// Match stock wl_dump: space-separated args with trailing space.
			param = strings.Join(rest, " ") + " "
			if len(param) >= 510 {
				return "", false
			}
		}
		var cParam *C.char
		if param != "" {
			cParam = C.CString(param)
			defer C.free(unsafe.Pointer(cParam))
		}
		var txt *C.char
		if C.bcm_go_wl_dump(cIf, cParam, &txt) != 0 || txt == nil {
			return "", true
		}
		defer C.free(unsafe.Pointer(txt))
		out := C.GoString(txt)
		if len(out) > 0 && out[len(out)-1] != '\n' {
			out += "\n"
		}
		return out, true

	case "spect":
		if len(rest) == 0 {
			var v C.int32_t
			if C.bcm_go_wl_get_i32(cIf, C.GO_WLC_GET_SPECT_MANAGMENT, &v) != 0 {
				return "", true
			}
			if s, ok := formatWlSpect(int32(v)); ok {
				return s, true
			}
			return "", true
		}
		if len(rest) == 1 {
			v, ok := parseWlIntArg(rest[0])
			if !ok || v < 0 || v > 4 {
				return "", false
			}
			_ = C.bcm_go_wl_set_i32(cIf, C.GO_WLC_SET_SPECT_MANAGMENT, C.int32_t(v))
			return "", true
		}
		return "", false

	case "bw_cap":
		if len(rest) < 1 || len(rest) > 2 {
			return "", false
		}
		band, ok := parseWlBand(rest[0])
		if !ok {
			return "", false
		}
		var capVal uint32
		isSet := 0
		if len(rest) == 2 {
			v, ok := parseWlUintArg(rest[1])
			if !ok {
				return "", false
			}
			capVal = v
			if capVal != 0 {
				isSet = 1
			}
		}
		var outCap C.uint32_t
		if C.bcm_go_wl_bw_cap(cIf, C.uint32_t(band), C.uint32_t(capVal), C.int(isSet), &outCap) != 0 {
			return "", true
		}
		if isSet != 0 {
			return "", true
		}
		return fmt.Sprintf("0x%x\n", uint32(outCap)), true

	case "frameburst":
		return bcmExecWlIntIoctl(cIf, C.GO_WLC_GET_FRAMEBURST, C.GO_WLC_SET_FRAMEBURST, rest)

	case "txpwr_percent":
		return bcmExecWlIntIoctl(cIf, C.GO_WLC_GET_PWRPERCENT, C.GO_WLC_SET_PWRPERCENT, rest)

	default:
		if !wlVarIntSet[sub] || len(rest) > 1 {
			return "", false
		}
		cName := C.CString(sub)
		defer C.free(unsafe.Pointer(cName))
		if len(rest) == 0 {
			var v C.int32_t
			if C.bcm_wl_iovar_getint(cIf, cName, &v) != 0 {
				return "", true
			}
			return formatWlVarInt(int32(v)), true
		}
		v, ok := parseWlIntArg(rest[0])
		if !ok {
			return "", false
		}
		_ = C.bcm_wl_iovar_setint(cIf, cName, C.int32_t(v))
		return "", true
	}
}

func bcmExecWlIntIoctl(cIf *C.char, getCmd, setCmd C.int, rest []string) (string, bool) {
	if len(rest) == 0 {
		var v C.int32_t
		if C.bcm_go_wl_get_i32(cIf, getCmd, &v) != 0 {
			return "", true
		}
		return fmt.Sprintf("%d\n", int32(v)), true
	}
	if len(rest) == 1 {
		v, ok := parseWlIntArg(rest[0])
		if !ok {
			return "", false
		}
		_ = C.bcm_go_wl_set_i32(cIf, setCmd, C.int32_t(v))
		return "", true
	}
	return "", false
}
