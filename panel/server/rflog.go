package main

import (
	"bytes"
	"encoding/csv"
	"fmt"
	"math"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

type RFSample struct {
	TS         int64          `json:"ts"`
	W5Busy     int            `json:"w5_busy"`
	W5Obss     int            `json:"w5_obss"`
	W5Txop     int            `json:"w5_txop"`
	W5Noise    int            `json:"w5_noise"`
	W5Glitch   int            `json:"w5_glitch"`
	W5Retrans  float64        `json:"w5_retrans"`
	W5Temp     int            `json:"w5_temp"`
	W5Clients  int            `json:"w5_clients"`
	W2Busy     int            `json:"w2_busy"`
	W2Obss     int            `json:"w2_obss"`
	W2Txop     int            `json:"w2_txop"`
	W2Noise    int            `json:"w2_noise"`
	W2Glitch   int            `json:"w2_glitch"`
	W2Retrans  float64        `json:"w2_retrans"`
	W2Temp     int            `json:"w2_temp"`
	W2Clients  int            `json:"w2_clients"`
	SocTemp    float64        `json:"soc_temp"`
	ClientRSSI map[string]int `json:"client_rssi,omitempty"`
}

type RFEvent struct {
	TS     int64  `json:"ts"`
	Kind   string `json:"kind"` // "join" | "leave" | "band_switch"
	MAC    string `json:"mac"`
	Label  string `json:"label"`
	Band   string `json:"band"`
	Detail string `json:"detail"`
}

type trackedClient struct {
	MAC    string
	Band   string
	Iface  string
	RSSI   int
	SeenAt time.Time
}

type recentLeave struct {
	Band string
	At   time.Time
}

const (
	maxRFSamples   = 1440 // 24 hours @ 1 sample/min
	maxRFEvents    = 200
	hostapdCtrlDir = "/run/hostapd"
)

func (s *Server) startRFRecorder() {
	// Capture an initial sample and seed currently associated STAs once on boot.
	time.Sleep(3 * time.Second)
	s.seedInitialClients()
	s.captureRFSample()

	// Subscribe directly to hostapd UNIX datagram control sockets for real-time
	// AP-STA-CONNECTED / AP-STA-DISCONNECTED events (zero polling, catches <1s drops).
	for _, spec := range []struct {
		iface   string
		band    string
		isGuest bool
	}{
		{"wl1", "5G", false},
		{"wl0", "2.4G", false},
		{"wl1.1", "5G", true},
		{"wl0.1", "2.4G", true},
	} {
		go s.watchHostapdEvents(spec.iface, spec.band, spec.isGuest)
	}

	minTk := time.NewTicker(60 * time.Second)
	defer minTk.Stop()

	for range minTk.C {
		s.captureRFSample()
	}
}

func (s *Server) seedInitialClients() {
	cur := make(map[string]trackedClient)
	now := time.Now()
	for _, spec := range []struct {
		iface string
		band  string
	}{
		{"wl1", "5G"},
		{"wl0", "2.4G"},
		{"wl1.1", "5G"},
		{"wl0.1", "2.4G"},
	} {
		if _, err := os.Stat(filepath.Join("/sys/class/net", spec.iface)); err != nil {
			continue
		}
		assoc := runCmdOut(2*time.Second, "wl", "-i", spec.iface, "assoclist")
		for _, line := range strings.Split(assoc, "\n") {
			f := strings.Fields(line)
			if len(f) != 2 || f[0] != "assoclist" {
				continue
			}
			mac := strings.ToUpper(f[1])
			cur[mac] = trackedClient{MAC: mac, Band: spec.band, Iface: spec.iface, SeenAt: now}
		}
	}
	s.rfMu.Lock()
	s.rfClients = cur
	if s.rfRecentLeave == nil {
		s.rfRecentLeave = make(map[string]recentLeave)
	}
	s.rfMu.Unlock()
}

func (s *Server) watchHostapdEvents(iface, band string, isGuest bool) {
	remotePath := filepath.Join(hostapdCtrlDir, iface)
	localPath := fmt.Sprintf("/tmp/ap-hapd-%s.sock", iface)

	for {
		if _, err := os.Stat(remotePath); err != nil {
			time.Sleep(5 * time.Second)
			continue
		}
		s.runHostapdEventSession(iface, band, isGuest, remotePath, localPath)
		time.Sleep(2 * time.Second)
	}
}

func (s *Server) runHostapdEventSession(iface, band string, isGuest bool, remotePath, localPath string) {
	_ = os.Remove(localPath)
	defer os.Remove(localPath)

	laddr := &net.UnixAddr{Name: localPath, Net: "unixgram"}
	raddr := &net.UnixAddr{Name: remotePath, Net: "unixgram"}
	conn, err := net.DialUnix("unixgram", laddr, raddr)
	if err != nil {
		return
	}
	defer conn.Close()

	_ = conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	if _, err := conn.Write([]byte("ATTACH")); err != nil {
		return
	}
	buf := make([]byte, 2048)
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err := conn.Read(buf)
	if err != nil || !strings.HasPrefix(string(buf[:n]), "OK") {
		return
	}
	defer func() {
		_ = conn.SetWriteDeadline(time.Now().Add(1 * time.Second))
		_, _ = conn.Write([]byte("DETACH"))
	}()

	awaitingPong := false
	for {
		wait := 15 * time.Second
		if awaitingPong {
			wait = 3 * time.Second
		}
		_ = conn.SetReadDeadline(time.Now().Add(wait))
		n, err := conn.Read(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				if awaitingPong {
					// hostapd likely restarted and replaced socket inode; reconnect
					return
				}
				_ = conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
				if _, werr := conn.Write([]byte("PING")); werr != nil {
					return
				}
				awaitingPong = true
				continue
			}
			return
		}

		msg := strings.TrimSpace(string(buf[:n]))
		if msg == "PONG" {
			awaitingPong = false
			continue
		}
		if msg == "OK" || msg == "" {
			continue
		}
		// Strip optional "<N>" hostapd log-level prefix
		if strings.HasPrefix(msg, "<") {
			if idx := strings.IndexByte(msg, '>'); idx > 0 {
				msg = strings.TrimSpace(msg[idx+1:])
			}
		}
		f := strings.Fields(msg)
		if len(f) < 2 {
			continue
		}
		mac := strings.ToUpper(f[1])
		if !reMAC.MatchString(mac) {
			continue
		}
		switch f[0] {
		case "AP-STA-CONNECTED":
			s.handleHostapdSTAEvent(iface, band, isGuest, mac, true)
		case "AP-STA-DISCONNECTED":
			s.handleHostapdSTAEvent(iface, band, isGuest, mac, false)
		}
	}
}

func (s *Server) handleHostapdSTAEvent(iface, band string, isGuest bool, mac string, connected bool) {
	now := time.Now()
	label := s.displayName(mac)
	if label == "" {
		label = mac
	}
	bandLabel := band
	if isGuest {
		bandLabel = band + " 访客"
	}

	if connected {
		s.rfMu.Lock()
		if s.rfClients == nil {
			s.rfClients = make(map[string]trackedClient)
		}
		if s.rfRecentLeave == nil {
			s.rfRecentLeave = make(map[string]recentLeave)
		}
		old, existed := s.rfClients[mac]
		if existed && old.Iface == iface {
			old.SeenAt = now
			s.rfClients[mac] = old
			s.rfMu.Unlock()
			return
		}
		switchedFrom := ""
		if existed && old.Band != band {
			switchedFrom = old.Band
		} else if rl, ok := s.rfRecentLeave[mac]; ok {
			delete(s.rfRecentLeave, mac)
			if rl.Band != band && now.Sub(rl.At) <= 15*time.Second {
				switchedFrom = rl.Band
			}
		}
		s.rfClients[mac] = trackedClient{MAC: mac, Band: band, Iface: iface, SeenAt: now}
		s.rfMu.Unlock()

		if switchedFrom != "" {
			s.recordRFEvent(RFEvent{
				TS:     now.Unix(),
				Kind:   "band_switch",
				MAC:    mac,
				Label:  label,
				Band:   band,
				Detail: fmt.Sprintf("从 %s 切换至 %s", switchedFrom, bandLabel),
			})
			return
		}

		rssi := strings.TrimSpace(runCmdOut(1*time.Second, "wl", "-i", iface, "rssi", mac))
		detail := fmt.Sprintf("连入 %s", bandLabel)
		if rssi != "" && rssi != "0" {
			detail = fmt.Sprintf("连入 %s (%s dBm)", bandLabel, rssi)
		}
		s.recordRFEvent(RFEvent{
			TS:     now.Unix(),
			Kind:   "join",
			MAC:    mac,
			Label:  label,
			Band:   band,
			Detail: detail,
		})
		return
	}

	s.rfMu.Lock()
	if s.rfClients == nil {
		s.rfClients = make(map[string]trackedClient)
	}
	if s.rfRecentLeave == nil {
		s.rfRecentLeave = make(map[string]recentLeave)
	}
	cur, existed := s.rfClients[mac]
	if existed && cur.Iface != iface {
		// Already connected on another band/iface before this delayed disconnect arrived.
		s.rfMu.Unlock()
		return
	}
	if !existed {
		s.rfMu.Unlock()
		return
	}
	delete(s.rfClients, mac)
	s.rfRecentLeave[mac] = recentLeave{Band: band, At: now}
	s.rfMu.Unlock()

	s.recordRFEvent(RFEvent{
		TS:     now.Unix(),
		Kind:   "leave",
		MAC:    mac,
		Label:  label,
		Band:   band,
		Detail: fmt.Sprintf("从 %s 断开", bandLabel),
	})
}

func (s *Server) recordRFEvent(ev RFEvent) {
	s.rfMu.Lock()
	defer s.rfMu.Unlock()
	s.rfEvents = append(s.rfEvents, ev)
	if len(s.rfEvents) > maxRFEvents {
		s.rfEvents = s.rfEvents[len(s.rfEvents)-maxRFEvents:]
	}
}

func (s *Server) captureRFSample() RFSample {
	a5 := collectAirStats("wl1")
	a2 := collectAirStats("wl0")

	f5, r5 := readTxCounters("wl1")
	f2, r2 := readTxCounters("wl0")

	s.rfMu.Lock()
	ret5 := a5.TxRetrans
	if s.rfPrevValid && f5 > s.rfPrevF5 && r5 >= s.rfPrevR5 {
		df, dr := f5-s.rfPrevF5, r5-s.rfPrevR5
		if df >= 5 {
			ret5 = math.Round(float64(dr)*1000.0/float64(df+dr)) / 10.0
		}
	}
	ret2 := a2.TxRetrans
	if s.rfPrevValid && f2 > s.rfPrevF2 && r2 >= s.rfPrevR2 {
		df, dr := f2-s.rfPrevF2, r2-s.rfPrevR2
		if df >= 5 {
			ret2 = math.Round(float64(dr)*1000.0/float64(df+dr)) / 10.0
		}
	}
	s.rfPrevF5, s.rfPrevR5 = f5, r5
	s.rfPrevF2, s.rfPrevR2 = f2, r2
	s.rfPrevValid = true
	s.rfMu.Unlock()

	c5 := 0
	c2 := 0
	cRSSI := make(map[string]int)
	for _, pair := range [][2]string{{"wl1", "5G"}, {"wl0", "2.4G"}} {
		iface, band := pair[0], pair[1]
		assoc := runCmdOut(2*time.Second, "wl", "-i", iface, "assoclist")
		for _, line := range strings.Split(assoc, "\n") {
			f := strings.Fields(line)
			if len(f) != 2 || f[0] != "assoclist" {
				continue
			}
			mac := strings.ToUpper(f[1])
			if band == "5G" {
				c5++
			} else {
				c2++
			}
			if r, err := strconv.Atoi(strings.TrimSpace(runCmdOut(1*time.Second, "wl", "-i", iface, "rssi", mac))); err == nil && r < 0 {
				cRSSI[mac] = r
			}
		}
	}

	tempMilli, _ := strconv.Atoi(readTrim("/sys/class/thermal/thermal_zone0/temp"))
	socTemp := math.Round(float64(tempMilli)/100.0) / 10.0

	smp := RFSample{
		TS:         time.Now().Unix(),
		W5Busy:     a5.BusyPct,
		W5Obss:     a5.ObssPct,
		W5Txop:     a5.TxopPct,
		W5Noise:    a5.NoiseDBm,
		W5Glitch:   a5.Glitch,
		W5Retrans:  ret5,
		W5Temp:     a5.ChipTempC,
		W5Clients:  c5,
		W2Busy:     a2.BusyPct,
		W2Obss:     a2.ObssPct,
		W2Txop:     a2.TxopPct,
		W2Noise:    a2.NoiseDBm,
		W2Glitch:   a2.Glitch,
		W2Retrans:  ret2,
		W2Temp:     a2.ChipTempC,
		W2Clients:  c2,
		SocTemp:    socTemp,
		ClientRSSI: cRSSI,
	}

	s.rfMu.Lock()
	s.rfSamples = append(s.rfSamples, smp)
	if len(s.rfSamples) > maxRFSamples {
		s.rfSamples = s.rfSamples[len(s.rfSamples)-maxRFSamples:]
	}
	s.rfMu.Unlock()
	return smp
}

func (s *Server) getRFHistory(rangeStr string) map[string]any {
	dur := 6 * time.Hour
	switch rangeStr {
	case "1h":
		dur = 1 * time.Hour
	case "24h":
		dur = 24 * time.Hour
	}
	cutoff := time.Now().Add(-dur).Unix()

	s.rfMu.Lock()
	var filtered []RFSample
	for _, sm := range s.rfSamples {
		if sm.TS >= cutoff {
			filtered = append(filtered, sm)
		}
	}
	if len(filtered) == 0 && len(s.rfSamples) > 0 {
		filtered = append(filtered, s.rfSamples...)
	}
	evCopy := make([]RFEvent, len(s.rfEvents))
	copy(evCopy, s.rfEvents)
	totalStored := len(s.rfSamples)
	s.rfMu.Unlock()

	// Reverse events so newest appear first
	for i, j := 0, len(evCopy)-1; i < j; i, j = i+1, j-1 {
		evCopy[i], evCopy[j] = evCopy[j], evCopy[i]
	}

	// Build summary stats + client labels map
	var sum5Obss, max5Obss, sum5Noise, maxTemp int
	var sum2Obss, max2Obss int
	clientLabels := make(map[string]string)
	for _, sm := range filtered {
		sum5Obss += sm.W5Obss
		if sm.W5Obss > max5Obss {
			max5Obss = sm.W5Obss
		}
		sum5Noise += sm.W5Noise
		if sm.W5Temp > maxTemp {
			maxTemp = sm.W5Temp
		}
		sum2Obss += sm.W2Obss
		if sm.W2Obss > max2Obss {
			max2Obss = sm.W2Obss
		}
		for mac := range sm.ClientRSSI {
			if _, ok := clientLabels[mac]; !ok {
				n := s.displayName(mac)
				if n == "" {
					n = mac
				}
				clientLabels[mac] = n
			}
		}
	}
	avg5Obss, avg5Noise, avg2Obss := 0.0, 0.0, 0.0
	if n := len(filtered); n > 0 {
		avg5Obss = math.Round(float64(sum5Obss)*10/float64(n)) / 10
		avg5Noise = math.Round(float64(sum5Noise)*10/float64(n)) / 10
		avg2Obss = math.Round(float64(sum2Obss)*10/float64(n)) / 10
	}

	return map[string]any{
		"range":         rangeStr,
		"total_samples": totalStored,
		"samples":       filtered,
		"client_labels": clientLabels,
		"events":        evCopy,
		"stats": map[string]any{
			"avg_5g_obss":  avg5Obss,
			"max_5g_obss":  max5Obss,
			"avg_5g_noise": avg5Noise,
			"avg_24g_obss": avg2Obss,
			"max_24g_obss": max2Obss,
			"max_5g_temp":  maxTemp,
		},
	}
}

func (s *Server) exportRFCSV() []byte {
	s.rfMu.Lock()
	cp := make([]RFSample, len(s.rfSamples))
	copy(cp, s.rfSamples)
	s.rfMu.Unlock()

	var buf bytes.Buffer
	w := csv.NewWriter(&buf)
	_ = w.Write([]string{
		"time", "w5_busy_pct", "w5_obss_pct", "w5_txop_pct", "w5_noise_dbm", "w5_glitch", "w5_retrans_pct", "w5_temp_c", "w5_clients",
		"w2_busy_pct", "w2_obss_pct", "w2_txop_pct", "w2_noise_dbm", "w2_glitch", "w2_retrans_pct", "w2_temp_c", "w2_clients", "soc_temp_c",
	})
	for _, sm := range cp {
		_ = w.Write([]string{
			time.Unix(sm.TS, 0).Format("2006-01-02 15:04:05"),
			strconv.Itoa(sm.W5Busy), strconv.Itoa(sm.W5Obss), strconv.Itoa(sm.W5Txop),
			strconv.Itoa(sm.W5Noise), strconv.Itoa(sm.W5Glitch), fmt.Sprintf("%.1f", sm.W5Retrans),
			strconv.Itoa(sm.W5Temp), strconv.Itoa(sm.W5Clients),
			strconv.Itoa(sm.W2Busy), strconv.Itoa(sm.W2Obss), strconv.Itoa(sm.W2Txop),
			strconv.Itoa(sm.W2Noise), strconv.Itoa(sm.W2Glitch), fmt.Sprintf("%.1f", sm.W2Retrans),
			strconv.Itoa(sm.W2Temp), strconv.Itoa(sm.W2Clients), fmt.Sprintf("%.1f", sm.SocTemp),
		})
	}
	w.Flush()
	return buf.Bytes()
}
