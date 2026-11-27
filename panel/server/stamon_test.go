package main

import "testing"

func TestParseStaMonDumpWithCounters(t *testing.T) {
	sample := `Status : Enabled
Monitoring 2 stations 
AMT_Idx  MAC_Address            RSSI   Chanspec           Off-chan_Time 
-1       ce:68:6d:ce:f4:ca    -81dBm   0xeb32 48/160              0ms
Counters : data 141766, mngt 1277, cntl 347
0        ec:2c:0d:34:6d:0a    -42dBm   0xeb32 48/160              0ms
Counters : data 10, mngt 2, cntl 3
`
	m := parseStaMonDump(sample)
	if len(m) != 2 {
		t.Fatalf("expected 2 entries, got %d", len(m))
	}
	a := m["CE:68:6D:CE:F4:CA"]
	if a.RSSI != -81 || a.Total != 141766+1277+347 {
		t.Fatalf("unexpected entry A: %+v", a)
	}
	b := m["EC:2C:0D:34:6D:0A"]
	if b.RSSI != -42 || b.Total != 15 {
		t.Fatalf("unexpected entry B: %+v", b)
	}
}

func TestParseStaMonDumpAggregateCounters(t *testing.T) {
	sample := `Status : Enabled
Monitoring 2 stations 
AMT_Idx  MAC_Address            RSSI   Chanspec           Off-chan_Time 
-1       ce:68:6d:ce:f4:ca    -49dBm   0xeb32 48/160              0ms
0        ec:2c:0d:34:6d:0a    -62dBm   0xeb32 48/160              0ms
Counters : data 311, mngt 2, cntl 0
`
	m := parseStaMonDump(sample)
	if len(m) != 2 {
		t.Fatalf("expected 2 entries, got %d", len(m))
	}
	if m["CE:68:6D:CE:F4:CA"].Total != 313 || m["EC:2C:0D:34:6D:0A"].Total != 313 {
		t.Fatalf("expected aggregate counters applied to all rows, got %+v", m)
	}
}

func TestStaMonDumpNeedsResync(t *testing.T) {
	want := map[string]bool{"CE:68:6D:CE:F4:CA": true}
	good := `Status : Enabled
Monitoring 1 stations 
AMT_Idx  MAC_Address            RSSI   Chanspec           Off-chan_Time 
-1       ce:68:6d:ce:f4:ca    -49dBm   0xeb32 48/160              0ms
Counters : data 311, mngt 2, cntl 0
`
	if staMonDumpNeedsResync(good, parseStaMonDump(good), want) {
		t.Fatalf("expected healthy dump not to trigger resync")
	}
	if !staMonDumpNeedsResync("", parseStaMonDump(""), want) {
		t.Fatalf("expected empty/error dump to trigger resync")
	}
	disabled := "Status : Disabled\n"
	if !staMonDumpNeedsResync(disabled, parseStaMonDump(disabled), want) {
		t.Fatalf("expected disabled dump to trigger resync")
	}
	wrongMAC := `Status : Enabled
Monitoring 1 stations 
-1       11:22:33:44:55:66    -50dBm   0xeb32 48/160              0ms
Counters : data 1, mngt 0, cntl 0
`
	if !staMonDumpNeedsResync(wrongMAC, parseStaMonDump(wrongMAC), want) {
		t.Fatalf("expected mismatched MAC to trigger resync")
	}
}
