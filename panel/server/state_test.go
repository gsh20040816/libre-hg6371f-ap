package main

import (
	"path/filepath"
	"testing"
	"time"
)

func TestVolatileStateRoundTrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.json.gz")
	now := time.Now()

	s1 := &Server{
		radar:  newRadarEngine(),
		stamon: newStaMonEngine(),
	}
	s1.trafHist = []TrafficSample{{TS: now.Unix(), LanRx: 1234, LanTx: 5678}}
	s1.rfSamples = []RFSample{{TS: now.Unix(), W2Noise: -80, SocTemp: 66.5}}
	s1.rfEvents = []RFEvent{{TS: now.Unix(), Kind: "join", MAC: "AA:BB:CC:DD:EE:FF", Band: "5G"}}
	s1.radar.frames = 4200
	s1.radar.histPoints = []RadarMinute{{Ts: now.Unix() - 60, PeakScore: 2.5, OccSec: 0}}
	s1.radar.livePoints = []RadarPoint{{Ts: now.Unix(), Score: 1.2, PhaseMAD: 1.5, State: "empty"}}
	s1.radar.events = []RadarEvent{{Ts: now.Unix() - 10, From: "empty", To: "occupied", PeakScore: 25.0}}
	s1.radar.occupied = true
	s1.radar.occUntil = now.Add(25 * time.Second)
	s1.radar.occPeak = 25.0
	s1.stamon.states["11:22:33:44:55:66"] = &StaMonState{
		MAC: "11:22:33:44:55:66", Label: "Phone", Present: true, BestRSSI: -62, BestBand: "5G", LastSeenTs: now.Unix(), SeenCount: 9,
	}
	s1.stamon.events = []StaMonEvent{{Ts: now.Unix(), MAC: "11:22:33:44:55:66", Present: true, RSSI: -62, Band: "5G"}}
	s1.stamon.prevTotal["wl1"]["11:22:33:44:55:66"] = 88
	s1.stamon.prevKnown["wl1"]["11:22:33:44:55:66"] = true

	s1.saveVolatileStateTo(path)

	s2 := &Server{
		radar:  newRadarEngine(),
		stamon: newStaMonEngine(),
	}
	s2.loadVolatileStateFrom(path)

	if len(s2.trafHist) != 1 || s2.trafHist[0].LanRx != 1234 {
		t.Fatalf("trafHist mismatch: %+v", s2.trafHist)
	}
	if len(s2.rfSamples) != 1 || len(s2.rfEvents) != 1 || s2.rfSamples[0].W2Noise != -80 {
		t.Fatalf("rf mismatch: %+v %+v", s2.rfSamples, s2.rfEvents)
	}
	if s2.radar.frames != 4200 || len(s2.radar.histPoints) != 1 || len(s2.radar.livePoints) != 1 || len(s2.radar.events) != 1 {
		t.Fatalf("radar history mismatch: frames=%d hist=%d live=%d ev=%d", s2.radar.frames, len(s2.radar.histPoints), len(s2.radar.livePoints), len(s2.radar.events))
	}
	if !s2.radar.occupied || s2.radar.occPeak != 25.0 {
		t.Fatalf("radar occupancy not preserved across fast restart")
	}
	st := s2.stamon.states["11:22:33:44:55:66"]
	if st == nil || !st.Present || st.SeenCount != 9 || s2.stamon.prevTotal["wl1"]["11:22:33:44:55:66"] != 88 || !s2.stamon.prevKnown["wl1"]["11:22:33:44:55:66"] {
		t.Fatalf("stamon state mismatch: %+v", st)
	}

	// Expired occupancy must NOT be restored as occupied, but recent lastEmptyAt is preserved
	s1.radar.occUntil = now.Add(-10 * time.Second)
	s1.radar.lastEmptyAt = now.Add(-30 * time.Second)
	s1.saveVolatileStateTo(path)
	s3 := &Server{radar: newRadarEngine(), stamon: newStaMonEngine()}
	s3.loadVolatileStateFrom(path)
	if s3.radar.occupied {
		t.Fatalf("expired occupancy should reset to empty on load")
	}
	if s3.radar.lastEmptyAt.IsZero() {
		t.Fatalf("recent lastEmptyAt should be restored for reentry grace")
	}
}
