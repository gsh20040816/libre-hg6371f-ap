package main

import (
	"bufio"
	"encoding/json"
	"math"
	"math/cmplx"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestParseCSIMONStats(t *testing.T) {
	sample := `CSI Monitor: 1
Number of clients: 1

Aggregate Statistics
------------------------
null_frm_cnt 5764 xfer_to_usr_cnt 0 xfer_to_ddr_cnt 0 ack_fail_cnt 52
rec_ovfl_cnt 0 xfer_to_ddr_fail_cnt 5453 xfer_to_usr_fail_cnt 0

Per-client Statistics
------------------------
f0:c9:d1:ec:75:ff	 monitor_interval 200 milliseconds
null_frm_cnt 63 xfer_to_ddr_cnt 0 ack_fail_cnt 4
`
	st := parseCSIMONStats(sample)
	if st.nullCnt != 5764 || st.ddrCnt != 0 || st.ddrFailCnt != 5453 || st.ackFailCnt != 52 {
		t.Fatalf("unexpected stats: %+v", st)
	}
}

func TestRadarScoringAndReplay(t *testing.T) {
	// Basic sanity: constant frames yield zero score
	var rawRing, smRing [radarMaxWindow]radarFrame
	for i := 0; i < 20; i++ {
		for k := 0; k < 14; k++ {
			rawRing[i].a0[k] = 1.0 / math.Sqrt(14)
			rawRing[i].a1[k] = 1.0 / math.Sqrt(14)
			rawRing[i].pdiff[k] = 0.5
		}
		smRing[i] = rawRing[i]
	}
	f := computeRadarFeatures(&rawRing, &smRing, 20, 20)
	sc := scoreRadarFeatures(f, defaultRadarBaseline())
	if sc != 0 {
		t.Fatalf("expected 0 score on constant CSI, got %.2f (feats=%v)", sc, f)
	}

	// Optional replay against recorded real-world JSONL datasets when CSI_REPLAY_DIR is set
	dir := os.Getenv("CSI_REPLAY_DIR")
	if dir == "" {
		return
	}
	type rawRec struct {
		TsMs int64     `json:"ts_ms"`
		IQ   [56]int16 `json:"iq"`
	}
	load := func(name string) []radarFrame {
		fp, err := os.Open(filepath.Join(dir, name+".jsonl"))
		if err != nil {
			t.Fatalf("open %s: %v", name, err)
		}
		defer fp.Close()
		var out []radarFrame
		scanner := bufio.NewScanner(fp)
		for scanner.Scan() {
			var r rawRec
			if err := json.Unmarshal(scanner.Bytes(), &r); err != nil {
				continue
			}
			var h0, h1 [14]complex128
			var a0, a1 [14]float64
			var n0sq, n1sq float64
			for i := 0; i < 28; i++ {
				z := complex(float64(r.IQ[i*2]), float64(r.IQ[i*2+1]))
				sc := i / 2
				if i%2 == 0 {
					h0[sc] = z
					amp := cmplx.Abs(z)
					a0[sc] = amp
					n0sq += amp * amp
				} else {
					h1[sc] = z
					amp := cmplx.Abs(z)
					a1[sc] = amp
					n1sq += amp * amp
				}
			}
			n0, n1 := math.Sqrt(n0sq), math.Sqrt(n1sq)
			if n0 < 1 || n1 < 1 {
				continue
			}
			var fr radarFrame
			fr.ts = time.UnixMilli(r.TsMs)
			for k := 0; k < 14; k++ {
				fr.a0[k] = a0[k] / n0
				fr.a1[k] = a1[k] / n1
				fr.pdiff[k] = cmplx.Phase(h1[k] * cmplx.Conj(h0[k]))
			}
			out = append(out, fr)
		}
		return out
	}

	evalFeats := func(frs []radarFrame) (tsSec []float64, feats [][6]float64) {
		var rawR, smR [radarMaxWindow]radarFrame
		idx, ln := 0, 0
		t0 := frs[0].ts
		for _, fr := range frs {
			rawR[idx] = fr
			idx = (idx + 1) % radarMaxWindow
			if ln < radarMaxWindow {
				ln++
			}
			m := 3
			if ln < m {
				m = ln
			}
			var sm radarFrame
			sm.ts = fr.ts
			for k := 0; k < 14; k++ {
				var s0, s1, sx, sy float64
				for j := 0; j < m; j++ {
					slot := (idx - 1 - j + radarMaxWindow) % radarMaxWindow
					s0 += rawR[slot].a0[k]
					s1 += rawR[slot].a1[k]
					ph := rawR[slot].pdiff[k]
					sx += math.Cos(ph)
					sy += math.Sin(ph)
				}
				sm.a0[k] = s0 / float64(m)
				sm.a1[k] = s1 / float64(m)
				sm.pdiff[k] = math.Atan2(sy, sx)
			}
			smR[(idx-1+radarMaxWindow)%radarMaxWindow] = sm
			if ln >= 20 {
				tsSec = append(tsSec, fr.ts.Sub(t0).Seconds())
				feats = append(feats, computeRadarFeatures(&rawR, &smR, idx, 20))
			}
		}
		return
	}

	eTs, eFeats := evalFeats(load("empty"))
	var pureEmpty [][6]float64
	for i, tSec := range eTs {
		if tSec >= 15.0 {
			pureEmpty = append(pureEmpty, eFeats[i])
		}
	}
	var base RadarBaseline
	col := make([]float64, len(pureEmpty))
	for c := 0; c < 6; c++ {
		for i := range pureEmpty {
			col[i] = pureEmpty[i][c]
		}
		med := pctFloat64(col, 50)
		iqr := pctFloat64(col, 75) - pctFloat64(col, 25)
		if floor := radarIQRFloor(med); iqr < floor {
			iqr = floor
		}
		base.Med[c] = math.Round(med*10000) / 10000
		base.IQR[c] = math.Round(iqr*10000) / 10000
	}
	eScores := make([]float64, len(pureEmpty))
	for i, fv := range pureEmpty {
		eScores[i] = scoreRadarFeatures(fv, base)
	}
	micro := math.Round(math.Max(math.Max(pctFloat64(eScores, 99)*1.45, pctFloat64(eScores, 100)*1.3), 3.5)*10) / 10
	motion := math.Round(math.Max(micro*2.5, 12.0)*10) / 10
	t.Logf("Go baseline=%+v micro=%.1f motion=%.1f", base, micro, motion)
	t.Logf("EMPTY  p50=%.2f p99=%.2f max=%.2f", pctFloat64(eScores, 50), pctFloat64(eScores, 99), pctFloat64(eScores, 100))

	cfg := RadarConfig{IntervalMs: 200, MicroThresh: micro, MotionThresh: motion, ConfirmSec: 2, HoldSec: 30}
	simFSM := func(ts []float64, scores []float64, startOcc bool) (occPct float64, wakeSec float64, drops int) {
		re := newRadarEngine()
		t0 := time.Unix(1700000000, 0)
		if startOcc {
			re.occupied = true
			re.occUntil = t0.Add(time.Duration(cfg.HoldSec) * time.Second)
		}
		wakeSec = -1
		occCnt := 0
		for i, sc := range scores {
			now := t0.Add(time.Duration(ts[i] * float64(time.Second)))
			changed, _, toS, _, _ := re.evalPresenceFSM(sc, cfg, now)
			if changed && toS == "occupied" && wakeSec < 0 {
				wakeSec = ts[i] - ts[0]
			}
			if changed && toS == "empty" {
				drops++
			}
			if re.occupied {
				occCnt++
			}
		}
		return 100 * float64(occCnt) / float64(len(scores)), wakeSec, drops
	}

	var pureEmptyTs []float64
	for _, tSec := range eTs {
		if tSec >= 15.0 {
			pureEmptyTs = append(pureEmptyTs, tSec)
		}
	}
	eOcc, _, _ := simFSM(pureEmptyTs, eScores, false)

	sTs, sFeats := evalFeats(load("seated"))
	sScores := make([]float64, len(sFeats))
	sHitMicro, sHitMotion := 0, 0
	for i, fv := range sFeats {
		sc := scoreRadarFeatures(fv, base)
		sScores[i] = sc
		if sc >= micro {
			sHitMicro++
		}
		if sc >= motion {
			sHitMotion++
		}
	}
	sOccHold, _, sDrops := simFSM(sTs, sScores, true)
	t.Logf("SEATED p50=%.2f p75=%.2f max=%.2f (>micro=%.1f%% >motion=%.1f%% hold=%.1f%% drops=%d)",
		pctFloat64(sScores, 50), pctFloat64(sScores, 75), pctFloat64(sScores, 100),
		100*float64(sHitMicro)/float64(len(sScores)), 100*float64(sHitMotion)/float64(len(sScores)),
		sOccHold, sDrops)

	mTs, mFeats := evalFeats(load("moving"))
	mScores := make([]float64, len(mFeats))
	mHit := 0
	for i, fv := range mFeats {
		sc := scoreRadarFeatures(fv, base)
		mScores[i] = sc
		if sc >= motion {
			mHit++
		}
	}
	mOcc, mWake, _ := simFSM(mTs, mScores, false)
	t.Logf("MOVING p10=%.2f p50=%.2f max=%.2f (>motion=%.1f%% wake=%.2fs occ=%.1f%%)",
		pctFloat64(mScores, 10), pctFloat64(mScores, 50), pctFloat64(mScores, 100),
		100*float64(mHit)/float64(len(mScores)), mWake, mOcc)

	if pctFloat64(eScores, 100) >= micro || eOcc > 0 {
		t.Fatalf("empty room false trigger: max=%.2f micro=%.1f occ=%.1f%%", pctFloat64(eScores, 100), micro, eOcc)
	}
	if sDrops > 0 || sOccHold < 100.0 {
		t.Fatalf("seated hold dropped: occ=%.1f%% drops=%d", sOccHold, sDrops)
	}
	if sHitMicro == 0 || mHit < len(mScores) || mWake < 0 || mWake > 2.0 {
		t.Fatalf("unexpected detection drop: seated_micro=%d moving_hit=%d/%d wake=%.2fs", sHitMicro, mHit, len(mScores), mWake)
	}
}

func TestRadarFSMDecoupled(t *testing.T) {
	cfg := RadarConfig{IntervalMs: 200, MicroThresh: 3.5, MotionThresh: 12.0, ConfirmSec: 2, HoldSec: 30}
	re := newRadarEngine()
	now := time.Unix(1700000000, 0)

	// 1. Sustained slow drift (score 4..11) for 60s must NEVER wake empty -> occupied
	for i := 0; i < 300; i++ {
		now = now.Add(200 * time.Millisecond)
		sc := 4.0 + float64(i%8) // 4..11
		re.evalPresenceFSM(sc, cfg, now)
		if re.occupied {
			t.Fatalf("slow micro drift (%.1f) falsely woke empty at frame %d", sc, i)
		}
	}

	// 2. Strong walking motion (score 50) wakes within ~1.0s (2x fast-track)
	woke := false
	for i := 0; i < 10; i++ {
		now = now.Add(200 * time.Millisecond)
		if changed, _, toS, _, _ := re.evalPresenceFSM(50.0, cfg, now); changed && toS == "occupied" {
			woke = true
			break
		}
	}
	if !woke {
		t.Fatalf("strong motion failed to wake occupied within 2s")
	}

	// 3. Clustered seated micro-motion (1.0s burst every 20s) keeps occupied indefinitely
	for cycle := 0; cycle < 3; cycle++ {
		for i := 0; i < 95; i++ { // 19s quiet
			now = now.Add(200 * time.Millisecond)
			re.evalPresenceFSM(1.0, cfg, now)
			if !re.occupied {
				t.Fatalf("dropped occupied prematurely during quiet interval cycle=%d i=%d", cycle, i)
			}
		}
		for i := 0; i < 5; i++ { // 1s micro-motion burst (score 5.0)
			now = now.Add(200 * time.Millisecond)
			re.evalPresenceFSM(5.0, cfg, now)
		}
	}

	// 4. Isolated single-frame micro spikes every 15s must NOT prevent occupied -> empty expiration
	dropped := false
	for i := 0; i < 200; i++ { // 40s with isolated 1-frame spikes at 15s and 30s
		now = now.Add(200 * time.Millisecond)
		sc := 1.0
		if i == 75 || i == 150 {
			sc = 5.0
		}
		if changed, _, toS, _, _ := re.evalPresenceFSM(sc, cfg, now); changed && toS == "empty" {
			dropped = true
			break
		}
	}
	if !dropped {
		t.Fatalf("isolated 1-frame micro spikes kept occupied alive past hold_sec")
	}
}

func TestRadarFSMReentryGrace(t *testing.T) {
	cfg := RadarConfig{IntervalMs: 200, MicroThresh: 3.5, MotionThresh: 12.0, ConfirmSec: 2, HoldSec: 30}
	if got := radarReentryThresh(cfg); got != 7.3 {
		t.Fatalf("expected reentryThresh=7.3, got %.1f", got)
	}

	re := newRadarEngine()
	now := time.Unix(1700000000, 0)

	// Start occupied, then let hold_sec expire -> occupied -> empty sets lastEmptyAt
	re.occupied = true
	re.occUntil = now.Add(30 * time.Second)
	re.occPeak = 20.0
	for i := 0; i < 160; i++ {
		now = now.Add(200 * time.Millisecond)
		re.evalPresenceFSM(1.0, cfg, now)
		if !re.occupied {
			break
		}
	}
	if re.occupied || re.lastEmptyAt.IsZero() {
		t.Fatalf("expected drop to empty with lastEmptyAt set: occ=%v lastEmptyAt=%v", re.occupied, re.lastEmptyAt)
	}

	// 1. Within grace window: low drift (5.5 < 7.3) and isolated 1-frame spikes (9.0) must NOT re-enter
	for i := 0; i < 50; i++ {
		now = now.Add(200 * time.Millisecond)
		sc := 5.5
		if i == 20 {
			sc = 9.0
		}
		re.evalPresenceFSM(sc, cfg, now)
		if re.occupied {
			t.Fatalf("low drift or 1-frame spike falsely re-entered during grace at i=%d", i)
		}
	}

	// 2. Within grace window (at +60s): sustained moderate seated motion (8.5 for 2.2s) MUST re-enter
	now = re.lastEmptyAt.Add(60 * time.Second)
	re.lastEvalAt = now.Add(-200 * time.Millisecond)
	reentered := false
	for i := 0; i < 12; i++ {
		now = now.Add(200 * time.Millisecond)
		if changed, _, toS, _, _ := re.evalPresenceFSM(8.5, cfg, now); changed && toS == "occupied" {
			reentered = true
			break
		}
	}
	if !reentered {
		t.Fatalf("sustained moderate motion (8.5) failed to re-enter within grace window")
	}

	// 3. Drop to empty again, then advance past 5m grace window: sustained 9.5 must NOT wake empty
	for i := 0; i < 200; i++ {
		now = now.Add(200 * time.Millisecond)
		re.evalPresenceFSM(1.0, cfg, now)
		if !re.occupied {
			break
		}
	}
	if re.occupied {
		t.Fatalf("failed to drop to empty second time")
	}
	now = re.lastEmptyAt.Add(radarReentryGrace + 5*time.Second)
	re.lastEvalAt = now.Add(-200 * time.Millisecond)
	for i := 0; i < 30; i++ {
		now = now.Add(200 * time.Millisecond)
		re.evalPresenceFSM(9.5, cfg, now)
		if re.occupied {
			t.Fatalf("moderate motion (9.5) woke empty AFTER grace window expired at i=%d", i)
		}
	}
}
