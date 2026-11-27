package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

type TrafficSample struct {
	TS    int64   `json:"ts"`
	LanRx float64 `json:"lan_rx"`
	LanTx float64 `json:"lan_tx"`
	Wl1Rx float64 `json:"wl1_rx"`
	Wl1Tx float64 `json:"wl1_tx"`
	Wl0Rx float64 `json:"wl0_rx"`
	Wl0Tx float64 `json:"wl0_tx"`
}

type EthPortInfo struct {
	Iface     string `json:"iface"`
	Role      string `json:"role"`
	Link      bool   `json:"link"`
	Speed     string `json:"speed"`
	Duplex    string `json:"duplex"`
	MAC       string `json:"mac"`
	RxBytes   uint64 `json:"rx_bytes"`
	TxBytes   uint64 `json:"tx_bytes"`
	RxPackets uint64 `json:"rx_packets"`
	TxPackets uint64 `json:"tx_packets"`
	RxErrors  uint64 `json:"rx_errors"`
	TxErrors  uint64 `json:"tx_errors"`
}

type SlotStatus struct {
	Booted       int `json:"booted"`
	Reboot       int `json:"reboot"`
	Committed    int `json:"committed"`
	FirstCommit  int `json:"first_commit"`
	SecondCommit int `json:"second_commit"`
}

func runCmd(timeout time.Duration, name string, args ...string) string {
	if out, ok := bcmTryExec(name, args); ok {
		return out
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, name, args...)
	cmd.Env = []string{"PATH=" + defaultExecPath}
	out, _ := cmd.CombinedOutput()
	return string(out)
}

func runCmdOut(timeout time.Duration, name string, args ...string) string {
	if out, ok := bcmTryExec(name, args); ok {
		return out
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, name, args...)
	cmd.Env = []string{"PATH=" + defaultExecPath}
	out, _ := cmd.Output()
	return string(out)
}

func readTrim(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

func (s *Server) recordMACIP(mac, ip string) {
	mac = strings.ToUpper(strings.TrimSpace(mac))
	if mac == "" || ip == "" {
		return
	}
	s.arpMu.Lock()
	changed := s.macToIP[mac] != ip
	s.macToIP[mac] = ip
	s.arpMu.Unlock()
	if changed {
		s.markHostCacheDirty()
	}
}

func (s *Server) lookupIP(mac string) string {
	s.arpMu.RLock()
	defer s.arpMu.RUnlock()
	return s.macToIP[strings.ToUpper(mac)]
}

func (s *Server) lookupMACByIP(ip string) string {
	ip = strings.TrimSpace(strings.TrimPrefix(ip, "::ffff:"))
	if ip == "" {
		return ""
	}
	find := func() string {
		s.arpMu.RLock()
		defer s.arpMu.RUnlock()
		for mac, curIP := range s.macToIP {
			if curIP == ip {
				return mac
			}
		}
		return ""
	}
	if mac := find(); mac != "" {
		return mac
	}
	s.refreshARPTable()
	if mac := find(); mac != "" {
		return mac
	}
	// Fallback for IPv6 callers or fresh neighbor entries via `ip neigh`
	out := runCmdOut(1*time.Second, "ip", "neigh", "show", ip)
	for _, line := range strings.Split(out, "\n") {
		fields := strings.Fields(line)
		for i := 0; i+1 < len(fields); i++ {
			if fields[i] == "lladdr" && reMAC.MatchString(fields[i+1]) {
				return strings.ToUpper(fields[i+1])
			}
		}
	}
	return ""
}

func (s *Server) refreshARPTable() {
	b, err := os.ReadFile("/proc/net/arp")
	if err != nil {
		return
	}
	sc := bufio.NewScanner(strings.NewReader(string(b)))
	first := true
	for sc.Scan() {
		if first {
			first = false
			continue
		}
		f := strings.Fields(sc.Text())
		if len(f) >= 4 && reMAC.MatchString(f[3]) && f[3] != "00:00:00:00:00:00" {
			s.recordMACIP(f[3], f[0])
		}
	}
}

func (s *Server) loadNotes() {
	b, err := os.ReadFile(s.notesFile)
	if err != nil {
		return
	}
	m := make(map[string]string)
	if json.Unmarshal(b, &m) == nil {
		s.notesMu.Lock()
		for k, v := range m {
			s.clientNotes[strings.ToUpper(k)] = v
		}
		s.notesMu.Unlock()
	}
}

func (s *Server) setNote(mac, note string) error {
	s.notesMu.Lock()
	if note == "" {
		delete(s.clientNotes, mac)
	} else {
		s.clientNotes[mac] = note
	}
	b, err := json.MarshalIndent(s.clientNotes, "", "  ")
	s.notesMu.Unlock()
	if err != nil {
		return err
	}
	return os.WriteFile(s.notesFile, b, 0600)
}

func (s *Server) getNote(mac string) string {
	s.notesMu.RLock()
	defer s.notesMu.RUnlock()
	return s.clientNotes[strings.ToUpper(mac)]
}

func (s *Server) sampleCPU() float64 {
	b, err := os.ReadFile("/proc/stat")
	if err != nil {
		return 0
	}
	line := strings.SplitN(string(b), "\n", 2)[0]
	fields := strings.Fields(line)
	if len(fields) < 5 || fields[0] != "cpu" {
		return 0
	}
	var tot, idl uint64
	for i := 1; i < len(fields); i++ {
		v, _ := strconv.ParseUint(fields[i], 10, 64)
		tot += v
		if i == 4 || i == 5 {
			idl += v
		}
	}
	s.cpuMu.Lock()
	defer s.cpuMu.Unlock()
	if s.prevTot > 0 && tot > s.prevTot {
		dt := float64(tot - s.prevTot)
		di := float64(idl - s.prevIdl)
		pct := (1.0 - di/dt) * 100.0
		if pct < 0 {
			pct = 0
		}
		if pct > 100 {
			pct = 100
		}
		s.lastCPU = math.Round(pct*10) / 10
	}
	s.prevTot = tot
	s.prevIdl = idl
	return s.lastCPU
}

func readNetDevBytes() map[string][2]uint64 {
	res := make(map[string][2]uint64)
	b, err := os.ReadFile("/proc/net/dev")
	if err != nil {
		return res
	}
	sc := bufio.NewScanner(strings.NewReader(string(b)))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		idx := strings.IndexByte(line, ':')
		if idx <= 0 {
			continue
		}
		iface := strings.TrimSpace(line[:idx])
		f := strings.Fields(line[idx+1:])
		if len(f) < 9 {
			continue
		}
		rx, _ := strconv.ParseUint(f[0], 10, 64)
		tx, _ := strconv.ParseUint(f[8], 10, 64)
		res[iface] = [2]uint64{rx, tx}
	}
	return res
}

func (s *Server) sampleTraffic() {
	now := time.Now()
	cur := readNetDevBytes()

	s.trafMu.Lock()
	defer s.trafMu.Unlock()

	if !s.trafAt.IsZero() && s.trafPrev != nil {
		dt := now.Sub(s.trafAt).Seconds()
		if dt >= 0.5 {
			mbps := func(iface string, idx int) float64 {
				c, ok1 := cur[iface]
				p, ok2 := s.trafPrev[iface]
				if !ok1 || !ok2 || c[idx] < p[idx] {
					return 0
				}
				v := float64(c[idx]-p[idx]) * 8.0 / dt / 1e6
				return math.Round(v*100) / 100
			}
			sample := TrafficSample{
				TS:    now.Unix(),
				LanRx: mbps("lan0", 0),
				LanTx: mbps("lan0", 1),
				Wl1Rx: mbps("wl1", 0),
				Wl1Tx: mbps("wl1", 1),
				Wl0Rx: mbps("wl0", 0),
				Wl0Tx: mbps("wl0", 1),
			}
			s.trafHist = append(s.trafHist, sample)
			if len(s.trafHist) > 60 {
				s.trafHist = s.trafHist[len(s.trafHist)-60:]
			}
		}
	}
	s.trafAt = now
	s.trafPrev = cur
}

func (s *Server) getTrafficHistory() []TrafficSample {
	s.trafMu.Lock()
	defer s.trafMu.Unlock()
	out := make([]TrafficSample, len(s.trafHist))
	copy(out, s.trafHist)
	return out
}

func readMemMB() (total, used, avail int) {
	b, err := os.ReadFile("/proc/meminfo")
	if err != nil {
		return 0, 0, 0
	}
	var tKB, aKB, fKB, bufKB, cacheKB int
	sc := bufio.NewScanner(strings.NewReader(string(b)))
	for sc.Scan() {
		f := strings.Fields(sc.Text())
		if len(f) < 2 {
			continue
		}
		v, _ := strconv.Atoi(f[1])
		switch f[0] {
		case "MemTotal:":
			tKB = v
		case "MemAvailable:":
			aKB = v
		case "MemFree:":
			fKB = v
		case "Buffers:":
			bufKB = v
		case "Cached:":
			cacheKB = v
		}
	}
	if aKB == 0 {
		aKB = fKB + bufKB + cacheKB
	}
	total = tKB / 1024
	avail = aKB / 1024
	used = (tKB - aKB) / 1024
	if used < 0 {
		used = 0
	}
	return
}

func countProcs() int {
	ents, err := os.ReadDir("/proc")
	if err != nil {
		return 0
	}
	n := 0
	for _, e := range ents {
		if e.IsDir() {
			name := e.Name()
			if len(name) > 0 && name[0] >= '0' && name[0] <= '9' {
				n++
			}
		}
	}
	return n
}

var (
	reFlowActive = regexp.MustCompile(`Flow Ucast Learning\s+\w+\s*:\s*Max<\d+>,\s*Active<(\d+)>,\s*Cumulative\s*\[\s*(\d+)\s*-\s*(\d+)\s*\]`)
	reSpeed      = regexp.MustCompile(`Speed:\s*(\S+)`)
	reDuplex     = regexp.MustCompile(`Duplex:\s*(\S+)`)
	reLink       = regexp.MustCompile(`Link detected:\s*(\S+)`)
	reIPv4       = regexp.MustCompile(`inet\s+([0-9.]+/\d+)`)
)

func collectEthPort(iface, role string) EthPortInfo {
	p := EthPortInfo{
		Iface: iface,
		Role:  role,
		Speed: "未连接",
		MAC:   strings.ToUpper(readTrim("/sys/class/net/" + iface + "/address")),
	}
	et := runCmdOut(2*time.Second, "ethtool", iface)
	if m := reLink.FindStringSubmatch(et); len(m) == 2 {
		p.Link = strings.EqualFold(m[1], "yes")
	}
	if p.Link {
		if m := reSpeed.FindStringSubmatch(et); len(m) == 2 {
			p.Speed = m[1]
		}
		if m := reDuplex.FindStringSubmatch(et); len(m) == 2 {
			p.Duplex = m[1]
		}
	}
	stats := runCmdOut(2*time.Second, "ethtool", "-S", iface)
	for _, line := range strings.Split(stats, "\n") {
		k, v, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		k = strings.TrimSpace(k)
		n, _ := strconv.ParseUint(strings.TrimSpace(v), 10, 64)
		switch k {
		case "RxOctetsOK":
			p.RxBytes = n
		case "TxOctetsOK":
			p.TxBytes = n
		case "RxPacketsOK":
			p.RxPackets = n
		case "TxPacketsOK":
			p.TxPackets = n
		case "RxErrors":
			p.RxErrors = n
		case "TxErrors":
			p.TxErrors = n
		}
	}
	return p
}

func parseBootstate(out string) SlotStatus {
	var st SlotStatus
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimSpace(line)
		if strings.Contains(line, "Booted Partition:") {
			if strings.Contains(line, "Second") {
				st.Booted = 2
			} else if strings.Contains(line, "First") {
				st.Booted = 1
			}
		} else if strings.Contains(line, "Reboot Partition:") {
			if strings.Contains(line, "Second") {
				st.Reboot = 2
			} else if strings.Contains(line, "First") {
				st.Reboot = 1
			}
		} else if strings.Contains(line, "First  partition commit flag") {
			if strings.HasSuffix(line, "1") {
				st.FirstCommit = 1
			}
		} else if strings.Contains(line, "Second partition commit flag") {
			if strings.HasSuffix(line, "1") {
				st.SecondCommit = 1
			}
		} else if strings.Contains(line, "rd_metadata: committed ") {
			idx := strings.Index(line, "committed ")
			if idx >= 0 && len(line) > idx+10 {
				if line[idx+10] == '1' {
					st.Committed = 1
				} else if line[idx+10] == '2' {
					st.Committed = 2
				}
			}
		}
	}
	if st.Committed == 0 {
		if st.FirstCommit == 1 {
			st.Committed = 1
		} else if st.SecondCommit == 1 {
			st.Committed = 2
		}
	}
	return st
}

func (s *Server) getBootstateCached(force bool) SlotStatus {
	s.slotMu.Lock()
	if !force && s.slotCached {
		st := s.slotCache
		s.slotMu.Unlock()
		return st
	}
	s.slotMu.Unlock()

	st := parseBootstate(runCmd(3*time.Second, "bcm_bootstate"))
	s.slotMu.Lock()
	s.slotCache = st
	s.slotCached = true
	s.slotMu.Unlock()
	return st
}

func (s *Server) invalidateOverviewCache() {
	s.ovMu.Lock()
	s.ovCacheAt = time.Time{}
	s.ovMu.Unlock()
}

func (s *Server) collectOverview() map[string]any {
	s.ovMu.Lock()
	if s.ovCache != nil && time.Since(s.ovCacheAt) < 8*time.Second {
		cached := s.ovCache
		s.ovMu.Unlock()
		return cached
	}
	s.ovMu.Unlock()

	upFields := strings.Fields(readTrim("/proc/uptime"))
	upSec := 0
	if len(upFields) > 0 {
		if f, err := strconv.ParseFloat(upFields[0], 64); err == nil {
			upSec = int(f)
		}
	}
	loadFields := strings.Fields(readTrim("/proc/loadavg"))
	loadStr := ""
	if len(loadFields) >= 3 {
		loadStr = strings.Join(loadFields[:3], " ")
	}
	tempMilli, _ := strconv.Atoi(readTrim("/sys/class/thermal/thermal_zone0/temp"))
	tempC := math.Round(float64(tempMilli)/100.0) / 10.0

	memTotal, memUsed, memAvail := readMemMB()

	lan0 := collectEthPort("lan0", "2.5G 上联口")
	lan1 := collectEthPort("lan1", "1G 扩展口")

	brIP := ""
	ipOut := runCmdOut(2*time.Second, "ip", "-4", "addr", "show", "br0")
	if m := reIPv4.FindStringSubmatch(ipOut); len(m) == 2 {
		brIP = m[1]
	}
	brMAC := strings.ToUpper(readTrim("/sys/class/net/br0/address"))

	fc := runCmdOut(2*time.Second, "fcctl", "status")
	fcHW := strings.Contains(fc, "HW Acceleration <Enabled>")
	fcActive, fcCum := 0, 0
	if m := reFlowActive.FindStringSubmatch(fc); len(m) == 4 {
		fcActive, _ = strconv.Atoi(m[1])
		fcCum, _ = strconv.Atoi(m[2])
	}

	bs := s.getBootstateCached(false)

	s.cpuMu.Lock()
	cpuPct := s.lastCPU
	s.cpuMu.Unlock()

	cfg := s.getConfig()
	r1 := s.collectRadioSummary("wl1")
	r0 := s.collectRadioSummary("wl0")

	res := map[string]any{
		"hostname":       readTrim("/etc/hostname"),
		"alpine_version": readTrim("/etc/alpine-release"),
		"kernel":         s.kernelVer,
		"uptime_sec":     upSec,
		"load":           loadStr,
		"cpu_pct":        cpuPct,
		"temp_c":         tempC,
		"wl1_temp_c":     r1.Air.ChipTempC,
		"wl0_temp_c":     r0.Air.ChipTempC,
		"mem_total_mb":   memTotal,
		"mem_used_mb":    memUsed,
		"mem_avail_mb":   memAvail,
		"procs":          countProcs(),
		"uplink": map[string]any{
			"iface":  "lan0",
			"speed":  lan0.Speed,
			"duplex": lan0.Duplex,
			"link":   lan0.Link,
			"br_ip":  brIP,
			"br_mac": brMAC,
		},
		"ports": []EthPortInfo{lan0, lan1},
		"fcache": map[string]any{
			"hw_enabled":   fcHW,
			"active_flows": fcActive,
			"cumulative":   fcCum,
		},
		"radios":      []RadioSummary{r1, r0},
		"slots":       bs,
		"led":         s.getLEDState(),
		"led_mode":    cfg.LED.Mode,
		"led_cfg":     cfg.LED,
		"tls_enabled": s.isTLSActive(),
	}

	s.ovMu.Lock()
	s.ovCache = res
	s.ovCacheAt = time.Now()
	s.ovMu.Unlock()
	return res
}

func (s *Server) collectSystem() map[string]any {
	svcs := []string{"hostapd.wl1", "hostapd.wl0", "sshd", "chronyd", "haveged", "ap-led", "ap-panel"}
	svcList := make([]map[string]any, 0, len(svcs))
	for _, name := range svcs {
		out := strings.TrimSpace(runCmd(3*time.Second, "rc-service", name, "status"))
		status := "stopped"
		if strings.Contains(out, "started") {
			status = "started"
		} else if strings.Contains(out, "crashed") {
			status = "crashed"
		}
		svcList = append(svcList, map[string]any{
			"name":       name,
			"status":     status,
			"can_action": allowedServices[name],
		})
	}
	cfg := s.getConfig()
	return map[string]any{
		"slots":         parseBootstate(runCmd(3*time.Second, "bcm_bootstate")),
		"services":      svcList,
		"tls_enabled":   s.isTLSActive(),
		"tls_redirect":  cfg.TLSRedirect,
		"tls_cert":      s.tlsCertFile,
		"tls_key":       s.tlsKeyFile,
		"led":           cfg.LED,
		"led_state":     s.getLEDState(),
		"button":        cfg.Button,
		"wifi_schedule": cfg.WifiSchedule,
	}
}

func (s *Server) switchBootSlot(slot int, mode string) (SlotStatus, error) {
	if slot != 1 && slot != 2 {
		return SlotStatus{}, errors.New("槽位必须是 1 或 2")
	}
	var code string
	switch {
	case slot == 1 && mode == "commit":
		code = "5"
	case slot == 1 && mode == "once":
		code = "6"
	case slot == 2 && mode == "commit":
		code = "7"
	case slot == 2 && mode == "once":
		code = "8"
	default:
		return SlotStatus{}, errors.New("mode 必须是 once 或 commit")
	}
	if _, err := os.Stat("/dev/fhdrv_kdrv_board"); err == nil {
		wp := filepath.Join(s.baseDir, "bin", "fh-mtd-wp")
		_ = runCmd(5*time.Second, wp, "2", "0")
		defer func() { _ = runCmd(5*time.Second, wp, "2", "1") }()
	}
	_ = runCmd(10*time.Second, "bcm_bootstate", code)
	st := s.getBootstateCached(true)
	s.invalidateOverviewCache()
	if st.Reboot != slot || (mode == "commit" && st.Committed != slot) {
		return st, fmt.Errorf("切换未生效（当前 Reboot=%d Committed=%d，目标=%d mode=%s）", st.Reboot, st.Committed, slot, mode)
	}
	if mode == "commit" {
		_ = os.WriteFile("/fhconf/ap_alpine_committed", []byte("1\n"), 0644)
		_ = os.WriteFile("/tmp/ap-clean-ok", []byte("1\n"), 0644)
	}
	return st, nil
}

func (s *Server) readLogs(src string, maxLines int) []string {
	var raw string
	if src == "dmesg" {
		raw = runCmdOut(3*time.Second, "dmesg")
	} else {
		b, err := os.ReadFile("/var/log/messages")
		if err == nil {
			raw = string(b)
		}
	}
	lines := strings.Split(strings.TrimRight(raw, "\n"), "\n")
	if len(lines) == 1 && lines[0] == "" {
		return []string{}
	}
	if len(lines) > maxLines {
		lines = lines[len(lines)-maxLines:]
	}
	return lines
}
