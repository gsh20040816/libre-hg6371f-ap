package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
)

var (
	bcmRFConfPath = "/etc/bcm-rf.conf"
	bcmRFConfMu   sync.Mutex
)

func boolTo01(b bool) int {
	if b {
		return 1
	}
	return 0
}

func guestBridgeName(vlanID int) (string, error) {
	if vlanID <= 0 {
		return "br0", nil
	}
	if vlanID < 2 || vlanID > 4094 {
		return "", errors.New("VLAN ID 范围应为 2–4094（0 表示不隔离）")
	}
	return fmt.Sprintf("br-vlan%d", vlanID), nil
}

// readBCMRFConf overlays system RF & PHY tuning values from /etc/bcm-rf.conf
// onto cfg. Returns false if the file does not exist or cannot be read.
func readBCMRFConf(path string, cfg *PanelConfig) bool {
	bcmRFConfMu.Lock()
	b, err := os.ReadFile(path)
	bcmRFConfMu.Unlock()
	if err != nil {
		return false
	}
	if cfg.Radios == nil {
		cfg.Radios = defaultPanelConfig().Radios
	}
	r0 := cfg.Radios["wl0"]
	r1 := cfg.Radios["wl1"]

	sc := bufio.NewScanner(strings.NewReader(string(b)))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		k = strings.TrimSpace(k)
		n, err := strconv.Atoi(strings.TrimSpace(v))
		if err != nil {
			continue
		}
		switch k {
		case "WL0_ENABLED":
			r0.Enabled = n != 0
		case "WL0_WIDTH":
			if n == 20 || n == 40 {
				r0.WidthMHz = n
			}
		case "WL0_HE":
			r0.HEEnabled = n != 0
		case "WL0_TXPWR_PCT":
			if n >= 10 && n <= 100 {
				r0.TxPwrPercent = n
			}
		case "WL0_ASSOC_RSSI_TH":
			if n == 0 || (n >= -95 && n <= -40) {
				r0.AssocRSSITh = n
			}
		case "WL0_APSD":
			r0.APSD = n != 0
		case "WL0_GUEST_VLAN":
			if n == 0 || (n >= 2 && n <= 4094) {
				r0.Guest.VLANID = n
			}
		case "WL1_ENABLED":
			r1.Enabled = n != 0
		case "WL1_WIDTH":
			if n == 20 || n == 40 || n == 80 || n == 160 {
				r1.WidthMHz = n
			}
		case "WL1_HE":
			r1.HEEnabled = n != 0
		case "WL1_TXPWR_PCT":
			if n >= 10 && n <= 100 {
				r1.TxPwrPercent = n
			}
		case "WL1_ASSOC_RSSI_TH":
			if n == 0 || (n >= -95 && n <= -40) {
				r1.AssocRSSITh = n
			}
		case "WL1_APSD":
			r1.APSD = n != 0
		case "WL1_TWT":
			r1.TWT = n != 0
		case "WL1_ATF":
			r1.ATF = n != 0
		case "WL1_GUEST_VLAN":
			if n == 0 || (n >= 2 && n <= 4094) {
				r1.Guest.VLANID = n
			}
		case "TUNE_FRAMEBURST":
			if n == 0 || n == 1 {
				cfg.Tune.Frameburst = n
			}
		case "TUNE_AMPDU_RTS":
			if n == 0 || n == 1 {
				cfg.Tune.AmpduRTS = n
			}
		case "TUNE_PHY_RXDESENS":
			if n >= 0 && n <= 30 {
				cfg.Tune.PhyRxdesens = n
			}
		}
	}
	cfg.Radios["wl0"] = r0
	cfg.Radios["wl1"] = r1
	return true
}

// writeBCMRFConf atomically persists RF & PHY tuning values to /etc/bcm-rf.conf.
func writeBCMRFConf(path string, cfg PanelConfig) error {
	r0 := cfg.Radios["wl0"]
	r1 := cfg.Radios["wl1"]
	if r0.WidthMHz != 40 {
		r0.WidthMHz = 20
	}
	if r0.TxPwrPercent < 10 || r0.TxPwrPercent > 100 {
		r0.TxPwrPercent = 100
	}
	if r1.WidthMHz != 20 && r1.WidthMHz != 40 && r1.WidthMHz != 80 && r1.WidthMHz != 160 {
		r1.WidthMHz = 160
	}
	if r1.TxPwrPercent < 10 || r1.TxPwrPercent > 100 {
		r1.TxPwrPercent = 100
	}
	var sb strings.Builder
	sb.WriteString("# /etc/bcm-rf.conf — Single Source of Truth for HG6371F RF & PHY tuning.\n")
	sb.WriteString("# SSIDs, passphrases, channels, and roaming live solely in /etc/hostapd/wl{0,1}.conf.\n\n")
	fmt.Fprintf(&sb, "WL0_ENABLED=%d\n", boolTo01(r0.Enabled))
	fmt.Fprintf(&sb, "WL0_WIDTH=%d\n", r0.WidthMHz)
	fmt.Fprintf(&sb, "WL0_HE=%d\n", boolTo01(r0.HEEnabled))
	fmt.Fprintf(&sb, "WL0_TXPWR_PCT=%d\n", r0.TxPwrPercent)
	fmt.Fprintf(&sb, "WL0_ASSOC_RSSI_TH=%d\n", r0.AssocRSSITh)
	fmt.Fprintf(&sb, "WL0_APSD=%d\n", boolTo01(r0.APSD))
	fmt.Fprintf(&sb, "WL0_GUEST_VLAN=%d\n\n", r0.Guest.VLANID)
	fmt.Fprintf(&sb, "WL1_ENABLED=%d\n", boolTo01(r1.Enabled))
	fmt.Fprintf(&sb, "WL1_WIDTH=%d\n", r1.WidthMHz)
	fmt.Fprintf(&sb, "WL1_HE=%d\n", boolTo01(r1.HEEnabled))
	fmt.Fprintf(&sb, "WL1_TXPWR_PCT=%d\n", r1.TxPwrPercent)
	fmt.Fprintf(&sb, "WL1_ASSOC_RSSI_TH=%d\n", r1.AssocRSSITh)
	fmt.Fprintf(&sb, "WL1_APSD=%d\n", boolTo01(r1.APSD))
	fmt.Fprintf(&sb, "WL1_TWT=%d\n", boolTo01(r1.TWT))
	fmt.Fprintf(&sb, "WL1_ATF=%d\n", boolTo01(r1.ATF))
	fmt.Fprintf(&sb, "WL1_GUEST_VLAN=%d\n\n", r1.Guest.VLANID)
	fmt.Fprintf(&sb, "TUNE_FRAMEBURST=%d\n", cfg.Tune.Frameburst)
	fmt.Fprintf(&sb, "TUNE_AMPDU_RTS=%d\n", cfg.Tune.AmpduRTS)
	fmt.Fprintf(&sb, "TUNE_PHY_RXDESENS=%d\n", cfg.Tune.PhyRxdesens)

	bcmRFConfMu.Lock()
	defer bcmRFConfMu.Unlock()
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, []byte(sb.String()), 0644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// updateRFConfig loads the latest /etc/bcm-rf.conf, applies fn, and atomically
// persists both /etc/bcm-rf.conf (single source of truth) and config.json.
func (s *Server) updateRFConfig(fn func(*PanelConfig)) error {
	s.cfgMu.Lock()
	defer s.cfgMu.Unlock()
	readBCMRFConf(bcmRFConfPath, &s.cfg)
	fn(&s.cfg)
	if err := writeBCMRFConf(bcmRFConfPath, s.cfg); err != nil {
		return err
	}
	return s.saveConfigLocked()
}
