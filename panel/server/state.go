package main

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"log"
	"os"
	"time"
)

const (
	volatileStateFile    = "/tmp/ap-panel-state.json.gz" // RAM tmpfs: survives ap-panel restarts with zero flash wear
	volatileStateVersion = 1
)

type volatileRadarState struct {
	LivePoints  []RadarPoint  `json:"live_points,omitempty"`
	HistPoints  []RadarMinute `json:"hist_points,omitempty"`
	Events      []RadarEvent  `json:"events,omitempty"`
	Frames      uint64        `json:"frames,omitempty"`
	Occupied    bool          `json:"occupied,omitempty"`
	OccUntilTs  int64         `json:"occ_until_ts,omitempty"`
	OccPeak     float64       `json:"occ_peak,omitempty"`
	LastEmptyTs int64         `json:"last_empty_ts,omitempty"`
	MinBucketTs int64         `json:"min_bucket_ts,omitempty"`
	MinPeak     float64       `json:"min_peak,omitempty"`
	MinOccSec   int           `json:"min_occ_sec,omitempty"`
	CalLastMsg  string        `json:"cal_last_msg,omitempty"`
}

type volatileStaMonState struct {
	States    map[string]*StaMonState      `json:"states,omitempty"`
	Events    []StaMonEvent                `json:"events,omitempty"`
	PrevTotal map[string]map[string]uint64 `json:"prev_total,omitempty"`
	PrevKnown map[string]map[string]bool   `json:"prev_known,omitempty"`
}

type volatilePanelState struct {
	Version   int                 `json:"version"`
	SavedAt   int64               `json:"saved_at"`
	TrafHist  []TrafficSample     `json:"traf_hist,omitempty"`
	RFSamples []RFSample          `json:"rf_samples,omitempty"`
	RFEvents  []RFEvent           `json:"rf_events,omitempty"`
	Radar     volatileRadarState  `json:"radar"`
	StaMon    volatileStaMonState `json:"stamon"`
}

func (s *Server) saveVolatileState() {
	s.saveVolatileStateTo(volatileStateFile)
}

func (s *Server) saveVolatileStateTo(path string) {
	st := volatilePanelState{
		Version: volatileStateVersion,
		SavedAt: time.Now().Unix(),
	}

	s.trafMu.Lock()
	if len(s.trafHist) > 0 {
		st.TrafHist = append([]TrafficSample(nil), s.trafHist...)
	}
	s.trafMu.Unlock()

	s.rfMu.Lock()
	if len(s.rfSamples) > 0 {
		st.RFSamples = append([]RFSample(nil), s.rfSamples...)
	}
	if len(s.rfEvents) > 0 {
		st.RFEvents = append([]RFEvent(nil), s.rfEvents...)
	}
	s.rfMu.Unlock()

	if s.radar != nil {
		re := s.radar
		re.mu.Lock()
		st.Radar = volatileRadarState{
			LivePoints:  append([]RadarPoint(nil), re.livePoints...),
			HistPoints:  append([]RadarMinute(nil), re.histPoints...),
			Events:      append([]RadarEvent(nil), re.events...),
			Frames:      re.frames,
			Occupied:    re.occupied,
			OccPeak:     re.occPeak,
			MinBucketTs: re.minBucketTs,
			MinPeak:     re.minPeak,
			MinOccSec:   re.minOccSec,
			CalLastMsg:  re.calLastMsg,
		}
		if !re.occUntil.IsZero() {
			st.Radar.OccUntilTs = re.occUntil.Unix()
		}
		if !re.lastEmptyAt.IsZero() {
			st.Radar.LastEmptyTs = re.lastEmptyAt.Unix()
		}
		re.mu.Unlock()
	}

	if s.stamon != nil {
		eng := s.stamon
		eng.mu.Lock()
		statesCopy := make(map[string]*StaMonState, len(eng.states))
		for k, v := range eng.states {
			if v != nil {
				cp := *v
				statesCopy[k] = &cp
			}
		}
		ptCopy := make(map[string]map[string]uint64, len(eng.prevTotal))
		for ifc, m := range eng.prevTotal {
			inner := make(map[string]uint64, len(m))
			for mac, val := range m {
				inner[mac] = val
			}
			ptCopy[ifc] = inner
		}
		pkCopy := make(map[string]map[string]bool, len(eng.prevKnown))
		for ifc, m := range eng.prevKnown {
			inner := make(map[string]bool, len(m))
			for mac, val := range m {
				inner[mac] = val
			}
			pkCopy[ifc] = inner
		}
		st.StaMon = volatileStaMonState{
			States:    statesCopy,
			Events:    append([]StaMonEvent(nil), eng.events...),
			PrevTotal: ptCopy,
			PrevKnown: pkCopy,
		}
		eng.mu.Unlock()
	}

	raw, err := json.Marshal(st)
	if err != nil {
		return
	}
	var buf bytes.Buffer
	gw := gzip.NewWriter(&buf)
	if _, err := gw.Write(raw); err != nil {
		_ = gw.Close()
		return
	}
	if err := gw.Close(); err != nil {
		return
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, buf.Bytes(), 0600); err != nil {
		return
	}
	_ = os.Rename(tmp, path)
}

func (s *Server) loadVolatileState() {
	s.loadVolatileStateFrom(volatileStateFile)
}

func (s *Server) loadVolatileStateFrom(path string) {
	data, err := os.ReadFile(path)
	if err != nil || len(data) == 0 {
		return
	}
	gr, err := gzip.NewReader(bytes.NewReader(data))
	if err != nil {
		return
	}
	defer gr.Close()

	var st volatilePanelState
	if err := json.NewDecoder(gr).Decode(&st); err != nil || st.Version != volatileStateVersion {
		return
	}
	now := time.Now()

	if len(st.TrafHist) > 0 {
		if len(st.TrafHist) > 120 {
			st.TrafHist = st.TrafHist[len(st.TrafHist)-120:]
		}
		s.trafMu.Lock()
		s.trafHist = append([]TrafficSample(nil), st.TrafHist...)
		s.trafMu.Unlock()
	}

	if len(st.RFSamples) > 0 || len(st.RFEvents) > 0 {
		if len(st.RFSamples) > maxRFSamples {
			st.RFSamples = st.RFSamples[len(st.RFSamples)-maxRFSamples:]
		}
		if len(st.RFEvents) > maxRFEvents {
			st.RFEvents = st.RFEvents[len(st.RFEvents)-maxRFEvents:]
		}
		s.rfMu.Lock()
		s.rfSamples = append([]RFSample(nil), st.RFSamples...)
		s.rfEvents = append([]RFEvent(nil), st.RFEvents...)
		s.rfMu.Unlock()
	}

	if s.radar != nil {
		re := s.radar
		re.mu.Lock()
		if len(st.Radar.LivePoints) > radarLiveMax {
			st.Radar.LivePoints = st.Radar.LivePoints[len(st.Radar.LivePoints)-radarLiveMax:]
		}
		if len(st.Radar.HistPoints) > radarHistMax {
			st.Radar.HistPoints = st.Radar.HistPoints[len(st.Radar.HistPoints)-radarHistMax:]
		}
		if len(st.Radar.Events) > radarEventsMax {
			st.Radar.Events = st.Radar.Events[len(st.Radar.Events)-radarEventsMax:]
		}
		re.livePoints = append([]RadarPoint(nil), st.Radar.LivePoints...)
		re.histPoints = append([]RadarMinute(nil), st.Radar.HistPoints...)
		re.events = append([]RadarEvent(nil), st.Radar.Events...)
		re.frames = st.Radar.Frames
		re.minBucketTs = st.Radar.MinBucketTs
		re.minPeak = st.Radar.MinPeak
		re.minOccSec = st.Radar.MinOccSec
		re.calLastMsg = st.Radar.CalLastMsg
		if st.Radar.Occupied && st.Radar.OccUntilTs > now.Unix() {
			re.occupied = true
			re.occUntil = time.Unix(st.Radar.OccUntilTs, 0)
			re.occPeak = st.Radar.OccPeak
		}
		if !re.occupied && st.Radar.LastEmptyTs > 0 && now.Sub(time.Unix(st.Radar.LastEmptyTs, 0)) <= radarReentryGrace {
			re.lastEmptyAt = time.Unix(st.Radar.LastEmptyTs, 0)
		}
		re.mu.Unlock()
	}

	if s.stamon != nil {
		eng := s.stamon
		eng.mu.Lock()
		for k, v := range st.StaMon.States {
			if v != nil {
				cp := *v
				eng.states[k] = &cp
			}
		}
		if len(st.StaMon.Events) > staMonEventsMax {
			st.StaMon.Events = st.StaMon.Events[len(st.StaMon.Events)-staMonEventsMax:]
		}
		eng.events = append([]StaMonEvent(nil), st.StaMon.Events...)
		for _, ifc := range []string{"wl0", "wl1"} {
			if m := st.StaMon.PrevTotal[ifc]; len(m) > 0 {
				for mac, val := range m {
					eng.prevTotal[ifc][mac] = val
				}
			}
			if m := st.StaMon.PrevKnown[ifc]; len(m) > 0 {
				for mac, val := range m {
					eng.prevKnown[ifc][mac] = val
				}
			}
		}
		eng.mu.Unlock()
	}

	log.Printf("restored volatile state from %s (rf=%d/%d radar=%d/%d stamon=%d traf=%d)",
		path, len(st.RFSamples), len(st.RFEvents), len(st.Radar.HistPoints), len(st.Radar.Events),
		len(st.StaMon.Events), len(st.TrafHist))
}
