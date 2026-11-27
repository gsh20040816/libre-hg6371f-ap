package main

import (
	"path/filepath"
	"testing"
)

func TestBCMRFConfRoundTrip(t *testing.T) {
	p := filepath.Join(t.TempDir(), "bcm-rf.conf")

	in := defaultPanelConfig()
	in.Tune = TuneConfig{Frameburst: 0, AmpduRTS: 1, PhyRxdesens: 18}
	r0 := in.Radios["wl0"]
	r0.Enabled = false
	r0.WidthMHz = 40
	r0.HEEnabled = true
	r0.TxPwrPercent = 80
	r0.AssocRSSITh = -78
	r0.APSD = true
	r0.Guest.VLANID = 20
	in.Radios["wl0"] = r0

	r1 := in.Radios["wl1"]
	r1.Enabled = true
	r1.WidthMHz = 80
	r1.HEEnabled = true
	r1.TxPwrPercent = 90
	r1.AssocRSSITh = -72
	r1.APSD = false
	r1.TWT = false
	r1.ATF = true
	r1.Guest.VLANID = 30
	in.Radios["wl1"] = r1

	if err := writeBCMRFConf(p, in); err != nil {
		t.Fatalf("writeBCMRFConf: %v", err)
	}

	out := defaultPanelConfig()
	if !readBCMRFConf(p, &out) {
		t.Fatalf("readBCMRFConf returned false")
	}
	if out.Tune != in.Tune {
		t.Fatalf("Tune mismatch: got %+v, want %+v", out.Tune, in.Tune)
	}
	got0 := out.Radios["wl0"]
	if got0.Enabled != false || got0.WidthMHz != 40 || !got0.HEEnabled ||
		got0.TxPwrPercent != 80 || got0.AssocRSSITh != -78 || !got0.APSD || got0.Guest.VLANID != 20 {
		t.Fatalf("wl0 mismatch: %+v", got0)
	}
	got1 := out.Radios["wl1"]
	if !got1.Enabled || got1.WidthMHz != 80 || !got1.HEEnabled ||
		got1.TxPwrPercent != 90 || got1.AssocRSSITh != -72 || got1.APSD || got1.TWT || !got1.ATF || got1.Guest.VLANID != 30 {
		t.Fatalf("wl1 mismatch: %+v", got1)
	}
}

func TestGuestBridgeName(t *testing.T) {
	if br, err := guestBridgeName(0); err != nil || br != "br0" {
		t.Fatalf("guestBridgeName(0) = (%q, %v)", br, err)
	}
	if br, err := guestBridgeName(20); err != nil || br != "br-vlan20" {
		t.Fatalf("guestBridgeName(20) = (%q, %v)", br, err)
	}
	if _, err := guestBridgeName(1); err == nil {
		t.Fatalf("expected error for vlan 1")
	}
	if _, err := guestBridgeName(4095); err == nil {
		t.Fatalf("expected error for vlan 4095")
	}
}
