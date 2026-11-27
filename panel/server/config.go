package main

import (
	"encoding/json"
	"os"
	"path/filepath"
)

type GuestBSSConfig struct {
	Enabled    bool   `json:"enabled"`
	SSID       string `json:"ssid"`
	Passphrase string `json:"passphrase,omitempty"`
	Hidden     bool   `json:"hidden"`
	Isolate    bool   `json:"isolate"`
	VLANID     int    `json:"vlan_id"` // 0 = bridge on br0; 2..4094 = 802.1Q VLAN on lan0.<vid>
}

type RoamingConfig struct {
	Dot11k         bool   `json:"dot11k"`          // rrm_neighbor_report
	Dot11v         bool   `json:"dot11v"`          // bss_transition
	Dot11r         bool   `json:"dot11r"`          // ieee80211r FT-PSK
	MobilityDomain string `json:"mobility_domain"` // 4 hex chars, e.g. "a1b2"
}

type RadioConfig struct {
	Enabled      bool           `json:"enabled"`
	TxPwrPercent int            `json:"txpwr_percent"` // 10..100
	Isolate      bool           `json:"isolate"`
	AssocRSSITh  int            `json:"assoc_rssi_th"` // 0 (off) or -95..-40
	MACMode      int            `json:"mac_mode"`      // 0=off, 1=deny, 2=allow
	MACList      []string       `json:"mac_list"`
	WidthMHz     int            `json:"width_mhz"` // 20/40 for wl0; 20/40/80/160 for wl1
	HEEnabled    bool           `json:"he_enabled"`
	TWT          bool           `json:"twt"`
	ATF          bool           `json:"atf"`
	APSD         bool           `json:"apsd"`
	Roaming      RoamingConfig  `json:"roaming"`
	Guest        GuestBSSConfig `json:"guest"`
}

type LEDConfig struct {
	Mode       string `json:"mode"`        // "auto" | "off" | "schedule"
	NightStart string `json:"night_start"` // "HH:MM", e.g. "23:00"
	NightEnd   string `json:"night_end"`   // "HH:MM", e.g. "07:00"
}

type ButtonConfig struct {
	ShortAction string `json:"short_action"` // "toggle_led" | "none"
	LongAction  string `json:"long_action"`  // "toggle_guest" | "restart_wifi" | "none"
}

type WifiScheduleConfig struct {
	Enabled  bool   `json:"enabled"`
	OffStart string `json:"off_start"` // "HH:MM"
	OffEnd   string `json:"off_end"`   // "HH:MM"
	Band     string `json:"band"`      // "both" | "wl0" | "wl1"
}

type TuneConfig struct {
	Frameburst  int `json:"frameburst"`   // 0 or 1 (default 1)
	AmpduRTS    int `json:"ampdu_rts"`    // 0 or 1 (default 0)
	PhyRxdesens int `json:"phy_rxdesens"` // 0..30 (default 15)
}

type RadarBaseline struct {
	Med      [6]float64 `json:"med"`                // empty-room medians: [a0_top4, a1_top4, ph_top4, coh_a0, coh_a1, coh_ph]
	IQR      [6]float64 `json:"iqr"`                // empty-room floored IQRs for SNR normalization
	Chanspec string     `json:"chanspec,omitempty"` // chanspec at calibration time (e.g. "0x100d")
}

type RadarConfig struct {
	Enabled      bool           `json:"enabled"`
	Iface        string         `json:"iface"`         // "wl0" (Netlink 28) or "wl1" (Netlink 23)
	TargetMAC    string         `json:"target_mac"`    // stationary target STA MAC
	IntervalMs   int            `json:"interval_ms"`   // default 200 (5 Hz)
	MicroThresh  float64        `json:"micro_thresh"`  // calibrated empty-room micro hold threshold (default 3.5)
	MotionThresh float64        `json:"motion_thresh"` // calibrated empty-room motion entry threshold (default 12.0)
	ConfirmSec   int            `json:"confirm_sec"`   // leaky-bucket confirmation seconds (default 2)
	HoldSec      int            `json:"hold_sec"`      // occupancy hold seconds (default 30)
	WebhookURL   string         `json:"webhook_url"`   // optional HTTP POST webhook on state transition
	Baseline     *RadarBaseline `json:"baseline,omitempty"`
}

type StaMonTarget struct {
	MAC   string `json:"mac"`
	Label string `json:"label"`
}

type StaMonConfig struct {
	Enabled    bool           `json:"enabled"`
	Band       string         `json:"band"`        // "both" | "wl0" | "wl1"
	RSSIThresh int            `json:"rssi_thresh"` // dBm threshold for present (-95..-10, default -75)
	AwaySec    int            `json:"away_sec"`    // seconds without packet before away (default 180)
	WebhookURL string         `json:"webhook_url"`
	Targets    []StaMonTarget `json:"targets"` // max 4 (hardware AMT limit per radio)
}

type MQTTConfig struct {
	Enabled         bool   `json:"enabled"`
	Broker          string `json:"broker"`           // tcp://host:1883, tls://host:8883, or host:port
	Username        string `json:"username"`
	Password        string `json:"password"`
	ClientID        string `json:"client_id"`        // default "hg6371f-ap"
	TopicPrefix     string `json:"topic_prefix"`     // default "hg6371f-ap"
	Discovery       bool   `json:"discovery"`        // Home Assistant MQTT auto-discovery
	DiscoveryPrefix string `json:"discovery_prefix"` // default "homeassistant"
	IntervalSec     int    `json:"interval_sec"`     // telemetry period (10..300s, default 30)
	AllowControl    bool   `json:"allow_control"`    // expose guest Wi-Fi / LED / radar control entities
	TLSInsecure     bool   `json:"tls_insecure"`     // skip TLS verification for self-signed certs
}

type PanelConfig struct {
	LED          LEDConfig              `json:"led"`
	Button       ButtonConfig           `json:"button"`
	WifiSchedule WifiScheduleConfig     `json:"wifi_schedule"`
	Tune         TuneConfig             `json:"tune"`
	Radar        RadarConfig            `json:"radar"`
	StaMon       StaMonConfig           `json:"stamon"`
	MQTT         MQTTConfig             `json:"mqtt"`
	TLSRedirect  bool                   `json:"tls_redirect"`
	Radios       map[string]RadioConfig `json:"radios"`
}

func defaultPanelConfig() PanelConfig {
	return PanelConfig{
		LED: LEDConfig{
			Mode:       "auto",
			NightStart: "23:00",
			NightEnd:   "07:00",
		},
		Button: ButtonConfig{
			ShortAction: "toggle_led",
			LongAction:  "toggle_guest",
		},
		WifiSchedule: WifiScheduleConfig{
			Enabled:  false,
			OffStart: "01:00",
			OffEnd:   "06:30",
			Band:     "wl0",
		},
		Tune: TuneConfig{
			Frameburst:  1,
			AmpduRTS:    0,
			PhyRxdesens: 15,
		},
		Radar: RadarConfig{
			Enabled:      false,
			Iface:        "wl0",
			IntervalMs:   200,
			MicroThresh:  3.5,
			MotionThresh: 12.0,
			ConfirmSec:   2,
			HoldSec:      30,
		},
		StaMon: StaMonConfig{
			Enabled:    false,
			Band:       "both",
			RSSIThresh: -75,
			AwaySec:    180,
			Targets:    []StaMonTarget{},
		},
		MQTT: MQTTConfig{
			Enabled:         false,
			ClientID:        "hg6371f-ap",
			TopicPrefix:     "hg6371f-ap",
			Discovery:       true,
			DiscoveryPrefix: "homeassistant",
			IntervalSec:     30,
			AllowControl:    true,
		},
		TLSRedirect: false,
		Radios: map[string]RadioConfig{
			"wl1": {
				Enabled:      true,
				TxPwrPercent: 100,
				WidthMHz:     160,
				HEEnabled:    true,
				TWT:          true,
				APSD:         true,
				Roaming:      RoamingConfig{MobilityDomain: "6371"},
				Guest:        GuestBSSConfig{SSID: "OpenWrt-Guest", Isolate: true},
			},
			"wl0": {
				Enabled:      true,
				TxPwrPercent: 100,
				WidthMHz:     20,
				HEEnabled:    false,
				APSD:         false,
				Roaming:      RoamingConfig{MobilityDomain: "6371"},
				Guest:        GuestBSSConfig{SSID: "OpenWrt-IoT", Isolate: true},
			},
		},
	}
}

func (s *Server) loadConfig() {
	cfg := defaultPanelConfig()
	if b, err := os.ReadFile(s.configFile); err == nil {
		var loaded PanelConfig
		if json.Unmarshal(b, &loaded) == nil {
			if loaded.LED.Mode != "" {
				cfg.LED = loaded.LED
			}
			if loaded.Button.ShortAction != "" {
				cfg.Button = loaded.Button
			}
			cfg.WifiSchedule = loaded.WifiSchedule
			if cfg.WifiSchedule.OffStart == "" {
				cfg.WifiSchedule.OffStart = "01:00"
			}
			if cfg.WifiSchedule.OffEnd == "" {
				cfg.WifiSchedule.OffEnd = "06:30"
			}
			if cfg.WifiSchedule.Band == "" {
				cfg.WifiSchedule.Band = "wl0"
			}
			// Preserve loaded Tune settings whenever config.json explicitly contains a "tune" block (including all-zero values)
			var tuneProbe struct {
				Tune *TuneConfig `json:"tune"`
			}
			if json.Unmarshal(b, &tuneProbe) == nil && tuneProbe.Tune != nil {
				t := *tuneProbe.Tune
				if (t.Frameburst == 0 || t.Frameburst == 1) &&
					(t.AmpduRTS == 0 || t.AmpduRTS == 1) &&
					t.PhyRxdesens >= 0 && t.PhyRxdesens <= 30 {
					cfg.Tune = t
				}
			}
			cfg.TLSRedirect = loaded.TLSRedirect
			if loaded.Radar.Iface != "" || loaded.Radar.TargetMAC != "" || loaded.Radar.MicroThresh > 0 {
				rc := loaded.Radar
				if rc.Iface != "wl0" && rc.Iface != "wl1" {
					rc.Iface = "wl0"
				}
				if rc.IntervalMs < 100 || rc.IntervalMs > 2000 {
					rc.IntervalMs = 200
				}
				if rc.MicroThresh <= 0 {
					rc.MicroThresh = 3.5
				}
				if rc.MotionThresh <= rc.MicroThresh {
					rc.MotionThresh = rc.MicroThresh * 2.0
				}
				if rc.ConfirmSec < 1 || rc.ConfirmSec > 15 {
					rc.ConfirmSec = 2
				}
				if rc.HoldSec < 5 || rc.HoldSec > 600 {
					rc.HoldSec = 30
				}
				cfg.Radar = rc
			}
			if loaded.StaMon.Band != "" || len(loaded.StaMon.Targets) > 0 || loaded.StaMon.RSSIThresh < 0 || loaded.StaMon.Enabled {
				sm := loaded.StaMon
				if sm.Band != "both" && sm.Band != "wl0" && sm.Band != "wl1" {
					sm.Band = "both"
				}
				if sm.RSSIThresh >= 0 || sm.RSSIThresh < -95 {
					sm.RSSIThresh = -75
				}
				if sm.AwaySec < 30 || sm.AwaySec > 3600 {
					sm.AwaySec = 180
				}
				if len(sm.Targets) > staMonMaxTargets {
					sm.Targets = sm.Targets[:staMonMaxTargets]
				}
				cfg.StaMon = sm
			}
			var mqttProbe struct {
				MQTT *MQTTConfig `json:"mqtt"`
			}
			if json.Unmarshal(b, &mqttProbe) == nil && mqttProbe.MQTT != nil {
				mc := normalizeMQTTConfig(*mqttProbe.MQTT)
				cfg.MQTT = mc
			}
			if loaded.Radios != nil {
				for _, iface := range []string{"wl0", "wl1"} {
					if rc, ok := loaded.Radios[iface]; ok {
						if rc.TxPwrPercent < 10 || rc.TxPwrPercent > 100 {
							rc.TxPwrPercent = 100
						}
						if rc.Roaming.MobilityDomain == "" {
							rc.Roaming.MobilityDomain = "6371"
						}
						cfg.Radios[iface] = rc
					}
				}
			}
		}
	}
	readBCMRFConf(bcmRFConfPath, &cfg)
	s.cfgMu.Lock()
	s.cfg = cfg
	s.cfgMu.Unlock()
}

func (s *Server) getConfig() PanelConfig {
	s.cfgMu.RLock()
	cp := s.cfg
	if s.cfg.Radar.Baseline != nil {
		b := *s.cfg.Radar.Baseline
		cp.Radar.Baseline = &b
	}
	if len(s.cfg.StaMon.Targets) > 0 {
		tg := make([]StaMonTarget, len(s.cfg.StaMon.Targets))
		copy(tg, s.cfg.StaMon.Targets)
		cp.StaMon.Targets = tg
	}
	cp.Radios = make(map[string]RadioConfig, len(s.cfg.Radios))
	for k, v := range s.cfg.Radios {
		if len(v.MACList) > 0 {
			ml := make([]string, len(v.MACList))
			copy(ml, v.MACList)
			v.MACList = ml
		}
		cp.Radios[k] = v
	}
	s.cfgMu.RUnlock()
	readBCMRFConf(bcmRFConfPath, &cp)
	return cp
}

func (s *Server) saveConfigLocked() error {
	if err := os.MkdirAll(filepath.Dir(s.configFile), 0755); err != nil {
		return err
	}
	b, err := json.MarshalIndent(s.cfg, "", "  ")
	if err != nil {
		return err
	}
	tmp := s.configFile + ".tmp"
	if err := os.WriteFile(tmp, b, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, s.configFile)
}

func (s *Server) updateConfig(fn func(*PanelConfig)) error {
	s.cfgMu.Lock()
	fn(&s.cfg)
	err := s.saveConfigLocked()
	s.cfgMu.Unlock()
	return err
}
