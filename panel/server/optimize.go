package main

import (
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"
)

type RFSnapshot struct {
	TS            int64   `json:"ts"`
	Chanspec5G    string  `json:"chanspec_5g"`
	Chanspec24G   string  `json:"chanspec_24g"`
	Txop5G        int     `json:"txop_5g"`
	Obss5G        int     `json:"obss_5g"`
	Busy5G        int     `json:"busy_5g"`
	Noise5G       int     `json:"noise_5g"`
	Glitch5G      int     `json:"glitch_5g"`
	DeltaRetrans  float64 `json:"delta_retrans_pct"` // real-time 1.5s window retry rate
	AvgTxMbps     float64 `json:"avg_tx_mbps"`
	AvgRxMbps     float64 `json:"avg_rx_mbps"`
	ClientCount5G int     `json:"client_count_5g"`
}

func readTxCounters(iface string) (frames, retrans uint64) {
	cnt := runCmdOut(2*time.Second, "wl", "-i", iface, "counters")
	if m := reCntTxFrame.FindStringSubmatch(cnt); len(m) == 2 {
		frames, _ = strconv.ParseUint(m[1], 10, 64)
	}
	if m := reCntTxRetrans.FindStringSubmatch(cnt); len(m) == 2 {
		retrans, _ = strconv.ParseUint(m[1], 10, 64)
	}
	return
}

// collectRFSnapshot captures an accurate hardware-level RF snapshot using a 1.2s
// delta window for real-time retry rate plus chanim_stats and client link rates.
func (s *Server) collectRFSnapshot() RFSnapshot {
	f0, r0 := readTxCounters("wl1")
	time.Sleep(1200 * time.Millisecond)
	f1, r1 := readTxCounters("wl1")

	air5 := collectAirStats("wl1")
	_, _, cs5 := parseChanspec(runCmdOut(2*time.Second, "wl", "-i", "wl1", "chanspec"))
	_, _, cs2 := parseChanspec(runCmdOut(2*time.Second, "wl", "-i", "wl0", "chanspec"))
	clients5 := s.collectClientsFor("wl1")

	var sumTx, sumRx float64
	for _, c := range clients5 {
		sumTx += c.TxRate
		sumRx += c.RxRate
	}
	avgTx, avgRx := 0.0, 0.0
	if len(clients5) > 0 {
		avgTx = math.Round((sumTx/float64(len(clients5)))*10) / 10
		avgRx = math.Round((sumRx/float64(len(clients5)))*10) / 10
	}

	deltaRet := air5.TxRetrans
	if f1 > f0 && r1 >= r0 {
		df := f1 - f0
		dr := r1 - r0
		if df >= 5 {
			deltaRet = math.Round(float64(dr)*1000.0/float64(df+dr)) / 10.0
		}
	}

	return RFSnapshot{
		TS:            time.Now().Unix(),
		Chanspec5G:    cs5,
		Chanspec24G:   cs2,
		Txop5G:        air5.TxopPct,
		Obss5G:        air5.ObssPct,
		Busy5G:        air5.BusyPct,
		Noise5G:       air5.NoiseDBm,
		Glitch5G:      air5.Glitch,
		DeltaRetrans:  deltaRet,
		AvgTxMbps:     avgTx,
		AvgRxMbps:     avgRx,
		ClientCount5G: len(clients5),
	}
}

type ChannelScore struct {
	Channel        int    `json:"channel"`
	WidthMHz       int    `json:"width_mhz"`
	Score          int    `json:"score"` // 0..100 (higher = cleaner)
	PrimaryCount   int    `json:"primary_count"`
	OverlapCount   int    `json:"overlap_count"`
	StrongestRSSI  int    `json:"strongest_rssi"`
	StrongestSSID  string `json:"strongest_ssid"`
	IsCurrent      bool   `json:"is_current"`
	IsRecommended  bool   `json:"is_recommended"`
}

type BandOptimizeReport struct {
	Iface         string         `json:"iface"`
	CurrentCh     int            `json:"current_ch"`
	CurrentWidth  int            `json:"current_width"`
	BestCh        int            `json:"best_ch"`
	BestWidth     int            `json:"best_width"`
	NeedSwitch    bool           `json:"need_switch"`
	Summary       string         `json:"summary"`
	NeighborCount int            `json:"neighbor_count"`
	Candidates    []ChannelScore `json:"candidates"`
}

type OptimizeItem struct {
	ID          string `json:"id"`
	Label       string `json:"label"`
	Current     string `json:"current"`
	Recommended string `json:"recommended"`
	Reason      string `json:"reason"`
	Changed     bool   `json:"changed"`
	Disruptive  bool   `json:"disruptive"`
}

type AutoPlan struct {
	Wl1Channel    int  `json:"wl1_channel"`
	Wl1WidthMHz   int  `json:"wl1_width_mhz"`
	Wl1NeedSwitch bool `json:"wl1_need_switch"`
	Wl0Channel    int  `json:"wl0_channel"`
	Wl0NeedSwitch bool `json:"wl0_need_switch"`
	Frameburst    int  `json:"frameburst"`
	AmpduRTS      int  `json:"ampdu_rts"`
	PhyRxdesens   int  `json:"phy_rxdesens"`
	Wl1ATF        int  `json:"wl1_atf"`
	Wl1SpectOff   bool `json:"wl1_spect_off"`
	RunTuneScript bool `json:"run_tune_script"`
}

type AutoApplyReq struct {
	ApplyChannels bool      `json:"apply_channels"`
	Plan          *AutoPlan `json:"plan,omitempty"`
}

func rssiLinearPower(rssi int) float64 {
	if rssi >= 0 || rssi < -105 {
		return 0
	}
	// Normalized linear power relative to -100 dBm (so -40 dBm = 1e6, -70 dBm = 1e3)
	return math.Pow(10.0, float64(rssi+100)/10.0)
}

func scoreFromInterference(energy float64) int {
	if energy <= 1 {
		return 100
	}
	// Logarithmic map: 10^2 -> ~92, 10^4 -> ~65, 10^6 -> ~30, 10^7.5 -> ~8
	db := math.Log10(energy)
	s := 100.0 - db*12.5
	if s < 5 {
		s = 5
	}
	if s > 100 {
		s = 100
	}
	return int(math.Round(s))
}

func score5GChannels(aps []ScanAP, curCh, curWidth int) BandOptimizeReport {
	// Evaluate the 4 primary channels that support full 160 MHz in CN (36, 40, 44, 48).
	candidates := []int{36, 40, 44, 48}
	scores := make([]ChannelScore, 0, len(candidates))
	rawEnergies := make(map[int]float64)

	for _, ch := range candidates {
		var energy float64
		primCnt, ovCnt := 0, 0
		strongestRSSI := -120
		strongestSSID := ""

		for _, ap := range aps {
			p := ap.Primary
			if p == 0 {
				p = ap.Channel
			}
			// Only APs inside 36..64 overlap the 36..64 160 MHz block
			if p < 36 || p > 64 {
				continue
			}
			pw := rssiLinearPower(ap.RSSI)
			if p == ch {
				primCnt++
				energy += pw * 6.0 // Direct primary 20MHz beacon + CCA collision
			} else {
				ovCnt++
				energy += pw * 0.6 // Secondary 160MHz sub-band overlap
			}
			if ap.RSSI > strongestRSSI {
				strongestRSSI = ap.RSSI
				strongestSSID = ap.SSID
				if strongestSSID == "" {
					strongestSSID = "<隐藏>"
				}
			}
		}

		// Slight tie-breaker so current channel or ch 48 wins close ties
		adj := energy
		if ch == curCh {
			adj *= 0.92
		} else if ch == 48 {
			adj *= 0.97
		}
		rawEnergies[ch] = adj

		cs := ChannelScore{
			Channel:       ch,
			WidthMHz:      160,
			Score:         scoreFromInterference(energy),
			PrimaryCount:  primCnt,
			OverlapCount:  ovCnt,
			StrongestRSSI: strongestRSSI,
			StrongestSSID: strongestSSID,
			IsCurrent:     ch == curCh,
		}
		if strongestRSSI <= -110 {
			cs.StrongestRSSI = 0
		}
		scores = append(scores, cs)
	}

	bestIdx := 0
	for i := 1; i < len(scores); i++ {
		if rawEnergies[scores[i].Channel] < rawEnergies[scores[bestIdx].Channel] {
			bestIdx = i
		}
	}
	bestCh := scores[bestIdx].Channel
	scores[bestIdx].IsRecommended = true

	needSwitch := false
	if curWidth != 160 || curCh < 36 || curCh > 48 {
		needSwitch = true
	} else if bestCh != curCh {
		curE := rawEnergies[curCh]
		bestE := rawEnergies[bestCh]
		if curE > 0 && bestE < curE*0.75 {
			needSwitch = true
		} else {
			bestCh = curCh
			for i := range scores {
				scores[i].IsRecommended = scores[i].Channel == curCh
			}
		}
	}

	summary := fmt.Sprintf("当前 %d/%dMHz 已是 160MHz 频段内主信道冲突最低的选择", curCh, curWidth)
	if needSwitch {
		summary = fmt.Sprintf("建议切换至 %d/160MHz 以避开同主信道邻居竞争", bestCh)
	}

	return BandOptimizeReport{
		Iface:         "wl1",
		CurrentCh:     curCh,
		CurrentWidth:  curWidth,
		BestCh:        bestCh,
		BestWidth:     160,
		NeedSwitch:    needSwitch,
		Summary:       summary,
		NeighborCount: len(aps),
		Candidates:    scores,
	}
}

func overlap24Weight(cand, apCh int) float64 {
	d := cand - apCh
	if d < 0 {
		d = -d
	}
	switch d {
	case 0:
		return 1.0
	case 1:
		return 0.72
	case 2:
		return 0.35
	case 3:
		return 0.12
	default:
		return 0
	}
}

func score24GChannels(aps []ScanAP, curCh int) BandOptimizeReport {
	all := make([]ChannelScore, 0, 13)
	rawEnergies := make(map[int]float64)

	for ch := 1; ch <= 13; ch++ {
		var energy float64
		primCnt, ovCnt := 0, 0
		strongestRSSI := -120
		strongestSSID := ""

		for _, ap := range aps {
			p := ap.Primary
			if p == 0 {
				p = ap.Channel
			}
			if p < 1 || p > 13 {
				continue
			}
			w := overlap24Weight(ch, p)
			// Account for 40 MHz secondary channel ('l' = +4, 'u' = -4)
			if ap.WidthMHz >= 40 {
				sec := 0
				if ap.Sideband == "l" && p+4 <= 13 {
					sec = p + 4
				} else if ap.Sideband == "u" && p-4 >= 1 {
					sec = p - 4
				}
				if sec > 0 {
					w2 := overlap24Weight(ch, sec) * 0.85
					if w2 > w {
						w = w2
					}
				}
			}
			if w <= 0 {
				continue
			}
			pw := rssiLinearPower(ap.RSSI) * w
			energy += pw
			if p == ch {
				primCnt++
			} else {
				ovCnt++
			}
			if w >= 0.35 && ap.RSSI > strongestRSSI {
				strongestRSSI = ap.RSSI
				strongestSSID = ap.SSID
				if strongestSSID == "" {
					strongestSSID = "<隐藏>"
				}
			}
		}

		// Gentle preference for standard 1/6/11 channels and current channel on near-ties
		adj := energy
		if ch == 1 || ch == 6 || ch == 11 {
			adj *= 0.90
		}
		if ch == curCh {
			adj *= 0.92
		}
		rawEnergies[ch] = adj

		cs := ChannelScore{
			Channel:       ch,
			WidthMHz:      20,
			Score:         scoreFromInterference(energy),
			PrimaryCount:  primCnt,
			OverlapCount:  ovCnt,
			StrongestRSSI: strongestRSSI,
			StrongestSSID: strongestSSID,
			IsCurrent:     ch == curCh,
		}
		if strongestRSSI <= -110 {
			cs.StrongestRSSI = 0
		}
		all = append(all, cs)
	}

	sort.Slice(all, func(i, j int) bool {
		return rawEnergies[all[i].Channel] < rawEnergies[all[j].Channel]
	})

	bestCh := all[0].Channel
	needSwitch := false
	if bestCh != curCh && rawEnergies[curCh] > 0 && rawEnergies[bestCh] < rawEnergies[curCh]*0.72 {
		needSwitch = true
	} else {
		bestCh = curCh
	}

	// Present top 4 cleanest channels (always including current channel)
	top := make([]ChannelScore, 0, 5)
	hasCur := false
	for _, c := range all {
		if c.Channel == bestCh {
			c.IsRecommended = true
		}
		if len(top) < 4 || (c.IsCurrent && !hasCur) {
			if c.IsCurrent {
				hasCur = true
			}
			top = append(top, c)
		}
	}

	summary := fmt.Sprintf("当前 2.4G 信道 %d 已较好避开周边强邻道干扰", curCh)
	if needSwitch {
		summary = fmt.Sprintf("建议切换 2.4G 至信道 %d，可显著降低强邻道重叠干扰", bestCh)
	}

	return BandOptimizeReport{
		Iface:         "wl0",
		CurrentCh:     curCh,
		CurrentWidth:  20,
		BestCh:        bestCh,
		BestWidth:     20,
		NeedSwitch:    needSwitch,
		Summary:       summary,
		NeighborCount: len(aps),
		Candidates:    top,
	}
}

func (s *Server) analyzeAutoOptimize() (map[string]any, error) {
	scan5, _ := s.scanWifi("wl1")
	scan2, _ := s.scanWifi("wl0")
	aps5, _ := scan5["aps"].([]ScanAP)
	aps2, _ := scan2["aps"].([]ScanAP)

	curCh5, curW5, _ := parseChanspec(runCmdOut(2*time.Second, "wl", "-i", "wl1", "chanspec"))
	curCh2, _, _ := parseChanspec(runCmdOut(2*time.Second, "wl", "-i", "wl0", "chanspec"))

	rep5 := score5GChannels(aps5, curCh5, curW5)
	rep2 := score24GChannels(aps2, curCh2)

	air5 := collectAirStats("wl1")
	clients5 := s.collectClientsFor("wl1")

	curFB := wlInt("wl1", "frameburst")
	curRTS := wlInt("wl1", "ampdu_rts")
	curDesens := wlInt("wl1", "phy_rxdesens")
	curATF := wlInt("wl1", "atf")
	curSpect := strings.TrimSpace(runCmdOut(2*time.Second, "wl", "-i", "wl1", "spect"))
	curSpectOff := strings.EqualFold(curSpect, "Off")
	rmem := readTrim("/proc/sys/net/core/rmem_max")
	wmem := readTrim("/proc/sys/net/core/wmem_max")
	backlog := readTrim("/proc/sys/net/core/netdev_max_backlog")
	wmeRaw := runCmdOut(2*time.Second, "wl", "-i", "wl1", "wme_ac", "ap")
	wmeOK := strings.Contains(wmeRaw, "txop 0x2f") && strings.Contains(wmeRaw, "ecwmax 5")

	// Adaptive RF parameter targets
	recFB := 1
	recRTS := 0
	rtsReason := "高吞吐推荐关闭（当前 5G 干扰低，免去每帧 RTS 握手开销）"
	if air5.ObssPct >= 22 && air5.TxRetrans >= 25 {
		recRTS = 1
		rtsReason = fmt.Sprintf("检测到较强同频竞争（OBSS %d%% / 重传 %.1f%%），建议开启防隐藏节点冲突", air5.ObssPct, air5.TxRetrans)
	}

	recDesens := 15
	desensReason := "中等底噪推荐 15（过滤远端弱噪声同时保住高 MCS 灵敏度）"
	if air5.NoiseDBm != 0 && air5.NoiseDBm <= -86 && air5.Glitch < 30 {
		recDesens = 10
		desensReason = fmt.Sprintf("5G 空口很干净（底噪 %d dBm / glitch %d），可适当降至 10 提升远距灵敏度", air5.NoiseDBm, air5.Glitch)
	} else if air5.NoiseDBm > -76 || air5.Glitch >= 150 {
		recDesens = 20
		desensReason = fmt.Sprintf("5G 底噪/脉冲干扰偏高（底噪 %d dBm / glitch %d），建议提高至 20 抑制误触发", air5.NoiseDBm, air5.Glitch)
	}

	// Recommend ATF=1 when mixed-generation or slow clients share wl1 with Wi-Fi 6 clients
	hasSlow, hasFast := false, false
	for _, c := range clients5 {
		if c.Mode == "Wi-Fi 6" {
			hasFast = true
		} else {
			hasSlow = true
		}
	}
	recATF := 0
	atfReason := "当前在线均为同代高速终端，保持关闭避免额外调度开销"
	if hasFast && hasSlow {
		recATF = 1
		atfReason = "检测到新老制式终端混连，建议开启空口公平防止老设备拖慢 160MHz"
	}

	sysctlOK := rmem == "4194304" && wmem == "4194304" && backlog == "4096"
	needTuneScript := !sysctlOK || !wmeOK

	plan := AutoPlan{
		Wl1Channel:    rep5.BestCh,
		Wl1WidthMHz:   160,
		Wl1NeedSwitch: rep5.NeedSwitch,
		Wl0Channel:    rep2.BestCh,
		Wl0NeedSwitch: rep2.NeedSwitch,
		Frameburst:    recFB,
		AmpduRTS:      recRTS,
		PhyRxdesens:   recDesens,
		Wl1ATF:        recATF,
		Wl1SpectOff:   !curSpectOff,
		RunTuneScript: needTuneScript,
	}

	items := []OptimizeItem{
		{
			ID:          "wl1_ch",
			Label:       "5G 主信道 / 频宽",
			Current:     fmt.Sprintf("%d / %dMHz", curCh5, curW5),
			Recommended: fmt.Sprintf("%d / 160MHz", rep5.BestCh),
			Reason:      rep5.Summary,
			Changed:     rep5.NeedSwitch,
			Disruptive:  rep5.NeedSwitch,
		},
		{
			ID:          "wl0_ch",
			Label:       "2.4G 信道",
			Current:     fmt.Sprintf("信道 %d (20MHz)", curCh2),
			Recommended: fmt.Sprintf("信道 %d (20MHz)", rep2.BestCh),
			Reason:      rep2.Summary,
			Changed:     rep2.NeedSwitch,
			Disruptive:  rep2.NeedSwitch,
		},
		{
			ID:          "frameburst",
			Label:       "帧突发 (frameburst)",
			Current:     strconv.Itoa(curFB),
			Recommended: strconv.Itoa(recFB),
			Reason:      "连续帧聚合传输，提升大流量吞吐",
			Changed:     curFB != recFB,
		},
		{
			ID:          "ampdu_rts",
			Label:       "5G A-MPDU RTS",
			Current:     strconv.Itoa(curRTS),
			Recommended: strconv.Itoa(recRTS),
			Reason:      rtsReason,
			Changed:     curRTS != recRTS,
		},
		{
			ID:          "phy_rxdesens",
			Label:       "5G 接收降敏 (phy_rxdesens)",
			Current:     strconv.Itoa(curDesens),
			Recommended: strconv.Itoa(recDesens),
			Reason:      desensReason,
			Changed:     curDesens != recDesens,
		},
		{
			ID:          "atf",
			Label:       "5G 空口时间公平 (ATF)",
			Current:     strconv.Itoa(curATF),
			Recommended: strconv.Itoa(recATF),
			Reason:      atfReason,
			Changed:     curATF != recATF,
		},
		{
			ID:          "spect",
			Label:       "5G 802.11h 频谱管理 (spect)",
			Current:     curSpect,
			Recommended: "Off",
			Reason:      "与原厂保持一致，避免进入 DFS ISM 导致扫描受限",
			Changed:     !curSpectOff,
			Disruptive:  !curSpectOff,
		},
		{
			ID:          "wme_sysctl",
			Label:       "WME AC_BE 窗口与内核队列缓冲",
			Current:     fmt.Sprintf("WME:%v · rmem:%s", wmeOK, rmem),
			Recommended: "TXOP 1504us · 4MiB/4096",
			Reason:      "确保 2.5G 上联与 160MHz 无线突发转发不丢包",
			Changed:     needTuneScript,
		},
	}

	changes := 0
	for _, it := range items {
		if it.Changed {
			changes++
		}
	}
	health := 100 - changes*8
	if air5.BusyPct > 40 {
		health -= 10
	}
	if health < 40 {
		health = 40
	}

	return map[string]any{
		"health_score": health,
		"changes":      changes,
		"band5g":       rep5,
		"band24g":      rep2,
		"items":        items,
		"plan":         plan,
		"analyzed_at":  time.Now().Format("15:04:05"),
	}, nil
}

func (s *Server) applyAutoOptimize(req AutoApplyReq) (map[string]any, error) {
	plan := req.Plan
	if plan == nil {
		an, err := s.analyzeAutoOptimize()
		if err != nil {
			return nil, err
		}
		p := an["plan"].(AutoPlan)
		plan = &p
	}

	applied := make([]string, 0, 8)

	if plan.RunTuneScript {
		s.applyBaselineTune()
		applied = append(applied, "已校准 WME AC_BE 窗口与内核队列缓冲")
	}

	// Hitless RF tuning
	fb, rts, des, atf := plan.Frameburst, plan.AmpduRTS, plan.PhyRxdesens, plan.Wl1ATF
	if err := s.applyTune(TuneApplyReq{
		Frameburst:  &fb,
		AmpduRTS:    &rts,
		PhyRxdesens: &des,
		Wl1ATF:      &atf,
	}); err != nil {
		return nil, err
	}
	applied = append(applied, fmt.Sprintf("射频参数已校准（frameburst=%d, ampdu_rts=%d, rxdesens=%d, atf=%d）", fb, rts, des, atf))

	// Channel / spect changes (only if requested & needed)
	if req.ApplyChannels && (plan.Wl1NeedSwitch || plan.Wl1SpectOff) {
		conf1 := readHapdConf("wl1")
		rc1 := s.getConfig().Radios["wl1"]
		if conf1["ssid"] != "" {
			if err := s.saveWifiConfig(WifiSaveReq{
				Iface:        "wl1",
				SSID:         conf1["ssid"],
				Hidden:       conf1["ignore_broadcast_ssid"] == "1",
				Isolate:      rc1.Isolate,
				Channel:      plan.Wl1Channel,
				WidthMHz:     160,
				TxPwrPercent: rc1.TxPwrPercent,
				AssocRSSITh:  rc1.AssocRSSITh,
			}); err != nil {
				return nil, err
			}
			applied = append(applied, fmt.Sprintf("5G 已切换至 %d/160MHz 并关闭 802.11h", plan.Wl1Channel))
		}
	}

	if req.ApplyChannels && plan.Wl0NeedSwitch {
		conf0 := readHapdConf("wl0")
		rc0 := s.getConfig().Radios["wl0"]
		if conf0["ssid"] != "" {
			if err := s.saveWifiConfig(WifiSaveReq{
				Iface:        "wl0",
				SSID:         conf0["ssid"],
				Hidden:       conf0["ignore_broadcast_ssid"] == "1",
				Isolate:      rc0.Isolate,
				Channel:      plan.Wl0Channel,
				WidthMHz:     rc0.WidthMHz,
				TxPwrPercent: rc0.TxPwrPercent,
				AssocRSSITh:  rc0.AssocRSSITh,
			}); err != nil {
				return nil, err
			}
			applied = append(applied, fmt.Sprintf("2.4G 已切换至信道 %d", plan.Wl0Channel))
		}
	}

	return map[string]any{
		"ok":      true,
		"applied": applied,
		"tune":    s.collectTune(),
	}, nil
}
