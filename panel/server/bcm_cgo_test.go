package main

import (
	"strconv"
	"strings"
	"testing"
)

func TestFormatFCStatusMatchesOverviewParser(t *testing.T) {
	out := formatFCStatus(true, true, 16384, 54321, 54000)
	if !strings.Contains(out, "HW Acceleration <Enabled>") {
		t.Fatalf("expected HW Acceleration <Enabled>, got %q", out)
	}
	m := reFlowActive.FindStringSubmatch(out)
	if len(m) != 4 {
		t.Fatalf("reFlowActive failed to match %q", out)
	}
	active, _ := strconv.Atoi(m[1])
	cum, _ := strconv.Atoi(m[2])
	if active != 321 || cum != 54321 {
		t.Fatalf("parsed active=%d cum=%d, want 321 and 54321", active, cum)
	}

	// Wraparound: modular uint32 subtraction matches fcache.c (10 - 0xfffffff0 = 26).
	outWrap := formatFCStatus(false, false, 16384, 10, 0xfffffff0)
	if strings.Contains(outWrap, "HW Acceleration <Enabled>") {
		t.Fatalf("unexpected enabled HW accel in %q", outWrap)
	}
	m2 := reFlowActive.FindStringSubmatch(outWrap)
	if len(m2) != 4 || m2[1] != "26" {
		t.Fatalf("expected modular active=26 on uint32 wraparound, got %v from %q", m2, outWrap)
	}
}

func TestFormatBootstateMatchesParser(t *testing.T) {
	out := formatBootstateText(2, 2, '6', 2, [2]int{1, 2}, [2]int{7, 8}, [2]string{"$imageversion: A $", "$imageversion: B $"})
	st := parseBootstate(out)
	if st.Booted != 2 || st.Reboot != 2 || st.Committed != 2 || st.FirstCommit != 0 || st.SecondCommit != 1 {
		t.Fatalf("unexpected parsed bootstate: %+v\nraw:\n%s", st, out)
	}

	outOnce := formatBootstateText(2, 1, '7', 2, [2]int{1, 2}, [2]int{7, 8}, [2]string{"", ""})
	stOnce := parseBootstate(outOnce)
	if stOnce.Booted != 2 || stOnce.Reboot != 1 || stOnce.Committed != 2 || stOnce.SecondCommit != 1 {
		t.Fatalf("unexpected parsed one-shot bootstate: %+v", stOnce)
	}

	// Fallback: even without rd_metadata line, parseBootstate derives Committed from commit flag.
	noMeta := strings.Replace(out, "rd_metadata: committed 2 valid 1,2 seq 7,8\n", "", 1)
	stFallback := parseBootstate(noMeta)
	if stFallback.Committed != 2 {
		t.Fatalf("expected Committed fallback=2, got %+v", stFallback)
	}
}

func TestWlFormatAndParsers(t *testing.T) {
	if s, ok := formatWlSpect(0); !ok || s != "Off\n" {
		t.Fatalf("formatWlSpect(0) = (%q, %v)", s, ok)
	}
	if _, ok := formatWlSpect(9); ok {
		t.Fatalf("expected invalid spect 9 to fail")
	}
	if got := formatWlVarInt(1); got != "1\n" {
		t.Fatalf("formatWlVarInt(1) = %q", got)
	}
	if got := formatWlVarInt(15); got != "15 (0xf)\n" {
		t.Fatalf("formatWlVarInt(15) = %q", got)
	}
	if got := formatWlVarInt(-75); got != "-75\n" {
		t.Fatalf("formatWlVarInt(-75) = %q", got)
	}
	if b, ok := parseWlBand("2g"); !ok || b != 2 {
		t.Fatalf("parseWlBand(2g) = (%d, %v)", b, ok)
	}
	if b, ok := parseWlBand("5G"); !ok || b != 1 {
		t.Fatalf("parseWlBand(5G) = (%d, %v)", b, ok)
	}
	if v, ok := parseWlIntArg("-75"); !ok || v != -75 {
		t.Fatalf("parseWlIntArg(-75) = (%d, %v)", v, ok)
	}
	if v, ok := parseWlUintArg("0xf"); !ok || v != 15 {
		t.Fatalf("parseWlUintArg(0xf) = (%d, %v)", v, ok)
	}
	if c, ok := parseBootstateCode("8"); !ok || c != 8 {
		t.Fatalf("parseBootstateCode(8) = (%d, %v)", c, ok)
	}
	if _, ok := parseBootstateCode("+1"); ok {
		t.Fatalf("expected +1 to fall back to CLI")
	}
}
