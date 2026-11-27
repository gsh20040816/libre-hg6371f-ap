package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"strings"
	"sync"
	"testing"
	"time"
)

// miniMQTTBroker is a lightweight in-process MQTT 3.1.1 broker for unit testing.
type miniMQTTBroker struct {
	ln       net.Listener
	mu       sync.Mutex
	msgs     []mqttMsg
	subs     []string
	user     string
	pass     string
	conn     net.Conn
	gotPing  bool
	gotDisc  bool
}

func startMiniMQTTBroker(t *testing.T, user, pass string) *miniMQTTBroker {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	b := &miniMQTTBroker{ln: ln, user: user, pass: pass}
	go b.acceptLoop()
	t.Cleanup(func() { _ = ln.Close() })
	return b
}

func (b *miniMQTTBroker) acceptLoop() {
	for {
		c, err := b.ln.Accept()
		if err != nil {
			return
		}
		go b.handleConn(c)
	}
}

func (b *miniMQTTBroker) handleConn(c net.Conn) {
	defer c.Close()
	br := bufio.NewReader(c)
	for {
		hdr, err := br.ReadByte()
		if err != nil {
			return
		}
		rem, err := mqttReadRemLen(br)
		if err != nil {
			return
		}
		body := make([]byte, rem)
		if rem > 0 {
			if _, err := io.ReadFull(br, body); err != nil {
				return
			}
		}
		switch hdr & 0xf0 {
		case mqttPktConnect:
			// Verify optional auth in payload
			rc := byte(0)
			if b.user != "" {
				// Quick check that username & password strings exist in CONNECT body
				if !strings.Contains(string(body), b.user) || !strings.Contains(string(body), b.pass) {
					rc = 4
				}
			}
			_, _ = c.Write([]byte{mqttPktConnAck, 2, 0, rc})
			if rc != 0 {
				return
			}
			b.mu.Lock()
			b.conn = c
			b.mu.Unlock()
		case mqttPktPublish:
			topic, payload, qos, pktID, err := parseMQTTPublish(hdr, body)
			if err == nil {
				b.mu.Lock()
				b.msgs = append(b.msgs, mqttMsg{Topic: topic, Payload: append([]byte(nil), payload...)})
				b.mu.Unlock()
				if qos == 1 {
					var ack [4]byte
					ack[0] = mqttPktPubAck
					ack[1] = 2
					binary.BigEndian.PutUint16(ack[2:4], pktID)
					_, _ = c.Write(ack[:])
				}
			}
		case mqttPktSubscribe:
			if len(body) >= 2 {
				pktID := binary.BigEndian.Uint16(body[0:2])
				pos := 2
				var grants []byte
				for pos+2 <= len(body) {
					tLen := int(binary.BigEndian.Uint16(body[pos : pos+2]))
					pos += 2
					if pos+tLen+1 > len(body) {
						break
					}
					b.mu.Lock()
					b.subs = append(b.subs, string(body[pos:pos+tLen]))
					b.mu.Unlock()
					pos += tLen + 1
					grants = append(grants, 0)
				}
				var sb []byte
				sb = append(sb, byte(pktID>>8), byte(pktID))
				sb = append(sb, grants...)
				_, _ = c.Write(mqttFrame(mqttPktSubAck, sb))
			}
		case mqttPktPingReq:
			b.mu.Lock()
			b.gotPing = true
			b.mu.Unlock()
			_, _ = c.Write([]byte{mqttPktPingResp, 0})
		case mqttPktDisconnect:
			b.mu.Lock()
			b.gotDisc = true
			b.mu.Unlock()
			return
		}
	}
}

func (b *miniMQTTBroker) sendPublish(topic, payload string) {
	b.mu.Lock()
	c := b.conn
	b.mu.Unlock()
	if c == nil {
		return
	}
	mc := &mqttConn{netConn: c}
	_ = mc.publish(topic, []byte(payload), false)
}

func TestParseMQTTBroker(t *testing.T) {
	cases := []struct {
		in     string
		addr   string
		useTLS bool
		ok     bool
	}{
		{"192.168.1.1", "192.168.1.1:1883", false, true},
		{"tcp://192.168.1.1:1883", "192.168.1.1:1883", false, true},
		{"mqtt://broker.local", "broker.local:1883", false, true},
		{"tls://broker.local", "broker.local:8883", true, true},
		{"mqtts://broker.local:8884", "broker.local:8884", true, true},
		{"http://bad.local", "", false, false},
		{"", "", false, false},
	}
	for _, tc := range cases {
		addr, useTLS, err := parseMQTTBroker(tc.in)
		if (err == nil) != tc.ok || addr != tc.addr || useTLS != tc.useTLS {
			t.Fatalf("parseMQTTBroker(%q) = (%q, %v, %v), want (%q, %v, ok=%v)",
				tc.in, addr, useTLS, err, tc.addr, tc.useTLS, tc.ok)
		}
	}
}

func TestMQTTClientProtocolAndDiscovery(t *testing.T) {
	broker := startMiniMQTTBroker(t, "apuser", "secret123")
	addr := broker.ln.Addr().String()

	// Wrong password must be rejected with CONNACK rc=4
	badCfg := MQTTConfig{Broker: addr, Username: "apuser", Password: "wrong", ClientID: "test-ap"}
	if _, err := dialMQTT(badCfg, "", "", 3*time.Second); err == nil {
		t.Fatalf("expected auth failure with wrong password")
	}

	// Valid connection + subscribe + discovery + radar/stamon/control state publish
	cfg := normalizeMQTTConfig(MQTTConfig{
		Enabled:         true,
		Broker:          "tcp://" + addr,
		Username:        "apuser",
		Password:        "secret123",
		ClientID:        "hg6371f-test",
		TopicPrefix:     "hg6371f-ap",
		Discovery:       true,
		DiscoveryPrefix: "homeassistant",
		AllowControl:    true,
	})
	conn, err := dialMQTT(cfg, cfg.TopicPrefix+"/availability", "offline", 3*time.Second)
	if err != nil {
		t.Fatalf("dialMQTT failed: %v", err)
	}
	defer conn.disconnect()

	s := &Server{
		cfg:    defaultPanelConfig(),
		radar:  newRadarEngine(),
		stamon: newStaMonEngine(),
		mqtt:   newMQTTEngine(),
	}
	s.cfg.MQTT = cfg
	s.cfg.Radar.Enabled = true
	s.cfg.Radar.TargetMAC = "AA:BB:CC:DD:EE:01"
	s.cfg.StaMon.Enabled = true
	s.cfg.StaMon.Targets = []StaMonTarget{{MAC: "AA:BB:CC:DD:EE:02", Label: "Phone"}}
	s.stamon.states["AA:BB:CC:DD:EE:02"] = &StaMonState{
		MAC: "AA:BB:CC:DD:EE:02", Label: "Phone", Present: true, BestRSSI: -58, BestBand: "5G",
	}
	s.radar.occupied = true
	s.radar.score = 18.5

	if err := conn.subscribe(cfg.DiscoveryPrefix+"/status", cfg.TopicPrefix+"/cmd/#"); err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	if err := s.publishMQTTDiscovery(conn, cfg); err != nil {
		t.Fatalf("publishMQTTDiscovery: %v", err)
	}
	if err := s.publishMQTTRadarState(conn, cfg); err != nil {
		t.Fatalf("publishMQTTRadarState: %v", err)
	}
	if err := s.publishMQTTStaMonStates(conn, cfg); err != nil {
		t.Fatalf("publishMQTTStaMonStates: %v", err)
	}
	if err := s.publishMQTTControlState(conn, cfg); err != nil {
		t.Fatalf("publishMQTTControlState: %v", err)
	}
	if err := conn.ping(); err != nil {
		t.Fatalf("ping: %v", err)
	}
	time.Sleep(100 * time.Millisecond)

	broker.mu.Lock()
	msgs := append([]mqttMsg(nil), broker.msgs...)
	gotPing := broker.gotPing
	subs := append([]string(nil), broker.subs...)
	broker.mu.Unlock()

	if !gotPing {
		t.Fatalf("broker did not receive PINGREQ")
	}
	if len(subs) != 2 {
		t.Fatalf("expected 2 subscriptions, got %v", subs)
	}
	foundRadarDisc, foundStaDisc, foundRadarState, foundStaState := false, false, false, false
	for _, m := range msgs {
		if strings.HasSuffix(m.Topic, "/csi_occupied/config") {
			foundRadarDisc = true
		}
		if strings.HasSuffix(m.Topic, "/stamon_aabbccddee02/config") {
			foundStaDisc = true
		}
		if m.Topic == "hg6371f-ap/radar/state" {
			var p map[string]any
			if json.Unmarshal(m.Payload, &p) == nil && p["occupied"] == true {
				foundRadarState = true
			}
		}
		if m.Topic == "hg6371f-ap/stamon/aabbccddee02/state" {
			var p StaMonState
			if json.Unmarshal(m.Payload, &p) == nil && p.Present && p.BestRSSI == -58 {
				foundStaState = true
			}
		}
	}
	if !foundRadarDisc || !foundStaDisc || !foundRadarState || !foundStaState {
		t.Fatalf("missing expected MQTT topics: radarDisc=%v staDisc=%v radarState=%v staState=%v (total=%d)",
			foundRadarDisc, foundStaDisc, foundRadarState, foundStaState, len(msgs))
	}
}
