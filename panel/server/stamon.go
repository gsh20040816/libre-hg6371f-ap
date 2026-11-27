package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	staMonMaxTargets = 4  // BCM68252 wl sta_monitor hardware AMT limit per radio
	staMonEventsMax  = 50 // keep last 50 arrival/departure transitions in RAM
)

var (
	reStaMonRow      = regexp.MustCompile(`(?i)^\s*-?\d+\s+([0-9a-f]{2}(?::[0-9a-f]{2}){5})\s+(-?\d+)dBm\b`)
	reStaMonCounters = regexp.MustCompile(`(?i)^\s*Counters\s*:\s*data\s+(\d+)\s*,\s*mngt\s+(\d+)\s*,\s*cntl\s+(\d+)`)
)

type staMonDumpEntry struct {
	RSSI  int
	Total uint64 // data + mngt + cntl frame counter
}

type StaMonState struct {
	MAC        string `json:"mac"`
	Label      string `json:"label"`
	Present    bool   `json:"present"`
	RSSI24G    int    `json:"rssi_24g"`
	RSSI5G     int    `json:"rssi_5g"`
	BestRSSI   int    `json:"best_rssi"`
	BestBand   string `json:"best_band"`
	LastSeenTs int64  `json:"last_seen_ts"`
	SeenCount  uint64 `json:"seen_count"`
}

type StaMonEvent struct {
	Ts      int64  `json:"ts"`
	MAC     string `json:"mac"`
	Label   string `json:"label"`
	Present bool   `json:"present"`
	RSSI    int    `json:"rssi"`
	Band    string `json:"band"`
	Detail  string `json:"detail"`
}

type staMonEngine struct {
	mu        sync.Mutex
	states    map[string]*StaMonState
	events    []StaMonEvent
	active    map[string]map[string]bool   // iface -> active MAC set currently loaded in driver
	enabled   map[string]bool              // iface -> driver enabled state
	prevTotal map[string]map[string]uint64 // iface -> mac -> previous (data+mngt+cntl) counter
	prevKnown map[string]map[string]bool   // iface -> mac -> whether baseline counter was seeded
	syncCh    chan struct{}
}

func newStaMonEngine() *staMonEngine {
	return &staMonEngine{
		states:    make(map[string]*StaMonState),
		active:    map[string]map[string]bool{"wl0": {}, "wl1": {}},
		enabled:   map[string]bool{"wl0": false, "wl1": false},
		prevTotal: map[string]map[string]uint64{"wl0": {}, "wl1": {}},
		prevKnown: map[string]map[string]bool{"wl0": {}, "wl1": {}},
		syncCh:    make(chan struct{}, 1),
	}
}

func (e *staMonEngine) triggerSync() {
	select {
	case e.syncCh <- struct{}{}:
	default:
	}
}

func staMonIfaces(band string) []string {
	switch band {
	case "wl0":
		return []string{"wl0"}
	case "wl1":
		return []string{"wl1"}
	default:
		return []string{"wl0", "wl1"}
	}
}

// parseStaMonDump parses `wl -i <iface> sta_monitor dump` output into upper-case MAC -> {RSSI, Total}.
// Broadcom wl.ko permanently caches the last historical RSSI even when a station has left and even
// after del/add or disable/enable, so callers MUST check whether Total (data+mngt+cntl) increased.
// Broadcom prints one aggregate Counters line after all station rows (or per-row in some builds);
// apply each Counters line to all preceding rows since the previous Counters line.
func parseStaMonDump(out string) map[string]staMonDumpEntry {
	res := make(map[string]staMonDumpEntry)
	var pending []string
	for _, line := range strings.Split(out, "\n") {
		if m := reStaMonRow.FindStringSubmatch(line); len(m) == 3 {
			mac := strings.ToUpper(m[1])
			rssi, err := strconv.Atoi(m[2])
			if err == nil {
				res[mac] = staMonDumpEntry{RSSI: rssi}
				pending = append(pending, mac)
			}
			continue
		}
		if len(pending) > 0 {
			if m := reStaMonCounters.FindStringSubmatch(line); len(m) == 4 {
				d, _ := strconv.ParseUint(m[1], 10, 64)
				mg, _ := strconv.ParseUint(m[2], 10, 64)
				ct, _ := strconv.ParseUint(m[3], 10, 64)
				tot := d + mg + ct
				for _, mac := range pending {
					e := res[mac]
					e.Total = tot
					res[mac] = e
				}
				pending = pending[:0]
			}
		}
	}
	return res
}

func staMonDumpNeedsResync(dumpOut string, parsed map[string]staMonDumpEntry, wantMACs map[string]bool) bool {
	if !strings.Contains(dumpOut, "Status : Enabled") {
		return true
	}
	if len(parsed) != len(wantMACs) {
		return true
	}
	for mac := range wantMACs {
		if _, ok := parsed[mac]; !ok {
			return true
		}
	}
	return false
}

func (s *Server) syncStaMonDriver(cfg StaMonConfig) {
	eng := s.stamon
	wantMACs := make(map[string]bool, len(cfg.Targets))
	for _, t := range cfg.Targets {
		mac := strings.ToUpper(strings.TrimSpace(t.MAC))
		if reMAC.MatchString(mac) {
			wantMACs[mac] = true
		}
	}
	wantIfaces := make(map[string]bool)
	if cfg.Enabled && len(wantMACs) > 0 {
		for _, ifc := range staMonIfaces(cfg.Band) {
			wantIfaces[ifc] = true
		}
	}

	eng.mu.Lock()
	defer eng.mu.Unlock()

	for _, ifc := range []string{"wl0", "wl1"} {
		curSet := eng.active[ifc]
		if !wantIfaces[ifc] {
			if eng.enabled[ifc] || len(curSet) > 0 {
				for mac := range curSet {
					_ = runCmdOut(2*time.Second, "wl", "-i", ifc, "sta_monitor", "del", mac)
				}
				_ = runCmdOut(2*time.Second, "wl", "-i", ifc, "sta_monitor", "disable")
				eng.active[ifc] = make(map[string]bool)
				eng.prevTotal[ifc] = make(map[string]uint64)
				eng.prevKnown[ifc] = make(map[string]bool)
				eng.enabled[ifc] = false
			}
			continue
		}

		// Radio not up yet (e.g. during early boot or radio restart): keep unarmed
		// so the next 10s poll automatically reconciles once hostapd brings it up.
		if strings.TrimSpace(runCmdOut(1*time.Second, "wl", "-i", ifc, "isup")) != "1" {
			eng.active[ifc] = make(map[string]bool)
			eng.enabled[ifc] = false
			continue
		}

		// Reconcile against actual live driver state (handles boot ordering and hostapd restarts).
		driverMACs := make(map[string]bool)
		for _, line := range strings.Split(runCmdOut(2*time.Second, "wl", "-i", ifc, "sta_monitor"), "\n") {
			m := strings.ToUpper(strings.TrimSpace(line))
			if reMAC.MatchString(m) {
				driverMACs[m] = true
			}
		}
		curSet = driverMACs
		eng.active[ifc] = curSet

		statusOut := runCmdOut(2*time.Second, "wl", "-i", ifc, "sta_monitor", "status")
		if !strings.Contains(statusOut, "Enabled") {
			_ = runCmd(2*time.Second, "wl", "-i", ifc, "sta_monitor", "enable")
			statusOut = runCmdOut(2*time.Second, "wl", "-i", ifc, "sta_monitor", "status")
		}
		eng.enabled[ifc] = strings.Contains(statusOut, "Enabled")
		if !eng.enabled[ifc] {
			continue
		}

		// Remove unwanted MACs first so AMT slots (max 4) are freed before adding
		for mac := range curSet {
			if !wantMACs[mac] {
				_ = runCmd(2*time.Second, "wl", "-i", ifc, "sta_monitor", "del", mac)
				delete(curSet, mac)
				delete(eng.prevTotal[ifc], mac)
				delete(eng.prevKnown[ifc], mac)
			}
		}
		for mac := range wantMACs {
			if !curSet[mac] {
				out := strings.TrimSpace(runCmd(2*time.Second, "wl", "-i", ifc, "sta_monitor", "add", mac))
				if out != "" {
					log.Printf("stamon: add %s on %s failed: %s", mac, ifc, out)
				} else {
					curSet[mac] = true
				}
			}
		}
	}

	// Prune stale states & sync labels
	for mac := range eng.states {
		if !wantMACs[mac] {
			delete(eng.states, mac)
		}
	}
	for _, t := range cfg.Targets {
		mac := strings.ToUpper(strings.TrimSpace(t.MAC))
		if !reMAC.MatchString(mac) {
			continue
		}
		lbl := strings.TrimSpace(t.Label)
		if lbl == "" {
			if n := s.displayName(mac); n != "" {
				lbl = n
			} else {
				lbl = mac
			}
		}
		st, ok := eng.states[mac]
		if !ok {
			st = &StaMonState{MAC: mac, Label: lbl}
			eng.states[mac] = st
		} else {
			st.Label = lbl
		}
	}
}

func (s *Server) pollStaMonOnce(cfg StaMonConfig) {
	if !cfg.Enabled || len(cfg.Targets) == 0 {
		return
	}
	wantMACs := make(map[string]bool, len(cfg.Targets))
	for _, t := range cfg.Targets {
		mac := strings.ToUpper(strings.TrimSpace(t.MAC))
		if reMAC.MatchString(mac) {
			wantMACs[mac] = true
		}
	}
	if len(wantMACs) == 0 {
		return
	}
	ifaces := staMonIfaces(cfg.Band)
	now := time.Now()
	nowUnix := now.Unix()

	// Dump active interfaces (no del/add thrashing — Broadcom caches stale RSSI across del/add,
	// so freshness is strictly determined by per-STA packet counter increments).
	// If driver state was cleared (boot race or hostapd restart) while the radio is up,
	// self-heal via syncStaMonDriver.
	dumps := make(map[string]map[string]staMonDumpEntry, len(ifaces))
	assocSets := make(map[string]map[string]bool, len(ifaces))
	needResync := false
	for _, ifc := range ifaces {
		dumpOut := runCmdOut(2*time.Second, "wl", "-i", ifc, "sta_monitor", "dump")
		parsed := parseStaMonDump(dumpOut)
		if staMonDumpNeedsResync(dumpOut, parsed, wantMACs) &&
			strings.TrimSpace(runCmdOut(1*time.Second, "wl", "-i", ifc, "isup")) == "1" {
			needResync = true
		}
		dumps[ifc] = parsed
		assoc := make(map[string]bool)
		for _, line := range strings.Split(runCmdOut(1*time.Second, "wl", "-i", ifc, "assoclist"), "\n") {
			f := strings.Fields(line)
			if len(f) >= 2 && reMAC.MatchString(f[1]) {
				assoc[strings.ToUpper(f[1])] = true
			}
		}
		assocSets[ifc] = assoc
	}
	if needResync {
		s.syncStaMonDriver(cfg)
		for _, ifc := range ifaces {
			dumps[ifc] = parseStaMonDump(runCmdOut(2*time.Second, "wl", "-i", ifc, "sta_monitor", "dump"))
		}
	}

	awaySec := int64(cfg.AwaySec)
	if awaySec < 30 {
		awaySec = 180
	}
	rssiTh := cfg.RSSIThresh
	if rssiTh >= 0 || rssiTh < -95 {
		rssiTh = -75
	}

	var hooks []map[string]any
	anyChanged := false
	eng := s.stamon
	eng.mu.Lock()
	for _, t := range cfg.Targets {
		mac := strings.ToUpper(strings.TrimSpace(t.MAC))
		st, ok := eng.states[mac]
		if !ok {
			continue
		}

		checkFreshBand := func(ifc string) (int, bool) {
			e, exists := dumps[ifc][mac]
			if !exists {
				return 0, false
			}
			known := eng.prevKnown[ifc][mac]
			prev := eng.prevTotal[ifc][mac]
			eng.prevKnown[ifc][mac] = true
			eng.prevTotal[ifc][mac] = e.Total
			if !known {
				// On first poll after restart/add, only trust cached RSSI if the STA is
				// actually associated right now; otherwise wait for a real counter increment.
				if assocSets[ifc][mac] && e.RSSI < 0 {
					return e.RSSI, true
				}
				return 0, false
			}
			if (e.Total > prev || (e.Total > 0 && e.Total < prev)) && e.RSSI < 0 {
				return e.RSSI, true
			}
			return 0, false
		}

		r2, ok2 := checkFreshBand("wl0")
		r5, ok5 := checkFreshBand("wl1")
		if ok2 {
			st.RSSI24G = r2
		}
		if ok5 {
			st.RSSI5G = r5
		}
		if ok2 || ok5 {
			st.LastSeenTs = nowUnix
			st.SeenCount++
			switch {
			case ok5 && (!ok2 || r5 >= r2):
				st.BestRSSI = r5
				st.BestBand = "5G"
			case ok2:
				st.BestRSSI = r2
				st.BestBand = "2.4G"
			}
		}

		fresh := st.LastSeenTs > 0 && (nowUnix-st.LastSeenTs) <= awaySec
		if !fresh {
			st.RSSI24G = 0
			st.RSSI5G = 0
			st.BestRSSI = 0
			st.BestBand = ""
		}
		nextPresent := fresh && st.BestRSSI < 0 && st.BestRSSI >= rssiTh
		if nextPresent != st.Present {
			anyChanged = true
			st.Present = nextPresent
			detail := fmt.Sprintf("信号 %d dBm (%s) ≥ 阈值 %d dBm", st.BestRSSI, st.BestBand, rssiTh)
			if !nextPresent {
				if !fresh {
					detail = fmt.Sprintf("超过 %d 秒未捕获无线帧", awaySec)
				} else {
					detail = fmt.Sprintf("信号减弱至 %d dBm (< %d dBm)", st.BestRSSI, rssiTh)
				}
			}
			ev := StaMonEvent{
				Ts:      nowUnix,
				MAC:     mac,
				Label:   st.Label,
				Present: nextPresent,
				RSSI:    st.BestRSSI,
				Band:    st.BestBand,
				Detail:  detail,
			}
			eng.events = append(eng.events, ev)
			if len(eng.events) > staMonEventsMax {
				eng.events = append([]StaMonEvent(nil), eng.events[len(eng.events)-staMonEventsMax:]...)
			}
			if strings.HasPrefix(cfg.WebhookURL, "http://") || strings.HasPrefix(cfg.WebhookURL, "https://") {
				stateStr := "away"
				if nextPresent {
					stateStr = "present"
				}
				hooks = append(hooks, map[string]any{
					"event":   "stamon_presence",
					"state":   stateStr,
					"present": nextPresent,
					"mac":     mac,
					"label":   st.Label,
					"rssi":    st.BestRSSI,
					"band":    st.BestBand,
					"detail":  detail,
					"ts":      nowUnix,
				})
			}
		}
	}
	eng.mu.Unlock()

	if anyChanged {
		s.notifyMQTTStaMonEvent()
	}
	for _, payload := range hooks {
		go postRadarWebhook(cfg.WebhookURL, payload)
	}
}

func (s *Server) startStaMonitor() {
	if s.startupDone != nil {
		<-s.startupDone
	}
	s.syncStaMonDriver(s.getConfig().StaMon)

	tk := time.NewTicker(10 * time.Second)
	defer tk.Stop()
	for {
		select {
		case <-s.stamon.syncCh:
			cfg := s.getConfig().StaMon
			s.syncStaMonDriver(cfg)
			if cfg.Enabled && len(cfg.Targets) > 0 {
				time.Sleep(1200 * time.Millisecond)
				s.pollStaMonOnce(cfg)
			}
		case <-tk.C:
			cfg := s.getConfig().StaMon
			if cfg.Enabled && len(cfg.Targets) > 0 {
				s.pollStaMonOnce(cfg)
			}
		}
	}
}

func (s *Server) handleStaMon(w http.ResponseWriter, r *http.Request) {
	cfg := s.getConfig().StaMon
	eng := s.stamon
	eng.mu.Lock()
	targets := make([]StaMonState, 0, len(cfg.Targets))
	presentCnt := 0
	for _, t := range cfg.Targets {
		mac := strings.ToUpper(strings.TrimSpace(t.MAC))
		if st, ok := eng.states[mac]; ok {
			cp := *st
			if cp.Present {
				presentCnt++
			}
			targets = append(targets, cp)
		} else {
			lbl := strings.TrimSpace(t.Label)
			if lbl == "" {
				lbl = mac
			}
			targets = append(targets, StaMonState{MAC: mac, Label: lbl})
		}
	}
	events := make([]StaMonEvent, len(eng.events))
	copy(events, eng.events)
	eng.mu.Unlock()

	writeJSON(w, r, http.StatusOK, map[string]any{
		"config":        cfg,
		"max_targets":   staMonMaxTargets,
		"present_count": presentCnt,
		"targets":       targets,
		"events":        events,
		"candidates":    s.listRadarCandidates(),
	})
}

func (s *Server) handleStaMonConfig(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req StaMonConfig
	if err := json.NewDecoder(io.LimitReader(r.Body, 16384)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	if req.Band != "both" && req.Band != "wl0" && req.Band != "wl1" {
		req.Band = "both"
	}
	if req.RSSIThresh >= 0 || req.RSSIThresh < -95 {
		req.RSSIThresh = -75
	}
	if req.AwaySec < 30 || req.AwaySec > 3600 {
		req.AwaySec = 180
	}
	req.WebhookURL = strings.TrimSpace(req.WebhookURL)
	if req.WebhookURL != "" && !strings.HasPrefix(req.WebhookURL, "http://") && !strings.HasPrefix(req.WebhookURL, "https://") {
		writeErr(w, r, http.StatusBadRequest, "Webhook 仅支持 http:// 或 https://")
		return
	}

	seen := make(map[string]bool)
	cleaned := make([]StaMonTarget, 0, len(req.Targets))
	for _, t := range req.Targets {
		mac := strings.ToUpper(strings.TrimSpace(t.MAC))
		if mac == "" {
			continue
		}
		if !reMAC.MatchString(mac) {
			writeErr(w, r, http.StatusBadRequest, fmt.Sprintf("MAC 地址格式无效：%s", mac))
			return
		}
		if seen[mac] {
			continue
		}
		seen[mac] = true
		lbl := strings.TrimSpace(t.Label)
		if len(lbl) > 48 {
			lbl = lbl[:48]
		}
		cleaned = append(cleaned, StaMonTarget{MAC: mac, Label: lbl})
	}
	if len(cleaned) > staMonMaxTargets {
		writeErr(w, r, http.StatusBadRequest, fmt.Sprintf("受硬件 AMT 槽位限制，最多同时监听 %d 个 MAC", staMonMaxTargets))
		return
	}
	req.Targets = cleaned

	if err := s.updateConfig(func(c *PanelConfig) { c.StaMon = req }); err != nil {
		writeErr(w, r, http.StatusInternalServerError, err.Error())
		return
	}
	s.stamon.triggerSync()
	s.notifyMQTTPush()
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "config": req})
}

func (s *Server) handleStaMonTestWebhook(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		URL string `json:"url"`
	}
	_ = json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req)
	url := strings.TrimSpace(req.URL)
	if url == "" {
		url = strings.TrimSpace(s.getConfig().StaMon.WebhookURL)
	}
	if !strings.HasPrefix(url, "http://") && !strings.HasPrefix(url, "https://") {
		writeErr(w, r, http.StatusBadRequest, "请先填写有效的 http:// 或 https:// Webhook 地址")
		return
	}
	payload := map[string]any{
		"event":   "stamon_test",
		"state":   "present",
		"present": true,
		"mac":     "00:00:00:00:00:00",
		"label":   "测试设备",
		"rssi":    -52,
		"band":    "5G",
		"detail":  "HG6371F STA Monitor 测试推送",
		"ts":      time.Now().Unix(),
	}
	b, _ := json.Marshal(payload)
	cli := &http.Client{Timeout: 6 * time.Second}
	resp, err := cli.Post(url, "application/json", bytes.NewReader(b))
	if err != nil {
		writeJSON(w, r, http.StatusOK, map[string]any{"ok": false, "message": err.Error()})
		return
	}
	defer resp.Body.Close()
	_, _ = io.Copy(io.Discard, resp.Body)
	ok := resp.StatusCode >= 200 && resp.StatusCode < 300
	writeJSON(w, r, http.StatusOK, map[string]any{
		"ok":      ok,
		"status":  resp.StatusCode,
		"message": fmt.Sprintf("HTTP %d", resp.StatusCode),
	})
}
