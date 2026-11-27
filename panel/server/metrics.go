package main

import (
	"fmt"
	"net/http"
	"strings"
)

func asFloat(v any) float64 {
	switch x := v.(type) {
	case float64:
		return x
	case int:
		return float64(x)
	case int64:
		return float64(x)
	case uint64:
		return float64(x)
	}
	return 0
}

// handleMetrics exposes a standard Prometheus / OpenMetrics text endpoint.
// Authenticates via Authorization: Bearer <token>, X-Token: <token>, or ?t=<token>.
func (s *Server) handleMetrics(w http.ResponseWriter, r *http.Request) {
	ov := s.collectOverview()

	var b strings.Builder
	b.WriteString("# HELP hg6371f_up AP panel exporter up status.\n# TYPE hg6371f_up gauge\nhg6371f_up 1\n")
	fmt.Fprintf(&b, "# HELP hg6371f_uptime_seconds System uptime in seconds.\n# TYPE hg6371f_uptime_seconds counter\nhg6371f_uptime_seconds %.0f\n", asFloat(ov["uptime_sec"]))
	fmt.Fprintf(&b, "# HELP hg6371f_cpu_usage_percent CPU usage percentage.\n# TYPE hg6371f_cpu_usage_percent gauge\nhg6371f_cpu_usage_percent %.2f\n", asFloat(ov["cpu_pct"]))
	fmt.Fprintf(&b, "# HELP hg6371f_processes Total process count.\n# TYPE hg6371f_processes gauge\nhg6371f_processes %.0f\n", asFloat(ov["procs"]))
	fmt.Fprintf(&b, "# HELP hg6371f_memory_total_bytes Total RAM in bytes.\n# TYPE hg6371f_memory_total_bytes gauge\nhg6371f_memory_total_bytes %.0f\n", asFloat(ov["mem_total_mb"])*1024*1024)
	fmt.Fprintf(&b, "# HELP hg6371f_memory_available_bytes Available RAM in bytes.\n# TYPE hg6371f_memory_available_bytes gauge\nhg6371f_memory_available_bytes %.0f\n", asFloat(ov["mem_avail_mb"])*1024*1024)
	if loadStr, ok := ov["load"].(string); ok {
		if lf := strings.Fields(loadStr); len(lf) >= 1 {
			fmt.Fprintf(&b, "# HELP hg6371f_load1 1-minute load average.\n# TYPE hg6371f_load1 gauge\nhg6371f_load1 %s\n", lf[0])
		}
	}

	b.WriteString("# HELP hg6371f_temperature_celsius Hardware temperature sensors.\n# TYPE hg6371f_temperature_celsius gauge\n")
	if t := asFloat(ov["temp_c"]); t > 0 {
		fmt.Fprintf(&b, "hg6371f_temperature_celsius{sensor=\"soc\"} %.1f\n", t)
	}
	if t := asFloat(ov["wl1_temp_c"]); t > 0 {
		fmt.Fprintf(&b, "hg6371f_temperature_celsius{sensor=\"wl1\"} %.1f\n", t)
	}
	if t := asFloat(ov["wl0_temp_c"]); t > 0 {
		fmt.Fprintf(&b, "hg6371f_temperature_celsius{sensor=\"wl0\"} %.1f\n", t)
	}

	if fc, ok := ov["fcache"].(map[string]any); ok {
		hw := 0
		if en, _ := fc["hw_enabled"].(bool); en {
			hw = 1
		}
		fmt.Fprintf(&b, "# HELP hg6371f_flowcache_hw_enabled Broadcom FlowCache HW acceleration enabled.\n# TYPE hg6371f_flowcache_hw_enabled gauge\nhg6371f_flowcache_hw_enabled %d\n", hw)
		fmt.Fprintf(&b, "# HELP hg6371f_flowcache_active_flows Active FlowCache hardware flows.\n# TYPE hg6371f_flowcache_active_flows gauge\nhg6371f_flowcache_active_flows %.0f\n", asFloat(fc["active_flows"]))
		fmt.Fprintf(&b, "# HELP hg6371f_flowcache_cumulative_flows Cumulative FlowCache hardware flows.\n# TYPE hg6371f_flowcache_cumulative_flows counter\nhg6371f_flowcache_cumulative_flows %.0f\n", asFloat(fc["cumulative"]))
	}

	devs := readNetDevBytes()
	b.WriteString("# HELP hg6371f_net_rx_bytes_total Interface received bytes.\n# TYPE hg6371f_net_rx_bytes_total counter\n")
	for _, ifn := range []string{"lan0", "lan1", "wl1", "wl0", "br0"} {
		if pair, ok := devs[ifn]; ok {
			fmt.Fprintf(&b, "hg6371f_net_rx_bytes_total{iface=%q} %d\n", ifn, pair[0])
		}
	}
	b.WriteString("# HELP hg6371f_net_tx_bytes_total Interface transmitted bytes.\n# TYPE hg6371f_net_tx_bytes_total counter\n")
	for _, ifn := range []string{"lan0", "lan1", "wl1", "wl0", "br0"} {
		if pair, ok := devs[ifn]; ok {
			fmt.Fprintf(&b, "hg6371f_net_tx_bytes_total{iface=%q} %d\n", ifn, pair[1])
		}
	}

	if radios, ok := ov["radios"].([]RadioSummary); ok && len(radios) > 0 {
		b.WriteString("# HELP hg6371f_wifi_clients Associated stations per radio.\n# TYPE hg6371f_wifi_clients gauge\n")
		for _, rd := range radios {
			fmt.Fprintf(&b, "hg6371f_wifi_clients{iface=%q,band=%q} %d\n", rd.Iface, rd.Band, rd.Clients)
		}
		b.WriteString("# HELP hg6371f_wifi_noise_dbm Radio noise floor in dBm.\n# TYPE hg6371f_wifi_noise_dbm gauge\n")
		for _, rd := range radios {
			fmt.Fprintf(&b, "hg6371f_wifi_noise_dbm{iface=%q,band=%q} %d\n", rd.Iface, rd.Band, rd.Air.NoiseDBm)
		}
		b.WriteString("# HELP hg6371f_wifi_air_busy_percent Channel total busy percentage.\n# TYPE hg6371f_wifi_air_busy_percent gauge\n")
		for _, rd := range radios {
			fmt.Fprintf(&b, "hg6371f_wifi_air_busy_percent{iface=%q,band=%q} %d\n", rd.Iface, rd.Band, rd.Air.BusyPct)
		}
		b.WriteString("# HELP hg6371f_wifi_obss_percent Neighbor co-channel interference (OBSS) percentage.\n# TYPE hg6371f_wifi_obss_percent gauge\n")
		for _, rd := range radios {
			fmt.Fprintf(&b, "hg6371f_wifi_obss_percent{iface=%q,band=%q} %d\n", rd.Iface, rd.Band, rd.Air.ObssPct)
		}
		b.WriteString("# HELP hg6371f_wifi_txop_percent Available airtime (TXOP) percentage.\n# TYPE hg6371f_wifi_txop_percent gauge\n")
		for _, rd := range radios {
			fmt.Fprintf(&b, "hg6371f_wifi_txop_percent{iface=%q,band=%q} %d\n", rd.Iface, rd.Band, rd.Air.TxopPct)
		}
		b.WriteString("# HELP hg6371f_wifi_tx_retry_percent Radio TX retry rate percentage.\n# TYPE hg6371f_wifi_tx_retry_percent gauge\n")
		for _, rd := range radios {
			fmt.Fprintf(&b, "hg6371f_wifi_tx_retry_percent{iface=%q,band=%q} %.1f\n", rd.Iface, rd.Band, rd.Air.TxRetrans)
		}
	}

	if s.radar != nil {
		rcfg := s.getConfig().Radar
		en, occ := 0, 0
		if rcfg.Enabled {
			en = 1
		}
		s.radar.mu.Lock()
		if s.radar.occupied {
			occ = 1
		}
		score := s.radar.score
		ampMAD := s.radar.ampMAD
		phMAD := s.radar.phaseMAD
		frames := s.radar.frames
		r0, r1 := s.radar.rssi0, s.radar.rssi1
		s.radar.mu.Unlock()

		fmt.Fprintf(&b, "# HELP hg6371f_csi_radar_enabled CSIMON presence radar enabled.\n# TYPE hg6371f_csi_radar_enabled gauge\nhg6371f_csi_radar_enabled %d\n", en)
		fmt.Fprintf(&b, "# HELP hg6371f_csi_radar_occupied CSIMON room occupancy state (1=occupied, 0=empty).\n# TYPE hg6371f_csi_radar_occupied gauge\nhg6371f_csi_radar_occupied %d\n", occ)
		fmt.Fprintf(&b, "# HELP hg6371f_csi_radar_score Current W=10 MAD motion score.\n# TYPE hg6371f_csi_radar_score gauge\nhg6371f_csi_radar_score %.2f\n", score)
		fmt.Fprintf(&b, "# HELP hg6371f_csi_radar_phase_mad_deg Inter-antenna phase difference MAD in degrees.\n# TYPE hg6371f_csi_radar_phase_mad_deg gauge\nhg6371f_csi_radar_phase_mad_deg %.2f\n", phMAD)
		fmt.Fprintf(&b, "# HELP hg6371f_csi_radar_amp_mad Normalized RX1 amplitude MAD.\n# TYPE hg6371f_csi_radar_amp_mad gauge\nhg6371f_csi_radar_amp_mad %.5f\n", ampMAD)
		fmt.Fprintf(&b, "# HELP hg6371f_csi_radar_frames_total Total CSI frames received over Netlink.\n# TYPE hg6371f_csi_radar_frames_total counter\nhg6371f_csi_radar_frames_total %d\n", frames)
		if frames > 0 {
			b.WriteString("# HELP hg6371f_csi_radar_rssi_dbm Target STA per-antenna RSSI in dBm.\n# TYPE hg6371f_csi_radar_rssi_dbm gauge\n")
			fmt.Fprintf(&b, "hg6371f_csi_radar_rssi_dbm{chain=\"0\"} %d\n", r0)
			fmt.Fprintf(&b, "hg6371f_csi_radar_rssi_dbm{chain=\"1\"} %d\n", r1)
		}
	}

	if s.stamon != nil {
		scfg := s.getConfig().StaMon
		en := 0
		if scfg.Enabled {
			en = 1
		}
		fmt.Fprintf(&b, "# HELP hg6371f_stamon_enabled Unassociated STA proximity monitor enabled.\n# TYPE hg6371f_stamon_enabled gauge\nhg6371f_stamon_enabled %d\n", en)
		s.stamon.mu.Lock()
		if len(scfg.Targets) > 0 {
			b.WriteString("# HELP hg6371f_stamon_present Monitored STA proximity state (1=present, 0=away).\n# TYPE hg6371f_stamon_present gauge\n")
			for _, t := range scfg.Targets {
				mac := strings.ToUpper(strings.TrimSpace(t.MAC))
				st := s.stamon.states[mac]
				p := 0
				lbl := t.Label
				if st != nil {
					if st.Present {
						p = 1
					}
					if st.Label != "" {
						lbl = st.Label
					}
				}
				if lbl == "" {
					lbl = mac
				}
				fmt.Fprintf(&b, "hg6371f_stamon_present{mac=%q,label=%q} %d\n", mac, lbl, p)
			}
			wroteRSSI := false
			for _, t := range scfg.Targets {
				mac := strings.ToUpper(strings.TrimSpace(t.MAC))
				st := s.stamon.states[mac]
				if st == nil || st.LastSeenTs == 0 || st.BestRSSI >= 0 {
					continue
				}
				if !wroteRSSI {
					b.WriteString("# HELP hg6371f_stamon_rssi_dbm Last captured RSSI in dBm for monitored STA.\n# TYPE hg6371f_stamon_rssi_dbm gauge\n")
					wroteRSSI = true
				}
				lbl := st.Label
				if lbl == "" {
					lbl = mac
				}
				fmt.Fprintf(&b, "hg6371f_stamon_rssi_dbm{mac=%q,label=%q,band=%q} %d\n", mac, lbl, st.BestBand, st.BestRSSI)
			}
		}
		s.stamon.mu.Unlock()
	}

	w.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(b.String()))
}
