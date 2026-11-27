package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"math/cmplx"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	radarMaxWindow    = 40              // up to 40 frames (4.0s @ 10 Hz or 20 frames = 4.0s @ 5 Hz)
	radarLiveMax      = 120             // 120 x 1s = 2 min real-time curve
	radarHistMax      = 1440            // 24h x 1 min summary (zero NAND writes)
	radarEventsMax    = 50
	radarReentryGrace = 5 * time.Minute // grace window after occupied->empty for moderate seated re-entry
)

type RadarPoint struct {
	Ts       int64   `json:"ts"`
	Score    float64 `json:"score"`
	AmpMAD   float64 `json:"amp_mad"`
	PhaseMAD float64 `json:"phase_mad"`
	State    string  `json:"state"`
	Occupied bool    `json:"occupied"`
}

type RadarMinute struct {
	Ts        int64   `json:"ts"`
	PeakScore float64 `json:"peak_score"`
	OccSec    int     `json:"occ_sec"`
}

type RadarEvent struct {
	Ts        int64   `json:"ts"`
	From      string  `json:"from"`
	To        string  `json:"to"`
	PeakScore float64 `json:"peak_score"`
	Detail    string  `json:"detail"`
}

type RadarCandidate struct {
	Iface string `json:"iface"`
	MAC   string `json:"mac"`
	Note  string `json:"note"`
	RSSI  int    `json:"rssi"`
}

type radarFrame struct {
	ts    time.Time
	a0    [14]float64
	a1    [14]float64
	pdiff [14]float64
}

type radarEngine struct {
	mu          sync.Mutex
	reloadCh    chan struct{}
	activeIface string
	activeMAC   string

	netlinkProto int
	frames       uint64
	lastFrameAt  time.Time
	rssi0        int
	rssi1        int
	chanspec     uint16

	rawRing [radarMaxWindow]radarFrame
	smRing  [radarMaxWindow]radarFrame
	ringLen int
	ringIdx int

	score          float64
	ampMAD         float64
	phaseMAD       float64
	state          string // "empty" | "micro" | "motion" | "waiting" | "dma_fault"
	occupied       bool
	occUntil       time.Time
	occPeak        float64
	lastEmptyAt    time.Time // timestamp of last occupied -> empty transition (drives reentry grace)
	evMotion       float64   // leaky-bucket motion evidence (seconds)
	evMicro        float64   // leaky-bucket sustained micro evidence (seconds)
	lastEvalAt     time.Time
	scanPauseUntil time.Time

	secBucketTs int64
	secPeak     float64
	secAmp      float64
	secPhase    float64
	secState    string
	secOcc      bool
	livePoints  []RadarPoint

	minBucketTs int64
	minPeak     float64
	minOccSec   int
	histPoints  []RadarMinute
	events      []RadarEvent

	calActive  bool
	calStart   time.Time
	calDur     time.Duration
	calFeats   [][6]float64
	calLastMsg string

	suspendCount map[string]int
	sessionDone  chan struct{}
	faultMsg     string
}

func newRadarEngine() *radarEngine {
	return &radarEngine{
		reloadCh:     make(chan struct{}, 1),
		state:        "empty",
		suspendCount: make(map[string]int),
	}
}

func (r *radarEngine) notifyReload() {
	select {
	case r.reloadCh <- struct{}{}:
	default:
	}
}

func (r *radarEngine) isSuspended(iface string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.suspendCount[iface] > 0
}

// expireOccupiedIfDueLocked preserves occupancy across brief target STA reconnects or
// radio reloads, and emits a proper occupied->empty transition if hold_sec actually expires.
func (r *radarEngine) expireOccupiedIfDueLocked(cfg RadarConfig, now time.Time, reason string) map[string]any {
	if !r.occupied || r.occUntil.IsZero() || !now.After(r.occUntil) {
		return nil
	}
	peak := r.occPeak
	r.occupied = false
	r.lastEmptyAt = now
	r.evMotion = 0
	r.evMicro = 0
	r.occPeak = 0
	r.events = append(r.events, RadarEvent{
		Ts:        now.Unix(),
		From:      "occupied",
		To:        "empty",
		PeakScore: math.Round(peak*100) / 100,
		Detail:    fmt.Sprintf("%s且超过 %d 秒，本轮峰值 %.2f", reason, cfg.HoldSec, peak),
	})
	if len(r.events) > radarEventsMax {
		r.events = append([]RadarEvent(nil), r.events[len(r.events)-radarEventsMax:]...)
	}
	if strings.HasPrefix(cfg.WebhookURL, "http://") || strings.HasPrefix(cfg.WebhookURL, "https://") {
		return map[string]any{
			"event":      "csi_presence",
			"state":      "empty",
			"score":      0.0,
			"peak_score": math.Round(peak*100) / 100,
			"iface":      cfg.Iface,
			"target_mac": cfg.TargetMAC,
			"ts":         now.Unix(),
		}
	}
	return nil
}

// suspendRadar disarms CSIMON on iface and blocks re-arming until resumeRadar is called.
// Broadcom wl.ko on PCIe BCM6710 (wl0) can lock up its M2M DMA ring (xfer_to_ddr_fail 100%)
// if wl down / hostapd restart runs while CSIMON is actively armed.
func (s *Server) suspendRadar(iface string) {
	if s.radar == nil || iface == "" {
		return
	}
	re := s.radar
	re.mu.Lock()
	re.suspendCount[iface]++
	active := re.activeIface == iface
	doneCh := re.sessionDone
	mac := re.activeMAC
	if mac == "" {
		cfg := s.getConfig().Radar
		if cfg.Iface == iface {
			mac = cfg.TargetMAC
		}
	}
	re.mu.Unlock()

	re.notifyReload()
	if active && doneCh != nil {
		select {
		case <-doneCh:
		case <-time.After(3 * time.Second):
		}
	}
	if s.isCSIMONActive(iface) {
		s.disarmCSIMON(iface, mac)
	}
}

func (s *Server) resumeRadar(iface string) {
	if s.radar == nil || iface == "" {
		return
	}
	re := s.radar
	re.mu.Lock()
	if re.suspendCount[iface] > 0 {
		re.suspendCount[iface]--
	}
	re.mu.Unlock()
	re.notifyReload()
}

func (s *Server) shutdownRadar() {
	s.suspendRadar("wl0")
	s.suspendRadar("wl1")
}

type csimonStats struct {
	nullCnt    uint64
	ddrCnt     uint64
	ddrFailCnt uint64
	ackFailCnt uint64
}

func parseCSIMONStats(out string) csimonStats {
	var st csimonStats
	inAgg := false
	for _, line := range strings.Split(out, "\n") {
		trim := strings.TrimSpace(line)
		if strings.HasPrefix(trim, "Aggregate Statistics") {
			inAgg = true
			continue
		}
		if strings.HasPrefix(trim, "Per-client Statistics") {
			break
		}
		if !inAgg {
			continue
		}
		fields := strings.Fields(trim)
		for i := 0; i+1 < len(fields); i += 2 {
			v, err := strconv.ParseUint(fields[i+1], 10, 64)
			if err != nil {
				continue
			}
			switch fields[i] {
			case "null_frm_cnt":
				st.nullCnt = v
			case "xfer_to_ddr_cnt":
				st.ddrCnt = v
			case "xfer_to_ddr_fail_cnt":
				st.ddrFailCnt = v
			case "ack_fail_cnt":
				st.ackFailCnt = v
			}
		}
	}
	return st
}

func (s *Server) readCSIMONStats(iface string) csimonStats {
	if iface == "" {
		return csimonStats{}
	}
	return parseCSIMONStats(runCmd(2*time.Second, "wl", "-i", iface, "dump", "csimon"))
}

// pauseForScan blanks CSI ingestion while wl -i <iface> scan hops off-channel
// and clears the sliding ring so pre-scan and post-scan frames never mix.
func (r *radarEngine) pauseForScan(iface string, dur time.Duration) {
	r.mu.Lock()
	if r.activeIface == iface {
		until := time.Now().Add(dur)
		if until.After(r.scanPauseUntil) {
			r.scanPauseUntil = until
		}
		r.ringLen = 0
		r.ringIdx = 0
		r.evMotion = 0
		r.evMicro = 0
		r.lastEvalAt = time.Time{}
	}
	r.mu.Unlock()
}

func (s *Server) detectCSIMONProto(iface string) int {
	out := runCmd(2*time.Second, "wl", "-i", iface, "dump", "csimon")
	for _, line := range strings.Split(out, "\n") {
		if strings.Contains(line, "Netlink subsystem for CSIMON:") {
			parts := strings.Split(line, ":")
			if len(parts) >= 2 {
				if v, err := strconv.Atoi(strings.TrimSpace(parts[len(parts)-1])); err == nil {
					return v
				}
			}
		}
	}
	return -1
}

func (s *Server) isSTAAssociated(iface, mac string) bool {
	if iface == "" || mac == "" {
		return false
	}
	want := strings.ToUpper(mac)
	out := runCmd(2*time.Second, "wl", "-i", iface, "assoclist")
	for _, line := range strings.Split(out, "\n") {
		f := strings.Fields(line)
		if len(f) >= 2 && strings.ToUpper(f[1]) == want {
			return true
		}
	}
	return false
}

func (s *Server) isCSIMONActive(iface string) bool {
	out := runCmd(2*time.Second, "wl", "-i", iface, "dump", "csimon")
	return strings.Contains(out, "CSI Monitor: 1")
}

func (s *Server) armCSIMON(iface, mac string, intervalMs int) {
	if iface == "" || mac == "" {
		return
	}
	if !s.isSTAAssociated(iface, mac) {
		return
	}
	if s.isCSIMONActive(iface) {
		_ = runCmd(3*time.Second, "wl", "-i", iface, "csimon", "disable")
	}
	_ = runCmd(3*time.Second, "wl", "-i", iface, "csimon", "add", mac, strconv.Itoa(intervalMs), "enable")
	_ = runCmd(3*time.Second, "wl", "-i", iface, "csimon", "enable")
}

func (s *Server) disarmCSIMON(iface, mac string) {
	if iface == "" {
		return
	}
	if s.isCSIMONActive(iface) {
		_ = runCmd(3*time.Second, "wl", "-i", iface, "csimon", "disable")
	}
	if mac != "" {
		_ = runCmd(3*time.Second, "wl", "-i", iface, "csimon", "del", mac)
	}
}

func medianFloat64(xs []float64) float64 {
	n := len(xs)
	if n == 0 {
		return 0
	}
	cp := make([]float64, n)
	copy(cp, xs)
	sort.Float64s(cp)
	if n%2 == 1 {
		return cp[n/2]
	}
	return 0.5 * (cp[n/2-1] + cp[n/2])
}

func pctFloat64(xs []float64, p float64) float64 {
	if len(xs) == 0 {
		return 0
	}
	cp := make([]float64, len(xs))
	copy(cp, xs)
	sort.Float64s(cp)
	idx := int(math.Round((p / 100.0) * float64(len(cp)-1)))
	if idx < 0 {
		idx = 0
	}
	if idx >= len(cp) {
		idx = len(cp) - 1
	}
	return cp[idx]
}

func radarIQRFloor(med float64) float64 {
	f := med * 0.25
	if f < 0.35 {
		f = 0.35
	}
	return f
}

func defaultRadarBaseline() RadarBaseline {
	return RadarBaseline{
		Med: [6]float64{3.5, 1.5, 2.5, 0.25, 0.15, 0.25},
		IQR: [6]float64{0.88, 0.38, 0.9, 0.35, 0.35, 0.35},
	}
}

func effectiveRadarBaseline(b *RadarBaseline) RadarBaseline {
	if b == nil {
		return defaultRadarBaseline()
	}
	out := *b
	for i := 0; i < 6; i++ {
		if floor := radarIQRFloor(out.Med[i]); out.IQR[i] < floor {
			out.IQR[i] = floor
		}
	}
	return out
}

func radarWindowParams(intervalMs int) (winLen, smoothLen int) {
	if intervalMs < 100 {
		intervalMs = 200
	}
	winLen = int(math.Round(4000.0 / float64(intervalMs)))
	if winLen < 12 {
		winLen = 12
	}
	if winLen > radarMaxWindow {
		winLen = radarMaxWindow
	}
	smoothLen = int(math.Round(600.0 / float64(intervalMs)))
	if smoothLen < 3 {
		smoothLen = 3
	}
	if smoothLen > 5 {
		smoothLen = 5
	}
	return winLen, smoothLen
}

func wrapDeg(d float64) float64 {
	for d > 180 {
		d -= 360
	}
	for d < -180 {
		d += 360
	}
	return d
}

// sortFloat64InPlace sorts small fixed slices via insertion sort with zero heap allocations.
func sortFloat64InPlace(xs []float64) {
	for i := 1; i < len(xs); i++ {
		v := xs[i]
		j := i - 1
		for j >= 0 && xs[j] > v {
			xs[j+1] = xs[j]
			j--
		}
		xs[j+1] = v
	}
}

func pctSorted(sorted []float64, p float64) float64 {
	n := len(sorted)
	if n == 0 {
		return 0
	}
	idx := int(math.Round((p / 100.0) * float64(n-1)))
	if idx < 0 {
		idx = 0
	}
	if idx >= n {
		idx = n - 1
	}
	return sorted[idx]
}

func lag1ACF(seq []float64) float64 {
	n := len(seq)
	if n < 4 {
		return 0
	}
	var sum float64
	for _, x := range seq {
		sum += x
	}
	mu := sum / float64(n)
	var v, cov float64
	prev := seq[0] - mu
	v += prev * prev
	for i := 1; i < n; i++ {
		cur := seq[i] - mu
		v += cur * cur
		cov += prev * cur
		prev = cur
	}
	if v < 1e-12 {
		return 0
	}
	return cov / v
}

func top4Mean(xs []float64) float64 {
	var top [4]float64
	for _, v := range xs {
		if v > top[3] {
			top[3] = v
			for i := 3; i > 0 && top[i] > top[i-1]; i-- {
				top[i], top[i-1] = top[i-1], top[i]
			}
		}
	}
	return 0.25 * (top[0] + top[1] + top[2] + top[3])
}

// computeRadarFeatures computes the 6 presence features over a chronological window of winLen frames:
//   f[0]: RX0 top-4 subcarrier smoothed P75 amplitude deviation (*1000)
//   f[1]: RX1 top-4 subcarrier smoothed P75 amplitude deviation (*1000)
//   f[2]: Inter-antenna phase top-4 subcarrier smoothed P75 deviation (deg)
//   f[3]: RX0 mean coherent energy = mean_k(max(0, ACF1_raw(a0[k])) * P75_smooth(a0[k])*1000)
//   f[4]: RX1 mean coherent energy = mean_k(max(0, ACF1_raw(a1[k])) * P75_smooth(a1[k])*1000)
//   f[5]: Phase mean coherent energy = mean_k(max(0, ACF1_raw(pdiff[k])) * P75_smooth(pdiff[k]))
func computeRadarFeatures(rawRing, smRing *[radarMaxWindow]radarFrame, ringIdx, winLen int) [6]float64 {
	var colBuf, devBuf, acfBuf [radarMaxWindow]float64
	col := colBuf[:winLen]
	dev := devBuf[:winLen]
	acfSeq := acfBuf[:winLen]

	var a0Sc, a1Sc [14]float64
	var phSc [10]float64
	var cohA0, cohA1, cohPh float64

	start := (ringIdx - winLen + radarMaxWindow) % radarMaxWindow

	for k := 0; k < 14; k++ {
		// RX0 smoothed P75 deviation + raw lag-1 ACF
		for j := 0; j < winLen; j++ {
			idx := (start + j) % radarMaxWindow
			col[j] = smRing[idx].a0[k]
			acfSeq[j] = rawRing[idx].a0[k]
		}
		sortFloat64InPlace(col)
		med0 := pctSorted(col, 50)
		for j := 0; j < winLen; j++ {
			dev[j] = math.Abs(col[j] - med0)
		}
		sortFloat64InPlace(dev)
		p75_0 := pctSorted(dev, 75) * 1000.0
		a0Sc[k] = p75_0
		if r := lag1ACF(acfSeq); r > 0 {
			cohA0 += r * p75_0
		}

		// RX1 smoothed P75 deviation + raw lag-1 ACF
		for j := 0; j < winLen; j++ {
			idx := (start + j) % radarMaxWindow
			col[j] = smRing[idx].a1[k]
			acfSeq[j] = rawRing[idx].a1[k]
		}
		sortFloat64InPlace(col)
		med1 := pctSorted(col, 50)
		for j := 0; j < winLen; j++ {
			dev[j] = math.Abs(col[j] - med1)
		}
		sortFloat64InPlace(dev)
		p75_1 := pctSorted(dev, 75) * 1000.0
		a1Sc[k] = p75_1
		if r := lag1ACF(acfSeq); r > 0 {
			cohA1 += r * p75_1
		}
	}

	// Inter-antenna phase difference across middle 10 subcarriers [2..11]
	for k := 2; k < 12; k++ {
		var sxS, syS, sxR, syR float64
		for j := 0; j < winLen; j++ {
			idx := (start + j) % radarMaxWindow
			ps := smRing[idx].pdiff[k]
			sxS += math.Cos(ps)
			syS += math.Sin(ps)
			pr := rawRing[idx].pdiff[k]
			sxR += math.Cos(pr)
			syR += math.Sin(pr)
		}
		refS := math.Atan2(syS, sxS)
		refR := math.Atan2(syR, sxR)
		for j := 0; j < winLen; j++ {
			idx := (start + j) % radarMaxWindow
			dev[j] = math.Abs(wrapDeg((smRing[idx].pdiff[k] - refS) * (180.0 / math.Pi)))
			acfSeq[j] = wrapDeg((rawRing[idx].pdiff[k] - refR) * (180.0 / math.Pi))
		}
		sortFloat64InPlace(dev)
		p75Ph := pctSorted(dev, 75)
		phSc[k-2] = p75Ph
		if r := lag1ACF(acfSeq); r > 0 {
			cohPh += r * p75Ph
		}
	}

	return [6]float64{
		top4Mean(a0Sc[:]),
		top4Mean(a1Sc[:]),
		top4Mean(phSc[:]),
		cohA0 / 14.0,
		cohA1 / 14.0,
		cohPh / 10.0,
	}
}

func scoreRadarFeatures(f [6]float64, base RadarBaseline) float64 {
	var zs [6]float64
	for c := 0; c < 6; c++ {
		iqr := base.IQR[c]
		if floor := radarIQRFloor(base.Med[c]); iqr < floor {
			iqr = floor
		}
		if d := f[c] - base.Med[c]; d > 0 {
			zs[c] = d / iqr
		}
	}
	// Find top-2 Z-scores (0.65 * max + 0.35 * second_max) with zero allocation
	var z1, z2 float64
	for _, z := range zs {
		if z > z1 {
			z2 = z1
			z1 = z
		} else if z > z2 {
			z2 = z
		}
	}
	return math.Round((0.65*z1+0.35*z2)*100) / 100
}

func radarReentryThresh(cfg RadarConfig) float64 {
	t := math.Round((cfg.MicroThresh+0.45*(cfg.MotionThresh-cfg.MicroThresh))*10) / 10
	if t < cfg.MicroThresh {
		t = cfg.MicroThresh
	}
	if t > cfg.MotionThresh {
		t = cfg.MotionThresh
	}
	return t
}

// evalPresenceFSM updates the decoupled occupancy state machine (caller holds r.mu):
//   - empty -> occupied: strictly requires leaky-bucket motion evidence >= confirm_sec
//     (2x fast-track on strong motion >= 2*motion_thresh). Low-level micro-drift cannot wake empty.
//     Within radarReentryGrace (5m) after dropping from occupied -> empty, sustained moderate
//     activity (>= reentryThresh, halfway between micro and motion) can also accumulate evidence
//     to quickly recover seated occupancy without weakening long-term empty-room immunity.
//   - occupied hold: refreshed immediately by motion, or by >= 0.6s clustered micro-motion
//     (rejects single-frame tail noise while holding seated micro-motion rock-solid).
func (r *radarEngine) evalPresenceFSM(score float64, cfg RadarConfig, now time.Time) (changed bool, fromS, toS, detail string, peak float64) {
	prevOcc := r.occupied
	hold := time.Duration(cfg.HoldSec) * time.Second
	if hold < 5*time.Second {
		hold = 30 * time.Second
	}
	confirmSec := float64(cfg.ConfirmSec)
	if confirmSec < 1 {
		confirmSec = 2
	}
	const microHoldConfirm = 0.6

	dt := float64(cfg.IntervalMs) / 1000.0
	if !r.lastEvalAt.IsZero() {
		dt = now.Sub(r.lastEvalAt).Seconds()
	}
	if dt < 0.05 {
		dt = 0.05
	} else if dt > 1.0 {
		dt = 1.0
	}
	r.lastEvalAt = now

	inGrace := !prevOcc && !r.lastEmptyAt.IsZero() && now.Sub(r.lastEmptyAt) <= radarReentryGrace
	reentryThresh := radarReentryThresh(cfg)

	if score >= cfg.MotionThresh {
		mult := 1.0
		if score >= cfg.MotionThresh*2.0 {
			mult = 2.0
		}
		r.evMotion = math.Min(confirmSec, r.evMotion+dt*mult)
		r.evMicro = math.Min(microHoldConfirm, r.evMicro+dt)
	} else if inGrace && score >= reentryThresh {
		r.evMotion = math.Min(confirmSec, r.evMotion+dt)
		r.evMicro = math.Min(microHoldConfirm, r.evMicro+dt)
	} else if score >= cfg.MicroThresh {
		r.evMotion = math.Max(0, r.evMotion-dt*0.5)
		r.evMicro = math.Min(microHoldConfirm, r.evMicro+dt)
	} else {
		r.evMotion = math.Max(0, r.evMotion-dt*1.0)
		r.evMicro = math.Max(0, r.evMicro-dt*1.0)
	}

	if !prevOcc {
		if r.evMotion >= confirmSec {
			r.occupied = true
			r.occUntil = now.Add(hold)
			r.occPeak = score
			r.evMicro = microHoldConfirm
		}
	} else {
		if score >= cfg.MotionThresh || r.evMicro >= microHoldConfirm {
			r.occUntil = now.Add(hold)
			if score > r.occPeak {
				r.occPeak = score
			}
		} else if now.After(r.occUntil) {
			r.occupied = false
			r.lastEmptyAt = now
			r.evMotion = 0
			r.evMicro = 0
		}
	}

	if r.occupied == prevOcc {
		return false, "", "", "", 0
	}
	if r.occupied {
		r.occPeak = score
		if inGrace && score < cfg.MotionThresh {
			elapsed := int(now.Sub(r.lastEmptyAt).Seconds())
			r.lastEmptyAt = time.Time{}
			return true, "empty", "occupied", fmt.Sprintf("宽限重入触发 %.2f（宽限阈值 %.1f，距无人 %ds）", score, reentryThresh, elapsed), score
		}
		r.lastEmptyAt = time.Time{}
		return true, "empty", "occupied", fmt.Sprintf("触发分数 %.2f（阈值 %.1f）", score, cfg.MotionThresh), score
	}
	peak = r.occPeak
	r.occPeak = 0
	return true, "occupied", "empty", fmt.Sprintf("静止超过 %d 秒，本轮峰值 %.2f", cfg.HoldSec, peak), peak
}

// ingestCSIPayload decodes a 2048-byte Broadcom CSIMON 1.1.0 Netlink payload
// and updates the dual-antenna SNR-normalized presence detector + leaky-bucket FSM.
func (s *Server) ingestCSIPayload(payload []byte) {
	if len(payload) < 0xd0 {
		return
	}
	chanspec := binary.LittleEndian.Uint16(payload[0x10:0x12])
	r0 := int(int8(payload[0x1c]))
	r1 := int(int8(payload[0x1d]))

	var h0, h1 [14]complex128
	var a0, a1 [14]float64
	var norm0Sq, norm1Sq float64
	for i := 0; i < 28; i++ {
		off := 0x60 + i*4
		re := float64(int16(binary.LittleEndian.Uint16(payload[off : off+2])))
		im := float64(int16(binary.LittleEndian.Uint16(payload[off+2 : off+4])))
		z := complex(re, im)
		sc := i / 2
		if i%2 == 0 {
			h0[sc] = z
			amp := cmplx.Abs(z)
			a0[sc] = amp
			norm0Sq += amp * amp
		} else {
			h1[sc] = z
			amp := cmplx.Abs(z)
			a1[sc] = amp
			norm1Sq += amp * amp
		}
	}
	norm0 := math.Sqrt(norm0Sq)
	norm1 := math.Sqrt(norm1Sq)
	if norm0 < 1.0 || norm1 < 1.0 {
		return
	}

	var fr radarFrame
	fr.ts = time.Now()
	for k := 0; k < 14; k++ {
		fr.a0[k] = a0[k] / norm0
		fr.a1[k] = a1[k] / norm1
		// Inter-antenna conjugate phase cancels CFO/SFO/PDD/AGC
		fr.pdiff[k] = cmplx.Phase(h1[k] * cmplx.Conj(h0[k]))
	}

	cfg := s.getConfig().Radar
	winLen, smoothLen := radarWindowParams(cfg.IntervalMs)
	base := effectiveRadarBaseline(cfg.Baseline)
	re := s.radar
	var webhookPayload map[string]any
	var calDoneCfg *RadarConfig
	radarChanged := false

	re.mu.Lock()
	if fr.ts.Before(re.scanPauseUntil) {
		re.mu.Unlock()
		return
	}
	re.frames++
	re.lastFrameAt = fr.ts
	re.rssi0 = r0
	re.rssi1 = r1
	re.chanspec = chanspec

	// Store raw frame and build trailing low-pass smoothed frame (kills white noise & single-frame glitches)
	re.rawRing[re.ringIdx] = fr
	re.ringIdx = (re.ringIdx + 1) % radarMaxWindow
	if re.ringLen < radarMaxWindow {
		re.ringLen++
	}

	m := smoothLen
	if re.ringLen < m {
		m = re.ringLen
	}
	var smFr radarFrame
	smFr.ts = fr.ts
	for k := 0; k < 14; k++ {
		var s0, s1, sx, sy float64
		for j := 0; j < m; j++ {
			idx := (re.ringIdx - 1 - j + radarMaxWindow) % radarMaxWindow
			s0 += re.rawRing[idx].a0[k]
			s1 += re.rawRing[idx].a1[k]
			ph := re.rawRing[idx].pdiff[k]
			sx += math.Cos(ph)
			sy += math.Sin(ph)
		}
		smFr.a0[k] = s0 / float64(m)
		smFr.a1[k] = s1 / float64(m)
		smFr.pdiff[k] = math.Atan2(sy, sx)
	}
	prevSlot := (re.ringIdx - 1 + radarMaxWindow) % radarMaxWindow
	re.smRing[prevSlot] = smFr

	if re.ringLen >= winLen {
		feats := computeRadarFeatures(&re.rawRing, &re.smRing, re.ringIdx, winLen)
		score := scoreRadarFeatures(feats, base)

		re.ampMAD = math.Round((math.Max(feats[0], feats[1])/1000.0)*100000) / 100000
		re.phaseMAD = math.Round(feats[2]*100) / 100
		re.score = score

		// Calibration collection: learn 6-channel empty-room baseline + auto SNR thresholds
		if re.calActive {
			re.calFeats = append(re.calFeats, feats)
			if time.Since(re.calStart) >= re.calDur && len(re.calFeats) >= 20 {
				var newBase RadarBaseline
				nCal := len(re.calFeats)
				col := make([]float64, nCal)
				for c := 0; c < 6; c++ {
					for i := 0; i < nCal; i++ {
						col[i] = re.calFeats[i][c]
					}
					med := pctFloat64(col, 50)
					iqr := pctFloat64(col, 75) - pctFloat64(col, 25)
					if floor := radarIQRFloor(med); iqr < floor {
						iqr = floor
					}
					newBase.Med[c] = math.Round(med*10000) / 10000
					newBase.IQR[c] = math.Round(iqr*10000) / 10000
				}
				if re.chanspec != 0 {
					newBase.Chanspec = fmt.Sprintf("0x%04x", re.chanspec)
				}
				calScores := make([]float64, nCal)
				for i := 0; i < nCal; i++ {
					calScores[i] = scoreRadarFeatures(re.calFeats[i], newBase)
				}
				p50 := pctFloat64(calScores, 50)
				p99 := pctFloat64(calScores, 99)
				mx := pctFloat64(calScores, 100)
				micro := math.Round(math.Max(math.Max(p99*1.45, mx*1.3), 3.5)*10) / 10
				motion := math.Round(math.Max(micro*2.5, 12.0)*10) / 10
				re.calActive = false
				re.calLastMsg = fmt.Sprintf("标定完成（%d 帧，6 通道基线已更新）：空房中位 %.2f / P99 %.2f / 峰值 %.2f → 微动维持 %.1f / 有人触发 %.1f",
					nCal, p50, p99, mx, micro, motion)
				re.calFeats = nil
				cc := cfg
				cc.MicroThresh = micro
				cc.MotionThresh = motion
				nb := newBase
				cc.Baseline = &nb
				calDoneCfg = &cc
			}
		}

		// Instantaneous activity level
		st := "empty"
		if score >= cfg.MotionThresh {
			st = "motion"
		} else if score >= cfg.MicroThresh {
			st = "micro"
		}
		re.state = st

		if changed, fromS, toS, detail, peak := re.evalPresenceFSM(score, cfg, fr.ts); changed {
			radarChanged = true
			re.events = append(re.events, RadarEvent{
				Ts:        fr.ts.Unix(),
				From:      fromS,
				To:        toS,
				PeakScore: math.Round(peak*100) / 100,
				Detail:    detail,
			})
			if len(re.events) > radarEventsMax {
				re.events = append([]RadarEvent(nil), re.events[len(re.events)-radarEventsMax:]...)
			}
			if strings.HasPrefix(cfg.WebhookURL, "http://") || strings.HasPrefix(cfg.WebhookURL, "https://") {
				webhookPayload = map[string]any{
					"event":      "csi_presence",
					"state":      toS,
					"score":      score,
					"peak_score": math.Round(peak*100) / 100,
					"iface":      cfg.Iface,
					"target_mac": cfg.TargetMAC,
					"ts":         fr.ts.Unix(),
				}
			}
		}

		// 1-second live bucket
		secTs := fr.ts.Unix()
		if re.secBucketTs == 0 {
			re.secBucketTs = secTs
			re.secPeak = score
			re.secAmp = re.ampMAD
			re.secPhase = re.phaseMAD
			re.secState = re.state
			re.secOcc = re.occupied
		} else if secTs != re.secBucketTs {
			re.livePoints = append(re.livePoints, RadarPoint{
				Ts:       re.secBucketTs,
				Score:    re.secPeak,
				AmpMAD:   re.secAmp,
				PhaseMAD: re.secPhase,
				State:    re.secState,
				Occupied: re.secOcc,
			})
			if len(re.livePoints) > radarLiveMax {
				re.livePoints = append([]RadarPoint(nil), re.livePoints[len(re.livePoints)-radarLiveMax:]...)
			}
			// Feed 1-minute bucket
			minTs := (re.secBucketTs / 60) * 60
			if re.minBucketTs == 0 {
				re.minBucketTs = minTs
			}
			if minTs != re.minBucketTs {
				re.histPoints = append(re.histPoints, RadarMinute{
					Ts:        re.minBucketTs,
					PeakScore: re.minPeak,
					OccSec:    re.minOccSec,
				})
				if len(re.histPoints) > radarHistMax {
					re.histPoints = append([]RadarMinute(nil), re.histPoints[len(re.histPoints)-radarHistMax:]...)
				}
				re.minBucketTs = minTs
				re.minPeak = 0
				re.minOccSec = 0
			}
			if re.secPeak > re.minPeak {
				re.minPeak = re.secPeak
			}
			if re.secOcc {
				re.minOccSec++
			}
			re.secBucketTs = secTs
			re.secPeak = score
			re.secAmp = re.ampMAD
			re.secPhase = re.phaseMAD
			re.secState = re.state
			re.secOcc = re.occupied
		} else {
			if score > re.secPeak {
				re.secPeak = score
				re.secAmp = re.ampMAD
				re.secPhase = re.phaseMAD
				re.secState = re.state
			}
			if re.occupied {
				re.secOcc = true
			}
		}
	}
	re.mu.Unlock()

	if calDoneCfg != nil {
		_ = s.updateConfig(func(c *PanelConfig) {
			c.Radar.MicroThresh = calDoneCfg.MicroThresh
			c.Radar.MotionThresh = calDoneCfg.MotionThresh
			c.Radar.Baseline = calDoneCfg.Baseline
		})
	}
	if radarChanged {
		s.notifyMQTTRadarEvent()
	}
	if webhookPayload != nil && cfg.WebhookURL != "" {
		go postRadarWebhook(cfg.WebhookURL, webhookPayload)
	}
}

func postRadarWebhook(url string, payload map[string]any) {
	b, err := json.Marshal(payload)
	if err != nil {
		return
	}
	cli := &http.Client{Timeout: 5 * time.Second}
	resp, err := cli.Post(url, "application/json", bytes.NewReader(b))
	if err == nil && resp != nil {
		_, _ = io.Copy(io.Discard, resp.Body)
		_ = resp.Body.Close()
	}
}

func (s *Server) listRadarCandidates() []RadarCandidate {
	var out []RadarCandidate
	for _, iface := range []string{"wl0", "wl1"} {
		raw := runCmd(2*time.Second, "wl", "-i", iface, "assoclist")
		for _, line := range strings.Split(raw, "\n") {
			f := strings.Fields(line)
			if len(f) >= 2 && reMAC.MatchString(f[1]) {
				mac := strings.ToUpper(f[1])
				rssi := 0
				if rStr := strings.TrimSpace(runCmd(1*time.Second, "wl", "-i", iface, "rssi", mac)); rStr != "" {
					if v, err := strconv.Atoi(strings.Fields(rStr)[0]); err == nil {
						rssi = v
					}
				}
				out = append(out, RadarCandidate{
					Iface: iface,
					MAC:   mac,
					Note:  s.displayName(mac),
					RSSI:  rssi,
				})
			}
		}
	}
	return out
}

func (s *Server) handleRadar(w http.ResponseWriter, r *http.Request) {
	cfg := s.getConfig().Radar
	re := s.radar

	proto0 := s.detectCSIMONProto("wl0")
	proto1 := s.detectCSIMONProto("wl1")
	proto := proto0
	if cfg.Iface == "wl1" {
		proto = proto1
	}

	re.mu.Lock()
	live := make([]RadarPoint, len(re.livePoints))
	copy(live, re.livePoints)
	hist := make([]RadarMinute, len(re.histPoints))
	copy(hist, re.histPoints)
	events := make([]RadarEvent, len(re.events))
	copy(events, re.events)
	calActive := re.calActive
	calPct := 0
	if calActive && re.calDur > 0 {
		calPct = int(math.Min(100, (time.Since(re.calStart).Seconds()/re.calDur.Seconds())*100))
	}
	curChanspec := fmt.Sprintf("0x%04x", re.chanspec)
	baselineStale := cfg.Baseline != nil && cfg.Baseline.Chanspec != "" && re.chanspec != 0 && cfg.Baseline.Chanspec != curChanspec
	snap := map[string]any{
		"supported":      proto0 > 0 || proto1 > 0,
		"proto_wl0":      proto0,
		"proto_wl1":      proto1,
		"netlink_proto":  proto,
		"config":         cfg,
		"state":          re.state,
		"occupied":       re.occupied,
		"score":          re.score,
		"amp_mad":        re.ampMAD,
		"phase_mad":      re.phaseMAD,
		"rssi0":          re.rssi0,
		"rssi1":          re.rssi1,
		"chanspec":       curChanspec,
		"baseline_stale": baselineStale,
		"frames":         re.frames,
		"last_frame_sec": int(time.Since(re.lastFrameAt).Seconds()),
		"live_points":    live,
		"hist_points":    hist,
		"events":         events,
		"calibrating":    calActive,
		"cal_progress":   calPct,
		"cal_message":    re.calLastMsg,
		"fault_message":  re.faultMsg,
	}
	if re.lastFrameAt.IsZero() {
		snap["last_frame_sec"] = -1
	}
	re.mu.Unlock()

	snap["candidates"] = s.listRadarCandidates()
	writeJSON(w, r, http.StatusOK, snap)
}

func (s *Server) handleRadarConfig(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req RadarConfig
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	if req.Iface != "wl0" && req.Iface != "wl1" {
		req.Iface = "wl0"
	}
	req.TargetMAC = strings.ToUpper(strings.TrimSpace(req.TargetMAC))
	if req.Enabled && !reMAC.MatchString(req.TargetMAC) {
		writeErr(w, r, http.StatusBadRequest, "开启雷达前请先选择或填写有效的目标终端 MAC")
		return
	}
	if req.IntervalMs < 100 || req.IntervalMs > 2000 {
		req.IntervalMs = 200
	}
	if req.MicroThresh <= 0 {
		req.MicroThresh = 3.5
	}
	if req.MotionThresh <= req.MicroThresh {
		req.MotionThresh = math.Round(math.Max(req.MicroThresh*2.5, 12.0)*10) / 10
	}
	if req.ConfirmSec < 1 || req.ConfirmSec > 15 {
		req.ConfirmSec = 2
	}
	if req.HoldSec < 5 || req.HoldSec > 600 {
		req.HoldSec = 30
	}
	req.WebhookURL = strings.TrimSpace(req.WebhookURL)
	if req.WebhookURL != "" && !strings.HasPrefix(req.WebhookURL, "http://") && !strings.HasPrefix(req.WebhookURL, "https://") {
		writeErr(w, r, http.StatusBadRequest, "Webhook 仅支持 http:// 或 https://")
		return
	}
	if err := s.updateConfig(func(c *PanelConfig) {
		if req.Baseline == nil && c.Radar.Baseline != nil && c.Radar.Iface == req.Iface && c.Radar.TargetMAC == req.TargetMAC {
			b := *c.Radar.Baseline
			req.Baseline = &b
		}
		c.Radar = req
	}); err != nil {
		writeErr(w, r, http.StatusInternalServerError, err.Error())
		return
	}
	s.radar.mu.Lock()
	s.radar.faultMsg = ""
	s.radar.mu.Unlock()
	s.radar.notifyReload()
	s.notifyMQTTPush()
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "config": req})
}

func (s *Server) handleRadarCalibrate(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	cfg := s.getConfig().Radar
	if !cfg.Enabled || !reMAC.MatchString(cfg.TargetMAC) {
		writeErr(w, r, http.StatusBadRequest, "请先启用雷达并指定在线目标终端，再执行空房标定")
		return
	}
	var req struct {
		DurationSec int `json:"duration_sec"`
	}
	_ = json.NewDecoder(io.LimitReader(r.Body, 1024)).Decode(&req)
	dur := req.DurationSec
	if dur < 10 || dur > 120 {
		dur = 30
	}
	s.radar.mu.Lock()
	if s.radar.calActive {
		s.radar.mu.Unlock()
		writeErr(w, r, http.StatusConflict, "标定已在进行中")
		return
	}
	s.radar.calActive = true
	s.radar.calStart = time.Now()
	s.radar.calDur = time.Duration(dur) * time.Second
	s.radar.calFeats = nil
	s.radar.calLastMsg = fmt.Sprintf("正在进行 %d 秒空房底噪标定，请保持房间静止…", dur)
	s.radar.mu.Unlock()
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "duration_sec": dur})
}

func (s *Server) handleRadarTestWebhook(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		URL string `json:"url"`
	}
	_ = json.NewDecoder(io.LimitReader(r.Body, 2048)).Decode(&req)
	url := strings.TrimSpace(req.URL)
	if url == "" {
		url = s.getConfig().Radar.WebhookURL
	}
	if !strings.HasPrefix(url, "http://") && !strings.HasPrefix(url, "https://") {
		writeErr(w, r, http.StatusBadRequest, "请先填写有效的 http:// 或 https:// Webhook 地址")
		return
	}
	cfg := s.getConfig().Radar
	s.radar.mu.Lock()
	curScore := s.radar.score
	s.radar.mu.Unlock()
	payload := map[string]any{
		"event":      "csi_presence_test",
		"state":      "test",
		"score":      curScore,
		"iface":      cfg.Iface,
		"target_mac": cfg.TargetMAC,
		"ts":         time.Now().Unix(),
	}
	b, _ := json.Marshal(payload)
	t0 := time.Now()
	cli := &http.Client{Timeout: 6 * time.Second}
	resp, err := cli.Post(url, "application/json", bytes.NewReader(b))
	if err != nil {
		writeErr(w, r, http.StatusBadGateway, fmt.Sprintf("推送失败: %v", err))
		return
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
	_ = resp.Body.Close()
	writeJSON(w, r, http.StatusOK, map[string]any{
		"ok":          resp.StatusCode >= 200 && resp.StatusCode < 300,
		"status_code": resp.StatusCode,
		"elapsed_ms":  time.Since(t0).Milliseconds(),
		"message":     fmt.Sprintf("HTTP %d（耗时 %dms）", resp.StatusCode, time.Since(t0).Milliseconds()),
	})
}

var errRadarReload = errors.New("radar reload requested")
