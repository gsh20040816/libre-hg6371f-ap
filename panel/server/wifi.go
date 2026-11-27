package main

import (
	"bufio"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

type AirStats struct {
	TxPct     int `json:"tx_pct"`
	InBssPct  int `json:"inbss_pct"`
	ObssPct   int `json:"obss_pct"`
	BusyPct   int `json:"busy_pct"`
	IdlePct   int `json:"idle_pct"`
	TxopPct   int `json:"txop_pct"`
	Glitch    int `json:"glitch"`
	BadPLCP   int `json:"badplcp"`
	NoiseDBm  int `json:"noise_dbm"`
	ChipTempC int `json:"chip_temp_c"`
	TxRetrans float64 `json:"tx_retrans_pct"`
}

type RadioSummary struct {
	Iface     string         `json:"iface"`
	Band      string         `json:"band"`
	Up        bool           `json:"up"`
	Enabled   bool           `json:"enabled"`
	SSID      string         `json:"ssid"`
	BSSID     string         `json:"bssid"`
	Hidden    bool           `json:"hidden"`
	Isolate   bool           `json:"isolate"`
	Chanspec  string         `json:"chanspec"`
	Channel   int            `json:"channel"`
	WidthMHz  int            `json:"width_mhz"`
	NoiseDBm  int            `json:"noise_dbm"`
	TxPower   string         `json:"txpower"`
	TxPwrPct  int            `json:"txpwr_percent"`
	AssocTh   int            `json:"assoc_rssi_th"`
	Clients   int            `json:"clients"`
	HapdState string         `json:"hapd_state"`
	Air       AirStats       `json:"air"`
	Roaming   RoamingConfig  `json:"roaming"`
	MACMode   int            `json:"mac_mode"`
	MACList   []string       `json:"mac_list"`
	Guest     GuestBSSStatus `json:"guest"`
}

type GuestBSSStatus struct {
	Enabled   bool   `json:"enabled"`
	Iface     string `json:"iface"`
	Up        bool   `json:"up"`
	SSID      string `json:"ssid"`
	HasPass   bool   `json:"has_pass"`
	Hidden    bool   `json:"hidden"`
	Isolate   bool   `json:"isolate"`
	VLANID    int    `json:"vlan_id"`
	Clients   int    `json:"clients"`
}

type ClientInfo struct {
	MAC        string  `json:"mac"`
	IP         string  `json:"ip"`
	Hostname   string  `json:"hostname"`
	Note       string  `json:"note"`
	Iface      string  `json:"iface"`
	Band       string  `json:"band"`
	IsGuest    bool    `json:"is_guest"`
	RSSI       int     `json:"rssi"`
	AntRSSI    string  `json:"ant_rssi"`
	WidthNSS   string  `json:"width_nss"`
	TxRate     float64 `json:"tx_mbps"`
	RxRate     float64 `json:"rx_mbps"`
	TxBytes    uint64  `json:"tx_bytes"`
	RxBytes    uint64  `json:"rx_bytes"`
	RetryPct   float64 `json:"retry_pct"`
	IdleSec    int     `json:"idle_sec"`
	InNetSec   int     `json:"in_net_sec"`
	Mode       string  `json:"mode"`
	Chanspec   string  `json:"chanspec"`
	Connected  bool    `json:"connected"`
}

type WifiSaveReq struct {
	Iface        string         `json:"iface"`
	SSID         string         `json:"ssid"`
	Passphrase   string         `json:"passphrase"`
	Hidden       bool           `json:"hidden"`
	Isolate      bool           `json:"isolate"`
	Channel      int            `json:"channel"`
	WidthMHz     int            `json:"width_mhz"`
	TxPwrPercent int            `json:"txpwr_percent"`
	AssocRSSITh  int            `json:"assoc_rssi_th"`
	MACMode      *int           `json:"mac_mode,omitempty"`
	MACList      []string       `json:"mac_list,omitempty"`
	Roaming      *RoamingConfig `json:"roaming,omitempty"`
}

type TuneApplyReq struct {
	RunScript    bool `json:"run_script"`
	Frameburst   *int `json:"frameburst"`
	AmpduRTS     *int `json:"ampdu_rts"`
	PhyRxdesens  *int `json:"phy_rxdesens"`
	Wl1TxPwrPct  *int `json:"wl1_txpwr_pct"`
	Wl0TxPwrPct  *int `json:"wl0_txpwr_pct"`
	Wl1TWT       *int `json:"wl1_twt"`
	Wl1ATF       *int `json:"wl1_atf"`
	Wl1APSD      *int `json:"wl1_apsd"`
	Wl0HE        *int `json:"wl0_he"`
	Wl0WidthMHz  *int `json:"wl0_width_mhz"`
}

type ScanAP struct {
	SSID     string `json:"ssid"`
	BSSID    string `json:"bssid"`
	Channel  int    `json:"channel"`
	Primary  int    `json:"primary"`
	WidthMHz int    `json:"width_mhz"`
	Sideband string `json:"sideband,omitempty"` // "l" or "u" for 2.4G 40MHz
	Chanspec string `json:"chanspec"`
	RSSI     int    `json:"rssi"`
	Noise    int    `json:"noise"`
}

var (
	reNoise        = regexp.MustCompile(`noise:\s*(-?\d+)\s*dBm`)
	reStatusChan   = regexp.MustCompile(`Channel:\s*(\S+)`)
	reBSSID        = regexp.MustCompile(`BSSID:\s*([0-9A-Fa-f:]{17})`)
	reTxPwr        = regexp.MustCompile(`(\d+(?:\.\d+)?)\s*dbm`)
	reCntTxFrame   = regexp.MustCompile(`\btxframe\s+(\d+)`)
	reCntTxRetrans = regexp.MustCompile(`\btxretrans\s+(\d+)`)

	reStaTxBytes   = regexp.MustCompile(`tx total bytes:\s*(\d+)`)
	reStaRxBytes   = regexp.MustCompile(`rx data bytes:\s*(\d+)`)
	reStaTxRate    = regexp.MustCompile(`rate of last tx pkt:\s*(\d+)\s*kbps`)
	reStaMedTxRate = regexp.MustCompile(`median tx rate:\s*(\d+)\s*kbps`)
	reStaRxRate    = regexp.MustCompile(`rate of last rx pkt:\s*(\d+)\s*kbps`)
	reStaMedRxRate = regexp.MustCompile(`median rx rate:\s*(\d+)\s*kbps`)
	reStaIdle      = regexp.MustCompile(`idle\s+(\d+)\s+seconds`)
	reStaInNet     = regexp.MustCompile(`in network\s+(\d+)\s+seconds`)
	reStaChanspec  = regexp.MustCompile(`chanspec\s+(\S+)`)
	reStaAntRssi   = regexp.MustCompile(`per antenna average rssi of rx data frames:\s*(-?\d+)\s+(-?\d+)`)
	reStaRssi2     = regexp.MustCompile(`smoothed rssi:\s*(-?\d+)`)
	reStaOMI       = regexp.MustCompile(`OMI\s+\S+:\s*(\d+Mhz)\s+rx=(\d+ss)\s+tx=(\d+ss)`)
	reStaTxSent    = regexp.MustCompile(`tx total pkts sent:\s*(\d+)`)
	reStaTxRetries = regexp.MustCompile(`tx pkts retries:\s*(\d+)`)

	reScanSSID     = regexp.MustCompile(`^SSID:\s*"(.*)"`)
	reScanBSSID    = regexp.MustCompile(`BSSID:\s*([0-9A-Fa-f:]{17})`)
	reScanRSSI     = regexp.MustCompile(`RSSI:\s*(-?\d+)\s*dBm`)
	reScanNoise    = regexp.MustCompile(`noise:\s*(-?\d+)\s*dBm`)
	reScanChanspec = regexp.MustCompile(`Chanspec:\s*\S+\s+channel\s+(\d+)\s+(\d+)MHz`)
	reScanChanAlt  = regexp.MustCompile(`Channel:\s*(\S+)`)
)

func readHapdConf(iface string) map[string]string {
	m := make(map[string]string)
	b, err := os.ReadFile(fmt.Sprintf("/etc/hostapd/%s.conf", iface))
	if err != nil {
		return m
	}
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
		v = strings.TrimSpace(v)
		// Stop at secondary bss= section so primary keys aren't overwritten
		if k == "bss" {
			break
		}
		m[k] = v
	}
	return m
}

func parseChanspec(raw string) (ch int, width int, label string) {
	fields := strings.Fields(raw)
	if len(fields) == 0 {
		return 0, 20, ""
	}
	label = fields[0]
	width = 20
	if strings.Contains(label, "/160") {
		width = 160
	} else if strings.Contains(label, "/80") {
		width = 80
	} else if strings.HasSuffix(label, "l") || strings.HasSuffix(label, "u") || strings.Contains(label, "/40") {
		width = 40
	}
	numStr := label
	for i, c := range label {
		if c < '0' || c > '9' {
			numStr = label[:i]
			break
		}
	}
	ch, _ = strconv.Atoi(numStr)
	return ch, width, label
}

func collectAirStats(iface string) AirStats {
	var a AirStats
	raw := runCmdOut(2*time.Second, "wl", "-i", iface, "chanim_stats")
	lines := strings.Split(strings.TrimSpace(raw), "\n")
	if len(lines) >= 3 {
		// chspec tx inbss obss nocat nopkt doze txop goodtx badtx glitch badplcp knoise idle busy timestamp
		f := strings.Fields(lines[len(lines)-1])
		if len(f) >= 15 {
			a.TxPct, _ = strconv.Atoi(f[1])
			a.InBssPct, _ = strconv.Atoi(f[2])
			a.ObssPct, _ = strconv.Atoi(f[3])
			a.TxopPct, _ = strconv.Atoi(f[7])
			a.Glitch, _ = strconv.Atoi(f[10])
			a.BadPLCP, _ = strconv.Atoi(f[11])
			a.NoiseDBm, _ = strconv.Atoi(f[12])
			a.IdlePct, _ = strconv.Atoi(f[13])
			a.BusyPct, _ = strconv.Atoi(f[14])
		}
	}
	a.ChipTempC = wlInt(iface, "phy_tempsense")
	cnt := runCmdOut(2*time.Second, "wl", "-i", iface, "counters")
	var txFrame, txRetrans uint64
	if m := reCntTxFrame.FindStringSubmatch(cnt); len(m) == 2 {
		txFrame, _ = strconv.ParseUint(m[1], 10, 64)
	}
	if m := reCntTxRetrans.FindStringSubmatch(cnt); len(m) == 2 {
		txRetrans, _ = strconv.ParseUint(m[1], 10, 64)
	}
	if txFrame > 0 {
		a.TxRetrans = math.Round(float64(txRetrans)*1000.0/float64(txFrame+txRetrans)) / 10.0
	}
	return a
}

func (s *Server) collectRadioSummary(iface string) RadioSummary {
	band := "2.4G"
	if iface == "wl1" {
		band = "5G"
	}
	conf := readHapdConf(iface)
	cfg := s.getConfig()
	rc := cfg.Radios[iface]

	r := RadioSummary{
		Iface:    iface,
		Band:     band,
		SSID:     conf["ssid"],
		Hidden:   conf["ignore_broadcast_ssid"] == "1",
		Isolate:  conf["ap_isolate"] == "1",
		Enabled:  rc.Enabled,
		WidthMHz: rc.WidthMHz,
		TxPwrPct: rc.TxPwrPercent,
		AssocTh:  rc.AssocRSSITh,
		Roaming:  rc.Roaming,
		MACMode:  rc.MACMode,
		MACList:  rc.MACList,
	}
	if r.WidthMHz == 0 {
		if iface == "wl1" {
			r.WidthMHz = 160
		} else {
			r.WidthMHz = 20
		}
	}
	if r.TxPwrPct == 0 {
		r.TxPwrPct = 100
	}
	if r.MACList == nil {
		r.MACList = []string{}
	}

	st := runCmdOut(2*time.Second, "wl", "-i", iface, "status")
	if m := reBSSID.FindStringSubmatch(st); len(m) == 2 {
		r.BSSID = strings.ToUpper(m[1])
		r.Up = true
	}
	if m := reNoise.FindStringSubmatch(st); len(m) == 2 {
		r.NoiseDBm, _ = strconv.Atoi(m[1])
	}
	if m := reStatusChan.FindStringSubmatch(st); len(m) == 2 {
		r.Channel, r.WidthMHz, r.Chanspec = parseChanspec(m[1])
	}
	if r.Channel == 0 {
		r.Channel, _ = strconv.Atoi(conf["channel"])
	}
	if r.Up {
		tp := runCmdOut(2*time.Second, "wl", "-i", iface, "txpwr1")
		if m := reTxPwr.FindStringSubmatch(strings.ToLower(tp)); len(m) == 2 {
			r.TxPower = m[1] + " dBm"
		}
		r.Air = collectAirStats(iface)
		if r.NoiseDBm == 0 && r.Air.NoiseDBm != 0 {
			r.NoiseDBm = r.Air.NoiseDBm
		}
		assoc := runCmdOut(2*time.Second, "wl", "-i", iface, "assoclist")
		for _, line := range strings.Split(assoc, "\n") {
			if strings.HasPrefix(strings.TrimSpace(line), "assoclist ") {
				r.Clients++
			}
		}
		r.HapdState = "ENABLED"
	}

	// Guest BSS status (wl1.1 / wl0.1) — only probe if interface exists in sysfs
	gIface := iface + ".1"
	gUp := false
	gClients := 0
	if _, err := os.Stat("/sys/class/net/" + gIface); err == nil {
		gUp = strings.TrimSpace(runCmdOut(1*time.Second, "wl", "-i", gIface, "isup")) == "1"
		if gUp {
			ga := runCmdOut(1*time.Second, "wl", "-i", gIface, "assoclist")
			for _, line := range strings.Split(ga, "\n") {
				if strings.HasPrefix(strings.TrimSpace(line), "assoclist ") {
					gClients++
				}
			}
		}
	}
	r.Guest = GuestBSSStatus{
		Enabled: rc.Guest.Enabled,
		Iface:   gIface,
		Up:      gUp,
		SSID:    rc.Guest.SSID,
		HasPass: len(rc.Guest.Passphrase) >= 8,
		Hidden:  rc.Guest.Hidden,
		Isolate: rc.Guest.Isolate,
		VLANID:  rc.Guest.VLANID,
		Clients: gClients,
	}
	r.Clients += gClients
	return r
}

func (s *Server) collectClientsFor(iface string) []ClientInfo {
	baseIface := strings.SplitN(iface, ".", 2)[0]
	isGuest := strings.Contains(iface, ".")
	band := "2.4G"
	if baseIface == "wl1" {
		band = "5G"
	}
	var out []ClientInfo
	assoc := runCmdOut(2*time.Second, "wl", "-i", iface, "assoclist")
	for _, line := range strings.Split(assoc, "\n") {
		f := strings.Fields(line)
		if len(f) != 2 || f[0] != "assoclist" {
			continue
		}
		mac := strings.ToUpper(f[1])
		info := runCmdOut(2*time.Second, "wl", "-i", iface, "sta_info", mac)
		c := ClientInfo{
			MAC:       mac,
			IP:        s.lookupIP(mac),
			Hostname:  s.lookupHostname(mac),
			Note:      s.getNote(mac),
			Iface:     iface,
			Band:      band,
			IsGuest:   isGuest,
			Connected: strings.Contains(info, "AUTHORIZED"),
		}
		if m := reStaTxBytes.FindStringSubmatch(info); len(m) == 2 {
			c.TxBytes, _ = strconv.ParseUint(m[1], 10, 64)
		}
		if m := reStaRxBytes.FindStringSubmatch(info); len(m) == 2 {
			c.RxBytes, _ = strconv.ParseUint(m[1], 10, 64)
		}
		if m := reStaMedTxRate.FindStringSubmatch(info); len(m) == 2 {
			kbps, _ := strconv.ParseFloat(m[1], 64)
			c.TxRate = math.Round(kbps/100.0) / 10.0
		} else if m := reStaTxRate.FindStringSubmatch(info); len(m) == 2 {
			kbps, _ := strconv.ParseFloat(m[1], 64)
			c.TxRate = math.Round(kbps/100.0) / 10.0
		}
		if m := reStaMedRxRate.FindStringSubmatch(info); len(m) == 2 {
			kbps, _ := strconv.ParseFloat(m[1], 64)
			c.RxRate = math.Round(kbps/100.0) / 10.0
		} else if m := reStaRxRate.FindStringSubmatch(info); len(m) == 2 {
			kbps, _ := strconv.ParseFloat(m[1], 64)
			c.RxRate = math.Round(kbps/100.0) / 10.0
		}
		if m := reStaIdle.FindStringSubmatch(info); len(m) == 2 {
			c.IdleSec, _ = strconv.Atoi(m[1])
		}
		if m := reStaInNet.FindStringSubmatch(info); len(m) == 2 {
			c.InNetSec, _ = strconv.Atoi(m[1])
		}
		if m := reStaChanspec.FindStringSubmatch(info); len(m) == 2 {
			c.Chanspec = m[1]
		}
		if m := reStaOMI.FindStringSubmatch(info); len(m) == 4 {
			c.WidthNSS = strings.TrimSuffix(m[1], "Mhz") + "M · " + strings.ToUpper(m[2])
		}
		if m := reStaAntRssi.FindStringSubmatch(info); len(m) == 3 {
			a0, _ := strconv.Atoi(m[1])
			a1, _ := strconv.Atoi(m[2])
			if a0 < 0 && a1 < 0 {
				c.AntRSSI = fmt.Sprintf("%d / %d", a0, a1)
				if a0 > a1 {
					c.RSSI = a0
				} else {
					c.RSSI = a1
				}
			} else if a0 < 0 {
				c.RSSI = a0
			}
		}
		if c.RSSI == 0 {
			if m := reStaRssi2.FindStringSubmatch(info); len(m) == 2 {
				c.RSSI, _ = strconv.Atoi(m[1])
			} else {
				r := strings.TrimSpace(runCmdOut(2*time.Second, "wl", "-i", iface, "rssi", mac))
				c.RSSI, _ = strconv.Atoi(r)
			}
		}
		var sent, retries uint64
		if m := reStaTxSent.FindStringSubmatch(info); len(m) == 2 {
			sent, _ = strconv.ParseUint(m[1], 10, 64)
		}
		if m := reStaTxRetries.FindStringSubmatch(info); len(m) == 2 {
			retries, _ = strconv.ParseUint(m[1], 10, 64)
		}
		if sent > 0 {
			pct := float64(retries) * 100.0 / float64(sent+retries)
			c.RetryPct = math.Round(pct*10) / 10
		}
		switch {
		case strings.Contains(info, "HE_CAP"):
			c.Mode = "Wi-Fi 6"
		case strings.Contains(info, "VHT_CAP"):
			c.Mode = "Wi-Fi 5"
		case strings.Contains(info, "N_CAP"):
			c.Mode = "Wi-Fi 4"
		default:
			c.Mode = "Legacy"
		}
		out = append(out, c)
	}
	return out
}

func (s *Server) collectWifi() map[string]any {
	s.refreshARPTable()
	r1 := s.collectRadioSummary("wl1")
	r0 := s.collectRadioSummary("wl0")
	clients := append(s.collectClientsFor("wl1"), s.collectClientsFor("wl0")...)
	if r1.Guest.Up {
		clients = append(clients, s.collectClientsFor("wl1.1")...)
	}
	if r0.Guest.Up {
		clients = append(clients, s.collectClientsFor("wl0.1")...)
	}
	sort.Slice(clients, func(i, j int) bool {
		if clients[i].Band != clients[j].Band {
			return clients[i].Band > clients[j].Band
		}
		if clients[i].IsGuest != clients[j].IsGuest {
			return !clients[i].IsGuest
		}
		return clients[i].MAC < clients[j].MAC
	})
	return map[string]any{
		"radios":  []RadioSummary{r1, r0},
		"clients": clients,
	}
}

func deriveGuestBSSID(iface string) string {
	mac := strings.ToLower(readTrim("/sys/class/net/" + iface + "/address"))
	parts := strings.Split(mac, ":")
	if len(parts) != 6 {
		return ""
	}
	b0, e0 := strconv.ParseUint(parts[0], 16, 8)
	b5, e5 := strconv.ParseUint(parts[5], 16, 8)
	if e0 != nil || e5 != nil {
		return ""
	}
	// Set locally administered bit (0x02), clear multicast bit (0x01), increment last octet
	b0 = (b0 | 0x02) &^ 0x01
	b5 = (b5 + 1) & 0xff
	return fmt.Sprintf("%02x:%s:%s:%s:%s:%02x", b0, parts[1], parts[2], parts[3], parts[4], b5)
}

func (s *Server) writeHostapdConf(iface string, primary map[string]string, rc RadioConfig) error {
	hwMode := "g"
	if iface == "wl1" {
		hwMode = "a"
	}
	ssid := primary["ssid"]
	pass := primary["wpa_passphrase"]
	ch := primary["channel"]
	if ch == "" {
		if iface == "wl1" {
			ch = "48"
		} else {
			ch = "6"
		}
	}
	hiddenVal := primary["ignore_broadcast_ssid"]
	if hiddenVal == "" {
		hiddenVal = "0"
	}
	isoVal := "0"
	if rc.Isolate {
		isoVal = "1"
	}

	var sb strings.Builder
	fmt.Fprintf(&sb, `driver=nl80211
interface=%s
bridge=br0
ctrl_interface=/run/hostapd
ctrl_interface_group=0

hw_mode=%s
channel=%s
beacon_int=100
dtim_period=1
wmm_enabled=1
auth_algs=1
ap_isolate=%s
ignore_broadcast_ssid=%s

ssid=%s
wpa=2
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
ieee80211w=0
wpa_passphrase=%s
`, iface, hwMode, ch, isoVal, hiddenVal, ssid, pass)

	// 802.11k / 802.11v / 802.11r roaming options
	if rc.Roaming.Dot11k {
		sb.WriteString("rrm_neighbor_report=1\nrrm_beacon_report=1\n")
	}
	if rc.Roaming.Dot11v {
		sb.WriteString("bss_transition=1\n")
	}
	if rc.Roaming.Dot11r {
		md := strings.ToLower(strings.TrimSpace(rc.Roaming.MobilityDomain))
		if len(md) != 4 {
			md = "6371"
		}
		sb.WriteString("wpa_key_mgmt=WPA-PSK FT-PSK\n")
		fmt.Fprintf(&sb, "mobility_domain=%s\nft_over_ds=0\nft_psk_generate_local=1\n", md)
	}

	// MAC ACL via hostapd accept/deny files
	acceptPath := fmt.Sprintf("/etc/hostapd/%s.accept", iface)
	denyPath := fmt.Sprintf("/etc/hostapd/%s.deny", iface)
	if rc.MACMode == 1 && len(rc.MACList) > 0 {
		_ = os.WriteFile(denyPath, []byte(strings.Join(rc.MACList, "\n")+"\n"), 0600)
		fmt.Fprintf(&sb, "macaddr_acl=0\ndeny_mac_file=%s\n", denyPath)
	} else if rc.MACMode == 2 && len(rc.MACList) > 0 {
		_ = os.WriteFile(acceptPath, []byte(strings.Join(rc.MACList, "\n")+"\n"), 0600)
		fmt.Fprintf(&sb, "macaddr_acl=1\naccept_mac_file=%s\n", acceptPath)
	}

	// Optional secondary Guest / IoT BSS (wl1.1 / wl0.1) with optional 802.1Q VLAN bridge
	if rc.Guest.Enabled && len(rc.Guest.SSID) > 0 && len(rc.Guest.Passphrase) >= 8 {
		brName, err := guestBridgeName(rc.Guest.VLANID)
		if err != nil {
			return err
		}
		gIface := iface + ".1"
		gBSSID := deriveGuestBSSID(iface)
		gHidden := "0"
		if rc.Guest.Hidden {
			gHidden = "1"
		}
		gIso := "0"
		if rc.Guest.Isolate {
			gIso = "1"
		}
		fmt.Fprintf(&sb, `
bss=%s
ctrl_interface=/run/hostapd
ctrl_interface_group=0
bridge=%s
`, gIface, brName)
		if gBSSID != "" {
			fmt.Fprintf(&sb, "bssid=%s\n", gBSSID)
		}
		fmt.Fprintf(&sb, `ssid=%s
ignore_broadcast_ssid=%s
ap_isolate=%s
wmm_enabled=1
auth_algs=1
wpa=2
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
ieee80211w=0
wpa_passphrase=%s
`, rc.Guest.SSID, gHidden, gIso, rc.Guest.Passphrase)
	}

	confPath := fmt.Sprintf("/etc/hostapd/%s.conf", iface)
	tmp := confPath + ".tmp"
	if err := os.WriteFile(tmp, []byte(sb.String()), 0600); err != nil {
		return err
	}
	return os.Rename(tmp, confPath)
}

func (s *Server) applyRadioPower(iface string, on bool) error {
	if iface != "wl0" && iface != "wl1" {
		return errors.New("接口必须是 wl0 或 wl1")
	}
	svc := "hostapd." + iface
	if on {
		_ = runCmd(15*time.Second, "rc-service", svc, "start")
	} else {
		s.suspendRadar(iface)
		defer s.resumeRadar(iface)
		_ = runCmd(10*time.Second, "rc-service", svc, "stop")
	}
	return nil
}

func (s *Server) toggleRadio(iface string, enabled bool) error {
	if iface != "wl0" && iface != "wl1" {
		return errors.New("接口必须是 wl0 或 wl1")
	}
	if err := s.updateRFConfig(func(c *PanelConfig) {
		rc := c.Radios[iface]
		rc.Enabled = enabled
		c.Radios[iface] = rc
	}); err != nil {
		return err
	}
	return s.applyRadioPower(iface, enabled)
}

func sanitizeMACList(list []string) ([]string, error) {
	seen := make(map[string]bool)
	out := make([]string, 0, len(list))
	for _, item := range list {
		m := strings.ToUpper(strings.TrimSpace(item))
		if m == "" {
			continue
		}
		if !reMAC.MatchString(m) {
			return nil, fmt.Errorf("无效的 MAC 地址: %s", m)
		}
		if !seen[m] {
			seen[m] = true
			out = append(out, m)
		}
	}
	return out, nil
}

func (s *Server) saveWifiConfig(req WifiSaveReq) error {
	if req.Iface != "wl0" && req.Iface != "wl1" {
		return errors.New("接口必须是 wl0 或 wl1")
	}
	req.SSID = strings.TrimSpace(req.SSID)
	if len(req.SSID) == 0 || len(req.SSID) > 32 || strings.ContainsAny(req.SSID, "\r\n") {
		return errors.New("SSID 长度需为 1-32 字符且不含换行")
	}
	if req.Passphrase != "" && (len(req.Passphrase) < 8 || len(req.Passphrase) > 63 || strings.ContainsAny(req.Passphrase, "\r\n")) {
		return errors.New("Wi-Fi 密码需为 8-63 位")
	}

	old := readHapdConf(req.Iface)
	pass := req.Passphrase
	if pass == "" {
		pass = old["wpa_passphrase"]
	}
	if len(pass) < 8 {
		return errors.New("缺少有效的 Wi-Fi 密码")
	}

	if req.AssocRSSITh != 0 && (req.AssocRSSITh < -95 || req.AssocRSSITh > -40) {
		return errors.New("弱信号拒连阈值需为 0（关闭）或 -95 ~ -40 dBm")
	}
	var parsedMACList []string
	if req.MACMode != nil {
		if *req.MACMode < 0 || *req.MACMode > 2 {
			return errors.New("MAC 过滤模式需为 0/1/2")
		}
		ml, err := sanitizeMACList(req.MACList)
		if err != nil {
			return err
		}
		parsedMACList = ml
	}
	var parsedRoaming *RoamingConfig
	if req.Roaming != nil {
		rm := *req.Roaming
		rm.MobilityDomain = strings.ToLower(strings.TrimSpace(rm.MobilityDomain))
		if rm.MobilityDomain == "" {
			rm.MobilityDomain = "6371"
		}
		if len(rm.MobilityDomain) != 4 {
			return errors.New("802.11r Mobility Domain 必须是 4 位十六进制字符")
		}
		if _, err := hex.DecodeString(rm.MobilityDomain); err != nil {
			return errors.New("802.11r Mobility Domain 必须是有效十六进制")
		}
		parsedRoaming = &rm
	}

	hiddenVal := "0"
	if req.Hidden {
		hiddenVal = "1"
	}
	primary := map[string]string{
		"ssid":                  req.SSID,
		"wpa_passphrase":        pass,
		"channel":               strconv.Itoa(req.Channel),
		"ignore_broadcast_ssid": hiddenVal,
	}

	var savedRC RadioConfig
	if err := s.updateRFConfig(func(c *PanelConfig) {
		r := c.Radios[req.Iface]
		r.Isolate = req.Isolate
		if req.TxPwrPercent >= 10 && req.TxPwrPercent <= 100 {
			r.TxPwrPercent = req.TxPwrPercent
		}
		r.AssocRSSITh = req.AssocRSSITh
		if req.WidthMHz > 0 {
			r.WidthMHz = req.WidthMHz
		}
		if req.MACMode != nil {
			r.MACMode = *req.MACMode
			r.MACList = parsedMACList
		}
		if parsedRoaming != nil {
			r.Roaming = *parsedRoaming
		}
		c.Radios[req.Iface] = r
		savedRC = r
	}); err != nil {
		return err
	}
	if err := s.writeHostapdConf(req.Iface, primary, savedRC); err != nil {
		return err
	}
	if !savedRC.Enabled {
		return nil
	}

	return s.reloadRadioHostapd(req.Iface)
}

func (s *Server) saveGuestConfig(iface string, req GuestBSSConfig) error {
	if iface != "wl0" && iface != "wl1" {
		return errors.New("接口必须是 wl0 或 wl1")
	}
	req.SSID = strings.TrimSpace(req.SSID)
	if req.Passphrase == "" {
		req.Passphrase = s.getConfig().Radios[iface].Guest.Passphrase
	}
	if req.Enabled {
		if len(req.SSID) == 0 || len(req.SSID) > 32 || strings.ContainsAny(req.SSID, "\r\n") {
			return errors.New("访客 SSID 长度需为 1-32 字符")
		}
		if len(req.Passphrase) < 8 || len(req.Passphrase) > 63 || strings.ContainsAny(req.Passphrase, "\r\n") {
			return errors.New("访客密码需为 8-63 位")
		}
		if req.VLANID != 0 && (req.VLANID < 2 || req.VLANID > 4094) {
			return errors.New("VLAN ID 需为 0（不隔离）或 2–4094")
		}
	}

	primary := readHapdConf(iface)
	if primary["ssid"] == "" || len(primary["wpa_passphrase"]) < 8 {
		return errors.New("主无线配置不完整，请先保存主无线配置")
	}

	var savedRC RadioConfig
	if err := s.updateRFConfig(func(c *PanelConfig) {
		r := c.Radios[iface]
		r.Guest = req
		c.Radios[iface] = r
		savedRC = r
	}); err != nil {
		return err
	}
	if err := s.writeHostapdConf(iface, primary, savedRC); err != nil {
		return err
	}
	if !savedRC.Enabled {
		return nil
	}
	return s.reloadRadioHostapd(iface)
}

func (s *Server) reloadRadioHostapd(iface string) error {
	s.suspendRadar(iface)
	defer s.resumeRadar(iface)
	_ = runCmd(15*time.Second, "rc-service", "hostapd."+iface, "restart")
	return nil
}

func parseScanResults(raw string) []ScanAP {
	var aps []ScanAP
	var cur *ScanAP
	flush := func() {
		if cur != nil && cur.BSSID != "" {
			aps = append(aps, *cur)
		}
		cur = nil
	}
	for _, line := range strings.Split(raw, "\n") {
		line = strings.TrimSpace(line)
		if m := reScanSSID.FindStringSubmatch(line); len(m) == 2 {
			flush()
			cur = &ScanAP{SSID: m[1]}
			continue
		}
		if cur == nil {
			continue
		}
		if m := reScanBSSID.FindStringSubmatch(line); len(m) == 2 {
			cur.BSSID = strings.ToUpper(m[1])
		}
		if m := reScanRSSI.FindStringSubmatch(line); len(m) == 2 {
			cur.RSSI, _ = strconv.Atoi(m[1])
		}
		if m := reScanNoise.FindStringSubmatch(line); len(m) == 2 {
			cur.Noise, _ = strconv.Atoi(m[1])
		}
		if m := reScanChanAlt.FindStringSubmatch(line); len(m) == 2 {
			ch, w, lbl := parseChanspec(m[1])
			cur.Primary = ch
			if cur.WidthMHz == 0 {
				cur.WidthMHz = w
			}
			if strings.HasSuffix(lbl, "l") {
				cur.Sideband = "l"
			} else if strings.HasSuffix(lbl, "u") {
				cur.Sideband = "u"
			}
			if cur.Channel == 0 {
				cur.Channel = ch
				cur.Chanspec = lbl
			}
		}
		if m := reScanChanspec.FindStringSubmatch(line); len(m) == 3 {
			// Broadcom `Chanspec:` reports the wide-band center channel (e.g. 50 for 160MHz 36..64,
			// 58 for 80MHz 52..64, 155 for 80MHz 149..161, 3 for 40MHz 1..5). Keep Primary as the
			// displayed channel so users see the actual 20MHz primary channel + width.
			centerCh, _ := strconv.Atoi(m[1])
			w, _ := strconv.Atoi(m[2])
			if w > 0 {
				cur.WidthMHz = w
			}
			dispCh := cur.Primary
			if dispCh == 0 {
				dispCh = centerCh
			}
			cur.Channel = dispCh
			if cur.Sideband != "" && cur.WidthMHz == 40 {
				cur.Chanspec = fmt.Sprintf("%d%s / %d MHz", dispCh, cur.Sideband, cur.WidthMHz)
			} else if cur.WidthMHz > 0 {
				cur.Chanspec = fmt.Sprintf("%d / %d MHz", dispCh, cur.WidthMHz)
			} else {
				cur.Chanspec = strconv.Itoa(dispCh)
			}
		}
	}
	flush()
	return aps
}

func (s *Server) scanWifi(iface string) (map[string]any, error) {
	s.scanMu.Lock()
	defer s.scanMu.Unlock()
	if s.radar != nil {
		s.radar.pauseForScan(iface, 15*time.Second)
		defer s.radar.pauseForScan(iface, 3*time.Second)
	}

	_ = runCmd(5*time.Second, "wl", "-i", iface, "scan")
	time.Sleep(2 * time.Second)
	raw := runCmdOut(5*time.Second, "wl", "-i", iface, "scanresults")
	aps := parseScanResults(raw)

	sort.Slice(aps, func(i, j int) bool {
		if aps[i].RSSI != aps[j].RSSI {
			return aps[i].RSSI > aps[j].RSSI
		}
		return aps[i].BSSID < aps[j].BSSID
	})
	return map[string]any{
		"iface": iface,
		"count": len(aps),
		"aps":   aps,
	}, nil
}

// sampleWalkTest performs a fast single-STA radio telemetry lookup for handheld signal mapping.
func (s *Server) sampleWalkTest(callerIP, reqMAC string) map[string]any {
	callerMAC := s.lookupMACByIP(callerIP)
	targetMAC := strings.ToUpper(strings.TrimSpace(reqMAC))
	autoDetected := false
	if targetMAC == "" && callerMAC != "" {
		targetMAC = callerMAC
		autoDetected = true
	}

	res := map[string]any{
		"ts_ms":         time.Now().UnixMilli(),
		"caller_ip":     callerIP,
		"caller_mac":    callerMAC,
		"mac":           targetMAC,
		"auto_detected": autoDetected,
		"online":        false,
	}
	if !reMAC.MatchString(targetMAC) {
		return res
	}
	res["name"] = s.displayName(targetMAC)
	res["ip"] = s.lookupIP(targetMAC)

	// Probe active wireless interfaces directly for this single STA (fast, ~4-8ms total)
	ifaces := []string{"wl1", "wl0"}
	cfg := s.getConfig()
	if cfg.Radios["wl1"].Guest.Enabled {
		ifaces = append(ifaces, "wl1.1")
	}
	if cfg.Radios["wl0"].Guest.Enabled {
		ifaces = append(ifaces, "wl0.1")
	}

	for _, iface := range ifaces {
		info := runCmdOut(1*time.Second, "wl", "-i", iface, "sta_info", targetMAC)
		if info == "" || !strings.Contains(info, "in network") {
			continue
		}
		baseIface := strings.SplitN(iface, ".", 2)[0]
		band := "2.4G"
		if baseIface == "wl1" {
			band = "5G"
		}

		var rssi, ant0, ant1 int
		var antRSSI string
		if m := reStaAntRssi.FindStringSubmatch(info); len(m) == 3 {
			ant0, _ = strconv.Atoi(m[1])
			ant1, _ = strconv.Atoi(m[2])
			if ant0 < 0 && ant1 < 0 {
				antRSSI = fmt.Sprintf("%d / %d", ant0, ant1)
				if ant0 > ant1 {
					rssi = ant0
				} else {
					rssi = ant1
				}
			} else if ant0 < 0 {
				rssi = ant0
			} else if ant1 < 0 {
				rssi = ant1
			}
		}
		if rssi == 0 {
			if m := reStaRssi2.FindStringSubmatch(info); len(m) == 2 {
				rssi, _ = strconv.Atoi(m[1])
			} else if r := strings.TrimSpace(runCmdOut(1*time.Second, "wl", "-i", iface, "rssi", targetMAC)); r != "" {
				rssi, _ = strconv.Atoi(strings.Fields(r)[0])
			}
		}

		noise := wlInt(baseIface, "noise")
		snr := 0
		if rssi < 0 && noise < 0 {
			snr = rssi - noise
			if snr < 0 {
				snr = 0
			}
		}

		var txMbps, rxMbps float64
		if m := reStaTxRate.FindStringSubmatch(info); len(m) == 2 {
			kbps, _ := strconv.ParseFloat(m[1], 64)
			txMbps = math.Round(kbps/100.0) / 10.0
		} else if m := reStaMedTxRate.FindStringSubmatch(info); len(m) == 2 {
			kbps, _ := strconv.ParseFloat(m[1], 64)
			txMbps = math.Round(kbps/100.0) / 10.0
		}
		if m := reStaRxRate.FindStringSubmatch(info); len(m) == 2 {
			kbps, _ := strconv.ParseFloat(m[1], 64)
			rxMbps = math.Round(kbps/100.0) / 10.0
		} else if m := reStaMedRxRate.FindStringSubmatch(info); len(m) == 2 {
			kbps, _ := strconv.ParseFloat(m[1], 64)
			rxMbps = math.Round(kbps/100.0) / 10.0
		}

		var txSent, txRetries uint64
		if m := reStaTxSent.FindStringSubmatch(info); len(m) == 2 {
			txSent, _ = strconv.ParseUint(m[1], 10, 64)
		}
		if m := reStaTxRetries.FindStringSubmatch(info); len(m) == 2 {
			txRetries, _ = strconv.ParseUint(m[1], 10, 64)
		}
		retryPct := 0.0
		if txSent > 0 {
			retryPct = math.Round(float64(txRetries)*1000.0/float64(txSent+txRetries)) / 10.0
		}

		ch, width, _ := parseChanspec(runCmdOut(1*time.Second, "wl", "-i", baseIface, "chanspec"))
		csText := fmt.Sprintf("%d / %d MHz", ch, width)
		if ch == 0 {
			csText = "—"
		}
		widthNSS := ""
		if m := reStaOMI.FindStringSubmatch(info); len(m) == 4 {
			widthNSS = strings.TrimSuffix(m[1], "Mhz") + "M · " + strings.ToUpper(m[2])
		}
		mode := "Legacy"
		switch {
		case strings.Contains(info, "HE_CAP"):
			mode = "Wi-Fi 6"
		case strings.Contains(info, "VHT_CAP"):
			mode = "Wi-Fi 5"
		case strings.Contains(info, "N_CAP"):
			mode = "Wi-Fi 4"
		}

		res["online"] = true
		res["iface"] = iface
		res["band"] = band
		res["is_guest"] = strings.Contains(iface, ".")
		res["channel"] = ch
		res["width_mhz"] = width
		res["chanspec"] = csText
		res["width_nss"] = widthNSS
		res["mode"] = mode
		res["rssi"] = rssi
		res["ant0"] = ant0
		res["ant1"] = ant1
		res["ant_rssi"] = antRSSI
		res["noise"] = noise
		res["snr"] = snr
		res["tx_mbps"] = txMbps
		res["rx_mbps"] = rxMbps
		res["tx_sent"] = txSent
		res["tx_retries"] = txRetries
		res["retry_pct"] = retryPct
		return res
	}
	return res
}

const (
	walkTestSpotsFile = "/tmp/ap-wt-spots.json"
	walkTestSpotsMax  = 60
)

type WalkTestSpot struct {
	ID       int64   `json:"id"`
	Spot     string  `json:"spot"`
	Time     string  `json:"time"`
	Device   string  `json:"device"`
	MAC      string  `json:"mac"`
	Band     string  `json:"band"`
	Chanspec string  `json:"chanspec"`
	RSSI     int     `json:"rssi"`
	AntRSSI  string  `json:"ant_rssi"`
	SNR      int     `json:"snr"`
	TxMbps   float64 `json:"tx_mbps"`
	RxMbps   float64 `json:"rx_mbps"`
	RTTMs    float64 `json:"rtt_ms"`
	RetryPct float64 `json:"retry_pct"`
}

func (s *Server) loadWalkTestSpots() {
	b, err := os.ReadFile(walkTestSpotsFile)
	if err != nil {
		return
	}
	var loaded []WalkTestSpot
	if json.Unmarshal(b, &loaded) != nil {
		return
	}
	if len(loaded) > walkTestSpotsMax {
		loaded = loaded[:walkTestSpotsMax]
	}
	s.wtMu.Lock()
	s.wtSpots = loaded
	s.wtMu.Unlock()
}

func (s *Server) saveWalkTestSpotsLocked() error {
	b, err := json.MarshalIndent(s.wtSpots, "", "  ")
	if err != nil {
		return err
	}
	tmp := walkTestSpotsFile + ".tmp"
	if err := os.WriteFile(tmp, b, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, walkTestSpotsFile)
}

func (s *Server) getWalkTestSpots() []WalkTestSpot {
	s.wtMu.Lock()
	defer s.wtMu.Unlock()
	out := make([]WalkTestSpot, len(s.wtSpots))
	copy(out, s.wtSpots)
	return out
}

func (s *Server) addWalkTestSpot(spot WalkTestSpot) ([]WalkTestSpot, error) {
	spot.Spot = strings.TrimSpace(spot.Spot)
	if len([]rune(spot.Spot)) > 32 {
		spot.Spot = string([]rune(spot.Spot)[:32])
	}
	spot.Device = strings.TrimSpace(spot.Device)
	spot.MAC = strings.ToUpper(strings.TrimSpace(spot.MAC))
	spot.ID = time.Now().UnixNano()
	if spot.Time == "" {
		spot.Time = time.Now().Format("15:04:05")
	}

	s.wtMu.Lock()
	defer s.wtMu.Unlock()
	if spot.Spot == "" {
		spot.Spot = fmt.Sprintf("点位 %d", len(s.wtSpots)+1)
	}
	s.wtSpots = append([]WalkTestSpot{spot}, s.wtSpots...)
	if len(s.wtSpots) > walkTestSpotsMax {
		s.wtSpots = s.wtSpots[:walkTestSpotsMax]
	}
	err := s.saveWalkTestSpotsLocked()
	out := make([]WalkTestSpot, len(s.wtSpots))
	copy(out, s.wtSpots)
	return out, err
}

func (s *Server) deleteWalkTestSpot(id int64) ([]WalkTestSpot, error) {
	s.wtMu.Lock()
	defer s.wtMu.Unlock()
	if id == 0 {
		s.wtSpots = nil
	} else {
		filtered := s.wtSpots[:0]
		for _, sp := range s.wtSpots {
			if sp.ID != id {
				filtered = append(filtered, sp)
			}
		}
		s.wtSpots = filtered
	}
	err := s.saveWalkTestSpotsLocked()
	out := make([]WalkTestSpot, len(s.wtSpots))
	copy(out, s.wtSpots)
	return out, err
}

func wlInt(iface, param string) int {
	out := strings.TrimSpace(runCmdOut(2*time.Second, "wl", "-i", iface, param))
	f := strings.Fields(out)
	if len(f) == 0 {
		return 0
	}
	v, _ := strconv.Atoi(f[0])
	return v
}

func (s *Server) collectTune() map[string]any {
	wmeRaw := runCmdOut(2*time.Second, "wl", "-i", "wl1", "wme_ac", "ap")
	wmeBE := ""
	lines := strings.Split(wmeRaw, "\n")
	for i, l := range lines {
		if strings.HasPrefix(strings.TrimSpace(l), "AC_BE:") && i+1 < len(lines) {
			wmeBE = strings.TrimSpace(lines[i+1])
			break
		}
	}
	_, wl0Width, _ := parseChanspec(runCmdOut(2*time.Second, "wl", "-i", "wl0", "chanspec"))
	return map[string]any{
		"wl1": map[string]any{
			"frameburst":     wlInt("wl1", "frameburst"),
			"ampdu_rts":      wlInt("wl1", "ampdu_rts"),
			"phy_rxdesens":   wlInt("wl1", "phy_rxdesens"),
			"txpwr_percent":  wlInt("wl1", "txpwr_percent"),
			"twt":            wlInt("wl1", "twt"),
			"atf":            wlInt("wl1", "atf"),
			"wme_apsd":       wlInt("wl1", "wme_apsd"),
			"txbf":           wlInt("wl1", "txbf"),
			"mu_features":    wlInt("wl1", "mu_features"),
			"wmf_bss_enable": wlInt("wl1", "wmf_bss_enable"),
			"wme_be":         wmeBE,
			"air":            collectAirStats("wl1"),
		},
		"wl0": map[string]any{
			"frameburst":     wlInt("wl0", "frameburst"),
			"txpwr_percent":  wlInt("wl0", "txpwr_percent"),
			"he":             wlInt("wl0", "he"),
			"width_mhz":      wl0Width,
			"txbf":           wlInt("wl0", "txbf"),
			"wmf_bss_enable": wlInt("wl0", "wmf_bss_enable"),
			"air":            collectAirStats("wl0"),
		},
		"sysctl": map[string]string{
			"rmem_max":           readTrim("/proc/sys/net/core/rmem_max"),
			"wmem_max":           readTrim("/proc/sys/net/core/wmem_max"),
			"netdev_max_backlog": readTrim("/proc/sys/net/core/netdev_max_backlog"),
		},
	}
}

func (s *Server) applyBaselineTune() {
	_ = runCmd(5*time.Second, "/usr/local/sbin/bcm-rf", "tune")
}

func (s *Server) applyTune(req TuneApplyReq) error {
	if req.RunScript {
		s.applyBaselineTune()
		return nil
	}
	if req.Frameburst != nil && *req.Frameburst != 0 && *req.Frameburst != 1 {
		return errors.New("frameburst 仅支持 0 或 1")
	}
	if req.AmpduRTS != nil && *req.AmpduRTS != 0 && *req.AmpduRTS != 1 {
		return errors.New("ampdu_rts 仅支持 0 或 1")
	}
	if req.PhyRxdesens != nil && (*req.PhyRxdesens < 0 || *req.PhyRxdesens > 30) {
		return errors.New("phy_rxdesens 范围应为 0-30")
	}
	if req.Wl1TxPwrPct != nil && (*req.Wl1TxPwrPct < 10 || *req.Wl1TxPwrPct > 100) {
		return errors.New("5G 发射功率范围应为 10-100%")
	}
	if req.Wl0TxPwrPct != nil && (*req.Wl0TxPwrPct < 10 || *req.Wl0TxPwrPct > 100) {
		return errors.New("2.4G 发射功率范围应为 10-100%")
	}
	if req.Wl1TWT != nil && *req.Wl1TWT != 0 && *req.Wl1TWT != 1 {
		return errors.New("TWT 仅支持 0 或 1")
	}
	if req.Wl1ATF != nil && *req.Wl1ATF != 0 && *req.Wl1ATF != 1 {
		return errors.New("ATF 仅支持 0 或 1")
	}
	if req.Wl1APSD != nil && *req.Wl1APSD != 0 && *req.Wl1APSD != 1 {
		return errors.New("WME APSD 仅支持 0 或 1")
	}
	if req.Wl0HE != nil && *req.Wl0HE != 0 && *req.Wl0HE != 1 {
		return errors.New("2.4G HE 仅支持 0 或 1")
	}
	if req.Wl0WidthMHz != nil && *req.Wl0WidthMHz != 20 && *req.Wl0WidthMHz != 40 {
		return errors.New("2.4G 频宽仅支持 20 或 40 MHz")
	}

	needWl0Reload := false
	wl0Enabled := false
	if err := s.updateRFConfig(func(c *PanelConfig) {
		if req.Frameburst != nil {
			c.Tune.Frameburst = *req.Frameburst
		}
		if req.AmpduRTS != nil {
			c.Tune.AmpduRTS = *req.AmpduRTS
		}
		if req.PhyRxdesens != nil {
			c.Tune.PhyRxdesens = *req.PhyRxdesens
		}
		r1 := c.Radios["wl1"]
		if req.Wl1TxPwrPct != nil {
			r1.TxPwrPercent = *req.Wl1TxPwrPct
		}
		if req.Wl1TWT != nil {
			r1.TWT = *req.Wl1TWT == 1
		}
		if req.Wl1ATF != nil {
			r1.ATF = *req.Wl1ATF == 1
		}
		if req.Wl1APSD != nil {
			r1.APSD = *req.Wl1APSD == 1
		}
		c.Radios["wl1"] = r1

		r0 := c.Radios["wl0"]
		wl0Enabled = r0.Enabled
		if req.Wl0TxPwrPct != nil {
			r0.TxPwrPercent = *req.Wl0TxPwrPct
		}
		if req.Wl0HE != nil && r0.HEEnabled != (*req.Wl0HE == 1) {
			r0.HEEnabled = *req.Wl0HE == 1
			needWl0Reload = true
		}
		if req.Wl0WidthMHz != nil && r0.WidthMHz != *req.Wl0WidthMHz {
			r0.WidthMHz = *req.Wl0WidthMHz
			needWl0Reload = true
		}
		c.Radios["wl0"] = r0
	}); err != nil {
		return err
	}
	if needWl0Reload && wl0Enabled {
		_ = s.reloadRadioHostapd("wl0")
	}
	s.applyBaselineTune()
	return nil
}
