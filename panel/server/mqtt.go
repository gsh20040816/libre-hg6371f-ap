package main

import (
	"bufio"
	"bytes"
	"crypto/tls"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

// ── Minimal Pure-Go MQTT 3.1.1 Client (zero external dependencies) ──────────

const (
	mqttPktConnect     byte = 1 << 4
	mqttPktConnAck     byte = 2 << 4
	mqttPktPublish     byte = 3 << 4
	mqttPktPubAck      byte = 4 << 4
	mqttPktSubscribe   byte = 8 << 4
	mqttPktSubAck      byte = 9 << 4
	mqttPktPingReq     byte = 12 << 4
	mqttPktPingResp    byte = 13 << 4
	mqttPktDisconnect  byte = 14 << 4
	mqttKeepAliveSec        = 60
	mqttIOBufLimit          = 256 * 1024
)

type mqttConn struct {
	netConn net.Conn
	br      *bufio.Reader
	wmu     sync.Mutex
	nextID  uint16
}

type mqttMsg struct {
	Topic   string
	Payload []byte
}

func parseMQTTBroker(raw string) (addr string, useTLS bool, err error) {
	s := strings.TrimSpace(raw)
	if s == "" {
		return "", false, errors.New("Broker 地址不能为空")
	}
	lower := strings.ToLower(s)
	switch {
	case strings.HasPrefix(lower, "tls://"):
		useTLS = true
		s = s[6:]
	case strings.HasPrefix(lower, "ssl://"):
		useTLS = true
		s = s[6:]
	case strings.HasPrefix(lower, "mqtts://"):
		useTLS = true
		s = s[8:]
	case strings.HasPrefix(lower, "tcp://"):
		s = s[6:]
	case strings.HasPrefix(lower, "mqtt://"):
		s = s[7:]
	case strings.Contains(s, "://"):
		return "", false, errors.New("仅支持 tcp:// / mqtt:// / tls:// / mqtts:// 协议")
	}
	s = strings.TrimRight(s, "/")
	if s == "" {
		return "", false, errors.New("Broker 主机地址无效")
	}
	if _, _, splitErr := net.SplitHostPort(s); splitErr != nil {
		if strings.HasPrefix(s, "[") && strings.HasSuffix(s, "]") {
			s = strings.Trim(s, "[]")
		}
		port := "1883"
		if useTLS {
			port = "8883"
		}
		s = net.JoinHostPort(s, port)
	}
	return s, useTLS, nil
}

func mqttWriteUTF8(buf *bytes.Buffer, s string) {
	var ln [2]byte
	binary.BigEndian.PutUint16(ln[:], uint16(len(s)))
	buf.Write(ln[:])
	buf.WriteString(s)
}

func mqttEncodeRemLen(buf *bytes.Buffer, n int) {
	for {
		b := byte(n % 128)
		n /= 128
		if n > 0 {
			b |= 0x80
		}
		buf.WriteByte(b)
		if n == 0 {
			break
		}
	}
}

func mqttReadRemLen(br *bufio.Reader) (int, error) {
	mult := 1
	val := 0
	for i := 0; i < 4; i++ {
		b, err := br.ReadByte()
		if err != nil {
			return 0, err
		}
		val += int(b&0x7f) * mult
		if (b & 0x80) == 0 {
			return val, nil
		}
		mult *= 128
	}
	return 0, errors.New("mqtt remaining length overflow")
}

func mqttFrame(header byte, body []byte) []byte {
	var out bytes.Buffer
	out.Grow(1 + 4 + len(body))
	out.WriteByte(header)
	mqttEncodeRemLen(&out, len(body))
	out.Write(body)
	return out.Bytes()
}

func dialMQTT(cfg MQTTConfig, willTopic, willPayload string, timeout time.Duration) (*mqttConn, error) {
	addr, useTLS, err := parseMQTTBroker(cfg.Broker)
	if err != nil {
		return nil, err
	}
	dialer := &net.Dialer{Timeout: timeout}
	var nc net.Conn
	if useTLS {
		host, _, _ := net.SplitHostPort(addr)
		nc, err = tls.DialWithDialer(dialer, "tcp", addr, &tls.Config{
			MinVersion:         tls.VersionTLS12,
			ServerName:         host,
			InsecureSkipVerify: cfg.TLSInsecure,
		})
	} else {
		nc, err = dialer.Dial("tcp", addr)
	}
	if err != nil {
		return nil, err
	}

	mc := &mqttConn{
		netConn: nc,
		br:      bufio.NewReaderSize(nc, 8192),
	}
	if err := mc.connect(cfg, willTopic, willPayload, timeout); err != nil {
		_ = nc.Close()
		return nil, err
	}
	return mc, nil
}

func (c *mqttConn) writePacket(pkt []byte) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	_ = c.netConn.SetWriteDeadline(time.Now().Add(8 * time.Second))
	_, err := c.netConn.Write(pkt)
	return err
}

func (c *mqttConn) readPacket(deadline time.Time) (byte, []byte, error) {
	_ = c.netConn.SetReadDeadline(deadline)
	hdr, err := c.br.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	rem, err := mqttReadRemLen(c.br)
	if err != nil {
		return 0, nil, err
	}
	if rem < 0 || rem > mqttIOBufLimit {
		return 0, nil, fmt.Errorf("mqtt packet too large: %d", rem)
	}
	body := make([]byte, rem)
	if rem > 0 {
		if _, err := io.ReadFull(c.br, body); err != nil {
			return 0, nil, err
		}
	}
	return hdr, body, nil
}

func (c *mqttConn) connect(cfg MQTTConfig, willTopic, willPayload string, timeout time.Duration) error {
	var body bytes.Buffer
	mqttWriteUTF8(&body, "MQTT")
	body.WriteByte(4) // MQTT 3.1.1

	var flags byte = 0x02 // CleanSession
	if willTopic != "" {
		flags |= 0x04 | 0x20 // WillFlag + WillRetain (QoS 0)
	}
	if cfg.Username != "" {
		flags |= 0x80
		if cfg.Password != "" {
			flags |= 0x40
		}
	}
	body.WriteByte(flags)
	var ka [2]byte
	binary.BigEndian.PutUint16(ka[:], mqttKeepAliveSec)
	body.Write(ka[:])

	clientID := strings.TrimSpace(cfg.ClientID)
	if clientID == "" {
		clientID = "hg6371f-ap"
	}
	mqttWriteUTF8(&body, clientID)
	if willTopic != "" {
		mqttWriteUTF8(&body, willTopic)
		mqttWriteUTF8(&body, willPayload)
	}
	if cfg.Username != "" {
		mqttWriteUTF8(&body, cfg.Username)
		if cfg.Password != "" {
			mqttWriteUTF8(&body, cfg.Password)
		}
	}

	if err := c.writePacket(mqttFrame(mqttPktConnect, body.Bytes())); err != nil {
		return err
	}
	hdr, ack, err := c.readPacket(time.Now().Add(timeout))
	if err != nil {
		return fmt.Errorf("等待 CONNACK 失败: %w", err)
	}
	if (hdr&0xf0) != mqttPktConnAck || len(ack) < 2 {
		return fmt.Errorf("收到非法 CONNACK 报文 (0x%02x)", hdr)
	}
	switch ack[1] {
	case 0:
		return nil
	case 1:
		return errors.New("Broker 拒绝连接：不支持 MQTT 3.1.1 协议")
	case 2:
		return errors.New("Broker 拒绝连接：Client ID 无效")
	case 3:
		return errors.New("Broker 服务暂不可用")
	case 4:
		return errors.New("认证失败：用户名或密码错误")
	case 5:
		return errors.New("未授权：Broker 拒绝该客户端访问")
	default:
		return fmt.Errorf("Broker 拒绝连接 (code=%d)", ack[1])
	}
}

func (c *mqttConn) publish(topic string, payload []byte, retain bool) error {
	var body bytes.Buffer
	body.Grow(2 + len(topic) + len(payload))
	mqttWriteUTF8(&body, topic)
	body.Write(payload)
	hdr := mqttPktPublish
	if retain {
		hdr |= 0x01
	}
	return c.writePacket(mqttFrame(hdr, body.Bytes()))
}

func (c *mqttConn) subscribe(topics ...string) error {
	if len(topics) == 0 {
		return nil
	}
	c.wmu.Lock()
	c.nextID++
	if c.nextID == 0 {
		c.nextID = 1
	}
	pktID := c.nextID
	c.wmu.Unlock()

	var body bytes.Buffer
	var idBuf [2]byte
	binary.BigEndian.PutUint16(idBuf[:], pktID)
	body.Write(idBuf[:])
	for _, t := range topics {
		mqttWriteUTF8(&body, t)
		body.WriteByte(0) // QoS 0
	}
	return c.writePacket(mqttFrame(mqttPktSubscribe|0x02, body.Bytes()))
}

func (c *mqttConn) ping() error {
	return c.writePacket([]byte{mqttPktPingReq, 0})
}

func (c *mqttConn) pubAck(pktID uint16) error {
	var pkt [4]byte
	pkt[0] = mqttPktPubAck
	pkt[1] = 2
	binary.BigEndian.PutUint16(pkt[2:4], pktID)
	return c.writePacket(pkt[:])
}

func (c *mqttConn) disconnect() {
	_ = c.writePacket([]byte{mqttPktDisconnect, 0})
	_ = c.netConn.Close()
}

func (c *mqttConn) close() {
	_ = c.netConn.Close()
}

func parseMQTTPublish(hdr byte, body []byte) (topic string, payload []byte, qos byte, pktID uint16, err error) {
	if len(body) < 2 {
		return "", nil, 0, 0, errors.New("truncated publish")
	}
	tLen := int(binary.BigEndian.Uint16(body[0:2]))
	pos := 2 + tLen
	if pos > len(body) {
		return "", nil, 0, 0, errors.New("truncated publish topic")
	}
	topic = string(body[2:pos])
	qos = (hdr >> 1) & 0x03
	if qos > 0 {
		if pos+2 > len(body) {
			return "", nil, 0, 0, errors.New("truncated publish pktID")
		}
		pktID = binary.BigEndian.Uint16(body[pos : pos+2])
		pos += 2
	}
	payload = body[pos:]
	return topic, payload, qos, pktID, nil
}

// ── MQTT / Home Assistant Integration Engine ────────────────────────────────

type mqttEngine struct {
	mu             sync.Mutex
	connected      bool
	lastError      string
	connectedAt    time.Time
	lastPubAt      time.Time
	pubCount       uint64
	rxCount        uint64
	discoveryCount int
	activeConn     *mqttConn
	stopped        bool
	knownStaSlug   map[string]bool // previously published stamon target slugs (for clean HA removal)

	reloadCh chan struct{}
	pushCh   chan struct{} // immediate full sync (discovery + telemetry + states)
	radarCh  chan struct{} // immediate radar state push
	stamonCh chan struct{} // immediate stamon state push
	stopCh   chan struct{}
	doneCh   chan struct{}
}

func newMQTTEngine() *mqttEngine {
	return &mqttEngine{
		knownStaSlug: make(map[string]bool),
		reloadCh:     make(chan struct{}, 1),
		pushCh:       make(chan struct{}, 1),
		radarCh:      make(chan struct{}, 1),
		stamonCh:     make(chan struct{}, 1),
		stopCh:       make(chan struct{}),
		doneCh:       make(chan struct{}),
	}
}

func normalizeMQTTConfig(c MQTTConfig) MQTTConfig {
	c.Broker = strings.TrimSpace(c.Broker)
	c.Username = strings.TrimSpace(c.Username)
	c.ClientID = strings.TrimSpace(c.ClientID)
	if c.ClientID == "" {
		c.ClientID = "hg6371f-ap"
	}
	c.TopicPrefix = strings.Trim(strings.TrimSpace(c.TopicPrefix), "/")
	if c.TopicPrefix == "" {
		c.TopicPrefix = "hg6371f-ap"
	}
	c.DiscoveryPrefix = strings.Trim(strings.TrimSpace(c.DiscoveryPrefix), "/")
	if c.DiscoveryPrefix == "" {
		c.DiscoveryPrefix = "homeassistant"
	}
	if c.IntervalSec < 10 || c.IntervalSec > 300 {
		c.IntervalSec = 30
	}
	return c
}

func mqttSlug(s string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(s) {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') {
			b.WriteRune(r)
		} else if r == '_' || r == '-' {
			b.WriteByte('_')
		}
	}
	out := strings.Trim(b.String(), "_")
	if out == "" {
		return "dev"
	}
	return out
}

func (s *Server) mqttDeviceID(cfg MQTTConfig) string {
	if b, err := os.ReadFile("/sys/class/net/br0/address"); err == nil {
		mac := strings.TrimSpace(string(b))
		if reMAC.MatchString(mac) {
			return "hg6371f_" + strings.ToLower(strings.ReplaceAll(mac, ":", ""))
		}
	}
	return "hg6371f_" + mqttSlug(cfg.ClientID)
}

func (s *Server) notifyMQTTReload() {
	if s.mqtt == nil {
		return
	}
	select {
	case s.mqtt.reloadCh <- struct{}{}:
	default:
	}
}

func (s *Server) notifyMQTTRadarEvent() {
	if s.mqtt == nil {
		return
	}
	select {
	case s.mqtt.radarCh <- struct{}{}:
	default:
	}
}

func (s *Server) notifyMQTTStaMonEvent() {
	if s.mqtt == nil {
		return
	}
	select {
	case s.mqtt.stamonCh <- struct{}{}:
	default:
	}
}

func (s *Server) notifyMQTTPush() {
	if s.mqtt == nil {
		return
	}
	select {
	case s.mqtt.pushCh <- struct{}{}:
	default:
	}
}

func (s *Server) shutdownMQTT() {
	if s.mqtt == nil {
		return
	}
	me := s.mqtt
	me.mu.Lock()
	if me.stopped {
		me.mu.Unlock()
		return
	}
	me.stopped = true
	conn := me.activeConn
	close(me.stopCh)
	me.mu.Unlock()

	if conn != nil {
		cfg := normalizeMQTTConfig(s.getConfig().MQTT)
		_ = conn.publish(cfg.TopicPrefix+"/availability", []byte("offline"), true)
		conn.disconnect()
	}
	select {
	case <-me.doneCh:
	case <-time.After(2 * time.Second):
	}
}

func (s *Server) startMQTT() {
	me := s.mqtt
	defer close(me.doneCh)

	backoff := 2 * time.Second
	for {
		select {
		case <-me.stopCh:
			return
		default:
		}

		cfg := normalizeMQTTConfig(s.getConfig().MQTT)
		if !cfg.Enabled || cfg.Broker == "" {
			me.mu.Lock()
			me.connected = false
			me.activeConn = nil
			me.lastError = ""
			me.mu.Unlock()
			backoff = 2 * time.Second
			select {
			case <-me.stopCh:
				return
			case <-me.reloadCh:
			case <-time.After(10 * time.Second):
			}
			continue
		}

		availTopic := cfg.TopicPrefix + "/availability"
		conn, err := dialMQTT(cfg, availTopic, "offline", 8*time.Second)
		if err != nil {
			me.mu.Lock()
			me.connected = false
			me.activeConn = nil
			me.lastError = err.Error()
			me.mu.Unlock()
			log.Printf("mqtt: connect %s failed: %v (retry in %v)", cfg.Broker, err, backoff)
			select {
			case <-me.stopCh:
				return
			case <-me.reloadCh:
				backoff = 2 * time.Second
			case <-time.After(backoff):
				if backoff < 30*time.Second {
					backoff *= 2
					if backoff > 30*time.Second {
						backoff = 30 * time.Second
					}
				}
			}
			continue
		}

		backoff = 2 * time.Second
		me.mu.Lock()
		me.connected = true
		me.activeConn = conn
		me.lastError = ""
		me.connectedAt = time.Now()
		me.mu.Unlock()
		log.Printf("mqtt: connected to %s (prefix=%s discovery=%v control=%v)",
			cfg.Broker, cfg.TopicPrefix, cfg.Discovery, cfg.AllowControl)

		err = s.runMQTTSession(conn, cfg)
		me.mu.Lock()
		me.connected = false
		me.activeConn = nil
		if err != nil && !me.stopped {
			me.lastError = err.Error()
		}
		me.mu.Unlock()

		if err == errMQTTReload {
			_ = conn.publish(availTopic, []byte("offline"), true)
			conn.disconnect()
			continue
		}
		conn.close()
		if err != nil && !me.stopped {
			log.Printf("mqtt: session ended: %v", err)
			select {
			case <-me.stopCh:
				return
			case <-me.reloadCh:
			case <-time.After(backoff):
			}
		}
	}
}

var errMQTTReload = errors.New("mqtt reload requested")

func (s *Server) runMQTTSession(conn *mqttConn, cfg MQTTConfig) error {
	me := s.mqtt
	availTopic := cfg.TopicPrefix + "/availability"

	// Subscribe to HA birth topic and (if allowed) command topics
	subs := []string{}
	if cfg.Discovery {
		subs = append(subs, cfg.DiscoveryPrefix+"/status")
	}
	if cfg.AllowControl {
		subs = append(subs, cfg.TopicPrefix+"/cmd/#")
	}
	if len(subs) > 0 {
		if err := conn.subscribe(subs...); err != nil {
			return fmt.Errorf("subscribe: %w", err)
		}
	}

	// Publish online availability, discovery configs, and initial states
	if err := s.mqttPub(conn, availTopic, []byte("online"), true); err != nil {
		return err
	}
	if err := s.publishMQTTFullSync(conn, cfg); err != nil {
		return err
	}

	// Reader goroutine handles PINGRESP, SUBACK, and incoming PUBLISH commands
	rxCh := make(chan mqttMsg, 16)
	readErrCh := make(chan error, 1)
	go func() {
		for {
			hdr, body, err := conn.readPacket(time.Now().Add(time.Duration(mqttKeepAliveSec*2) * time.Second))
			if err != nil {
				readErrCh <- err
				return
			}
			switch hdr & 0xf0 {
			case mqttPktPublish:
				topic, payload, qos, pktID, perr := parseMQTTPublish(hdr, body)
				if perr != nil {
					continue
				}
				if qos == 1 {
					_ = conn.pubAck(pktID)
				}
				select {
				case rxCh <- mqttMsg{Topic: topic, Payload: append([]byte(nil), payload...)}:
				default:
				}
			case mqttPktPingResp, mqttPktSubAck, mqttPktPubAck:
				// expected control packets
			}
		}
	}()

	teleTk := time.NewTicker(time.Duration(cfg.IntervalSec) * time.Second)
	defer teleTk.Stop()
	pingTk := time.NewTicker(time.Duration(mqttKeepAliveSec/2) * time.Second)
	defer pingTk.Stop()

	for {
		select {
		case <-me.stopCh:
			return nil
		case <-me.reloadCh:
			return errMQTTReload
		case err := <-readErrCh:
			return err
		case msg := <-rxCh:
			me.mu.Lock()
			me.rxCount++
			me.mu.Unlock()
			s.handleMQTTIncoming(conn, cfg, msg)
		case <-pingTk.C:
			if err := conn.ping(); err != nil {
				return fmt.Errorf("ping: %w", err)
			}
		case <-me.radarCh:
			if err := s.publishMQTTRadarState(conn, cfg); err != nil {
				return err
			}
		case <-me.stamonCh:
			if err := s.publishMQTTStaMonStates(conn, cfg); err != nil {
				return err
			}
		case <-me.pushCh:
			if err := s.publishMQTTFullSync(conn, cfg); err != nil {
				return err
			}
		case <-teleTk.C:
			if err := s.publishMQTTTelemetry(conn, cfg); err != nil {
				return err
			}
			if err := s.publishMQTTRadarState(conn, cfg); err != nil {
				return err
			}
			if err := s.publishMQTTStaMonStates(conn, cfg); err != nil {
				return err
			}
			if err := s.publishMQTTControlState(conn, cfg); err != nil {
				return err
			}
		}
	}
}

func (s *Server) mqttPub(conn *mqttConn, topic string, payload []byte, retain bool) error {
	if err := conn.publish(topic, payload, retain); err != nil {
		return err
	}
	me := s.mqtt
	me.mu.Lock()
	me.pubCount++
	me.lastPubAt = time.Now()
	me.mu.Unlock()
	return nil
}

func (s *Server) mqttPubJSON(conn *mqttConn, topic string, v any, retain bool) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return s.mqttPub(conn, topic, b, retain)
}

func (s *Server) publishMQTTFullSync(conn *mqttConn, cfg MQTTConfig) error {
	if cfg.Discovery {
		if err := s.publishMQTTDiscovery(conn, cfg); err != nil {
			return err
		}
	}
	if err := s.publishMQTTTelemetry(conn, cfg); err != nil {
		return err
	}
	if err := s.publishMQTTRadarState(conn, cfg); err != nil {
		return err
	}
	if err := s.publishMQTTStaMonStates(conn, cfg); err != nil {
		return err
	}
	return s.publishMQTTControlState(conn, cfg)
}

func (s *Server) mqttDeviceInfo(cfg MQTTConfig) map[string]any {
	devID := s.mqttDeviceID(cfg)
	info := map[string]any{
		"identifiers":  []string{devID},
		"name":         "HG6371F AP",
		"manufacturer": "FiberHome / Broadcom",
		"model":        "HG6371F (BCM68252C)",
	}
	if s.kernelVer != "" {
		info["sw_version"] = "Linux " + s.kernelVer
	}
	if ip := s.br0IPv4(); ip != "" {
		if s.isTLSActive() {
			info["configuration_url"] = "https://" + ip
		} else {
			info["configuration_url"] = "http://" + ip
		}
	}
	return info
}

func (s *Server) br0IPv4() string {
	ifc, err := net.InterfaceByName("br0")
	if err != nil {
		return ""
	}
	addrs, err := ifc.Addrs()
	if err != nil {
		return ""
	}
	for _, a := range addrs {
		if ipn, ok := a.(*net.IPNet); ok {
			if v4 := ipn.IP.To4(); v4 != nil {
				return v4.String()
			}
		}
	}
	return ""
}

func (s *Server) publishMQTTDiscovery(conn *mqttConn, cfg MQTTConfig) error {
	devID := s.mqttDeviceID(cfg)
	devInfo := s.mqttDeviceInfo(cfg)
	availTopic := cfg.TopicPrefix + "/availability"
	teleTopic := cfg.TopicPrefix + "/telemetry"
	radarTopic := cfg.TopicPrefix + "/radar/state"
	ctrlTopic := cfg.TopicPrefix + "/control/state"
	dp := cfg.DiscoveryPrefix

	count := 0
	pubDisc := func(comp, objID string, extra map[string]any) error {
		payload := map[string]any{
			"unique_id":          devID + "_" + objID,
			"object_id":          devID + "_" + objID,
			"availability_topic": availTopic,
			"device":             devInfo,
		}
		for k, v := range extra {
			payload[k] = v
		}
		topic := fmt.Sprintf("%s/%s/%s/%s/config", dp, comp, devID, objID)
		if err := s.mqttPubJSON(conn, topic, payload, true); err != nil {
			return err
		}
		count++
		return nil
	}
	clearDisc := func(comp, objID string) error {
		topic := fmt.Sprintf("%s/%s/%s/%s/config", dp, comp, devID, objID)
		return s.mqttPub(conn, topic, nil, true)
	}

	// 1. CSI Presence Radar
	if err := pubDisc("binary_sensor", "csi_occupied", map[string]any{
		"name":                  "CSI 房间存在雷达",
		"device_class":          "occupancy",
		"state_topic":           radarTopic,
		"value_template":        "{{ 'ON' if value_json.occupied else 'OFF' }}",
		"json_attributes_topic": radarTopic,
		"icon":                  "mdi:motion-sensor",
	}); err != nil {
		return err
	}
	for _, item := range []struct {
		id, name, tmpl, unit, cls, icon string
	}{
		{"csi_score", "CSI 运动指数", "{{ value_json.score }}", "", "", "mdi:waveform"},
		{"csi_phase", "CSI 相位扰动", "{{ value_json.phase_mad }}", "°", "", "mdi:sine-wave"},
		{"csi_rssi0", "CSI 目标 RSSI (RX0)", "{{ value_json.rssi0 }}", "dBm", "signal_strength", ""},
		{"csi_rssi1", "CSI 目标 RSSI (RX1)", "{{ value_json.rssi1 }}", "dBm", "signal_strength", ""},
	} {
		m := map[string]any{
			"name":           item.name,
			"state_topic":    radarTopic,
			"value_template": item.tmpl,
			"state_class":    "measurement",
		}
		if item.unit != "" {
			m["unit_of_measurement"] = item.unit
		}
		if item.cls != "" {
			m["device_class"] = item.cls
		}
		if item.icon != "" {
			m["icon"] = item.icon
		}
		if err := pubDisc("sensor", item.id, m); err != nil {
			return err
		}
	}

	// 2. StaMon proximity targets (dynamic per configured MAC, clears removed targets)
	scfg := s.getConfig().StaMon
	curSlugs := make(map[string]bool, len(scfg.Targets))
	for _, t := range scfg.Targets {
		mac := strings.ToUpper(strings.TrimSpace(t.MAC))
		if !reMAC.MatchString(mac) {
			continue
		}
		slug := strings.ToLower(strings.ReplaceAll(mac, ":", ""))
		curSlugs[slug] = true
		lbl := strings.TrimSpace(t.Label)
		if lbl == "" {
			lbl = s.displayName(mac)
		}
		if lbl == "" {
			lbl = mac
		}
		stTopic := fmt.Sprintf("%s/stamon/%s/state", cfg.TopicPrefix, slug)
		if err := pubDisc("binary_sensor", "stamon_"+slug, map[string]any{
			"name":                  lbl + " 在家",
			"device_class":          "presence",
			"state_topic":           stTopic,
			"value_template":        "{{ 'ON' if value_json.present else 'OFF' }}",
			"json_attributes_topic": stTopic,
			"icon":                  "mdi:cellphone-wireless",
		}); err != nil {
			return err
		}
		if err := pubDisc("sensor", "stamon_"+slug+"_rssi", map[string]any{
			"name":                lbl + " 无线信号",
			"device_class":        "signal_strength",
			"unit_of_measurement": "dBm",
			"state_class":         "measurement",
			"state_topic":         stTopic,
			"value_template":      "{{ value_json.best_rssi if value_json.best_rssi < 0 else none }}",
		}); err != nil {
			return err
		}
	}
	s.mqtt.mu.Lock()
	for oldSlug := range s.mqtt.knownStaSlug {
		if !curSlugs[oldSlug] {
			_ = clearDisc("binary_sensor", "stamon_"+oldSlug)
			_ = clearDisc("sensor", "stamon_"+oldSlug+"_rssi")
		}
	}
	s.mqtt.knownStaSlug = curSlugs
	s.mqtt.mu.Unlock()

	// 3. System & RF Telemetry sensors
	for _, item := range []struct {
		id, name, tmpl, unit, cls, cat, icon string
	}{
		{"soc_temp", "SoC 温度", "{{ value_json.soc_temp }}", "°C", "temperature", "diagnostic", ""},
		{"wl0_temp", "2.4G 射频温度", "{{ value_json.wl0_temp }}", "°C", "temperature", "diagnostic", ""},
		{"wl1_temp", "5G 射频温度", "{{ value_json.wl1_temp }}", "°C", "temperature", "diagnostic", ""},
		{"cpu_pct", "CPU 占用率", "{{ value_json.cpu_pct }}", "%", "", "diagnostic", "mdi:cpu-32-bit"},
		{"mem_pct", "内存使用率", "{{ value_json.mem_pct }}", "%", "", "diagnostic", "mdi:memory"},
		{"clients_total", "在线终端总数", "{{ value_json.clients_total }}", "台", "", "", "mdi:devices"},
		{"wl0_clients", "2.4G 在线终端", "{{ value_json.wl0_clients }}", "台", "", "", "mdi:wifi"},
		{"wl1_clients", "5G 在线终端", "{{ value_json.wl1_clients }}", "台", "", "", "mdi:wifi"},
		{"wl0_retrans", "2.4G 重传率", "{{ value_json.wl0_retrans }}", "%", "", "diagnostic", "mdi:repeat"},
		{"wl1_retrans", "5G 重传率", "{{ value_json.wl1_retrans }}", "%", "", "diagnostic", "mdi:repeat"},
		{"wl0_busy", "2.4G 信道忙碌度", "{{ value_json.wl0_busy }}", "%", "", "diagnostic", "mdi:radio-tower"},
		{"wl1_busy", "5G 信道忙碌度", "{{ value_json.wl1_busy }}", "%", "", "diagnostic", "mdi:radio-tower"},
		{"wl0_noise", "2.4G 底噪", "{{ value_json.wl0_noise }}", "dBm", "signal_strength", "diagnostic", ""},
		{"wl1_noise", "5G 底噪", "{{ value_json.wl1_noise }}", "dBm", "signal_strength", "diagnostic", ""},
		{"uptime_sec", "运行时间", "{{ value_json.uptime_sec }}", "s", "duration", "diagnostic", ""},
	} {
		m := map[string]any{
			"name":           item.name,
			"state_topic":    teleTopic,
			"value_template": item.tmpl,
		}
		if item.id != "uptime_sec" {
			m["state_class"] = "measurement"
		} else {
			m["state_class"] = "total_increasing"
		}
		if item.unit != "" {
			m["unit_of_measurement"] = item.unit
		}
		if item.cls != "" {
			m["device_class"] = item.cls
		}
		if item.cat != "" {
			m["entity_category"] = item.cat
		}
		if item.icon != "" {
			m["icon"] = item.icon
		}
		if err := pubDisc("sensor", item.id, m); err != nil {
			return err
		}
	}

	// 4. Reverse control entities (switches, select, button)
	if cfg.AllowControl {
		if err := pubDisc("switch", "guest_wl0", map[string]any{
			"name":           "2.4G 访客 Wi-Fi",
			"state_topic":    ctrlTopic,
			"value_template": "{{ 'ON' if value_json.guest_wl0 else 'OFF' }}",
			"command_topic":  cfg.TopicPrefix + "/cmd/guest_wl0",
			"payload_on":     "ON",
			"payload_off":    "OFF",
			"icon":           "mdi:wifi-plus",
		}); err != nil {
			return err
		}
		if err := pubDisc("switch", "guest_wl1", map[string]any{
			"name":           "5G 访客 Wi-Fi",
			"state_topic":    ctrlTopic,
			"value_template": "{{ 'ON' if value_json.guest_wl1 else 'OFF' }}",
			"command_topic":  cfg.TopicPrefix + "/cmd/guest_wl1",
			"payload_on":     "ON",
			"payload_off":    "OFF",
			"icon":           "mdi:wifi-plus",
		}); err != nil {
			return err
		}
		if err := pubDisc("switch", "radar_enabled", map[string]any{
			"name":           "CSI 存在雷达开关",
			"state_topic":    ctrlTopic,
			"value_template": "{{ 'ON' if value_json.radar_enabled else 'OFF' }}",
			"command_topic":  cfg.TopicPrefix + "/cmd/radar_enabled",
			"payload_on":     "ON",
			"payload_off":    "OFF",
			"icon":           "mdi:radar",
		}); err != nil {
			return err
		}
		if err := pubDisc("select", "led_mode", map[string]any{
			"name":            "面板指示灯模式",
			"state_topic":     ctrlTopic,
			"value_template":  "{{ value_json.led_mode }}",
			"command_topic":   cfg.TopicPrefix + "/cmd/led_mode",
			"options":         []string{"auto", "off", "schedule"},
			"entity_category": "config",
			"icon":            "mdi:led-on",
		}); err != nil {
			return err
		}
		if err := pubDisc("button", "radar_calibrate", map[string]any{
			"name":            "CSI 30 秒空房标定",
			"command_topic":   cfg.TopicPrefix + "/cmd/radar_calibrate",
			"payload_press":   "PRESS",
			"entity_category": "config",
			"icon":            "mdi:tune-variant",
		}); err != nil {
			return err
		}
	} else {
		_ = clearDisc("switch", "guest_wl0")
		_ = clearDisc("switch", "guest_wl1")
		_ = clearDisc("switch", "radar_enabled")
		_ = clearDisc("select", "led_mode")
		_ = clearDisc("button", "radar_calibrate")
	}

	s.mqtt.mu.Lock()
	s.mqtt.discoveryCount = count
	s.mqtt.mu.Unlock()
	return nil
}

func (s *Server) publishMQTTTelemetry(conn *mqttConn, cfg MQTTConfig) error {
	ov := s.collectOverview()
	memTot := asFloat(ov["mem_total_mb"])
	memAvail := asFloat(ov["mem_avail_mb"])
	memPct := 0.0
	if memTot > 0 {
		memPct = math.Round(((memTot-memAvail)/memTot)*1000) / 10
	}

	payload := map[string]any{
		"ts":            time.Now().Unix(),
		"uptime_sec":    int64(asFloat(ov["uptime_sec"])),
		"cpu_pct":       math.Round(asFloat(ov["cpu_pct"])*10) / 10,
		"mem_pct":       memPct,
		"soc_temp":      math.Round(asFloat(ov["temp_c"])*10) / 10,
		"wl0_temp":      math.Round(asFloat(ov["wl0_temp_c"])*10) / 10,
		"wl1_temp":      math.Round(asFloat(ov["wl1_temp_c"])*10) / 10,
		"clients_total": 0,
		"wl0_clients":   0,
		"wl1_clients":   0,
		"wl0_retrans":   0.0,
		"wl1_retrans":   0.0,
		"wl0_busy":      0,
		"wl1_busy":      0,
		"wl0_noise":     0,
		"wl1_noise":     0,
	}
	if radios, ok := ov["radios"].([]RadioSummary); ok {
		tot := 0
		for _, rd := range radios {
			tot += rd.Clients
			switch rd.Iface {
			case "wl0":
				payload["wl0_clients"] = rd.Clients
				payload["wl0_retrans"] = math.Round(rd.Air.TxRetrans*10) / 10
				payload["wl0_busy"] = rd.Air.BusyPct
				payload["wl0_noise"] = rd.Air.NoiseDBm
			case "wl1":
				payload["wl1_clients"] = rd.Clients
				payload["wl1_retrans"] = math.Round(rd.Air.TxRetrans*10) / 10
				payload["wl1_busy"] = rd.Air.BusyPct
				payload["wl1_noise"] = rd.Air.NoiseDBm
			}
		}
		payload["clients_total"] = tot
	}
	return s.mqttPubJSON(conn, cfg.TopicPrefix+"/telemetry", payload, true)
}

func (s *Server) publishMQTTRadarState(conn *mqttConn, cfg MQTTConfig) error {
	if s.radar == nil {
		return nil
	}
	rcfg := s.getConfig().Radar
	re := s.radar
	re.mu.Lock()
	payload := map[string]any{
		"ts":         time.Now().Unix(),
		"enabled":    rcfg.Enabled,
		"occupied":   re.occupied,
		"state":      re.state,
		"score":      re.score,
		"phase_mad":  re.phaseMAD,
		"rssi0":      re.rssi0,
		"rssi1":      re.rssi1,
		"chanspec":   fmt.Sprintf("0x%04x", re.chanspec),
		"frames":     re.frames,
		"iface":      rcfg.Iface,
		"target_mac": rcfg.TargetMAC,
	}
	re.mu.Unlock()
	return s.mqttPubJSON(conn, cfg.TopicPrefix+"/radar/state", payload, true)
}

func (s *Server) publishMQTTStaMonStates(conn *mqttConn, cfg MQTTConfig) error {
	if s.stamon == nil {
		return nil
	}
	scfg := s.getConfig().StaMon
	s.stamon.mu.Lock()
	snap := make([]StaMonState, 0, len(scfg.Targets))
	for _, t := range scfg.Targets {
		mac := strings.ToUpper(strings.TrimSpace(t.MAC))
		if !reMAC.MatchString(mac) {
			continue
		}
		if st, ok := s.stamon.states[mac]; ok && st != nil {
			snap = append(snap, *st)
		} else {
			snap = append(snap, StaMonState{MAC: mac, Label: t.Label})
		}
	}
	s.stamon.mu.Unlock()

	for _, st := range snap {
		slug := strings.ToLower(strings.ReplaceAll(st.MAC, ":", ""))
		topic := fmt.Sprintf("%s/stamon/%s/state", cfg.TopicPrefix, slug)
		if err := s.mqttPubJSON(conn, topic, st, true); err != nil {
			return err
		}
	}
	return nil
}

func (s *Server) publishMQTTControlState(conn *mqttConn, cfg MQTTConfig) error {
	pcfg := s.getConfig()
	payload := map[string]any{
		"guest_wl0":     pcfg.Radios["wl0"].Guest.Enabled,
		"guest_wl1":     pcfg.Radios["wl1"].Guest.Enabled,
		"radar_enabled": pcfg.Radar.Enabled,
		"led_mode":      pcfg.LED.Mode,
	}
	return s.mqttPubJSON(conn, cfg.TopicPrefix+"/control/state", payload, true)
}

func (s *Server) handleMQTTIncoming(conn *mqttConn, cfg MQTTConfig, msg mqttMsg) {
	val := strings.TrimSpace(string(msg.Payload))
	// Home Assistant birth message -> re-announce discovery + states immediately
	if cfg.Discovery && msg.Topic == cfg.DiscoveryPrefix+"/status" {
		if strings.EqualFold(val, "online") {
			log.Printf("mqtt: Home Assistant online birth received, republishing discovery")
			_ = s.publishMQTTFullSync(conn, cfg)
		}
		return
	}
	if !cfg.AllowControl {
		return
	}
	cmdPrefix := cfg.TopicPrefix + "/cmd/"
	if !strings.HasPrefix(msg.Topic, cmdPrefix) {
		return
	}
	cmd := strings.TrimPrefix(msg.Topic, cmdPrefix)
	upper := strings.ToUpper(val)

	switch cmd {
	case "guest_wl0", "guest_wl1":
		iface := "wl0"
		if cmd == "guest_wl1" {
			iface = "wl1"
		}
		want := upper == "ON" || upper == "1" || upper == "TRUE"
		if upper != "ON" && upper != "OFF" && upper != "1" && upper != "0" && upper != "TRUE" && upper != "FALSE" {
			return
		}
		pcfg := s.getConfig()
		g := pcfg.Radios[iface].Guest
		if g.Enabled == want {
			_ = s.publishMQTTControlState(conn, cfg)
			return
		}
		g.Enabled = want
		if err := s.saveGuestConfig(iface, g); err != nil {
			log.Printf("mqtt: toggle %s failed: %v", cmd, err)
		} else {
			s.invalidateOverviewCache()
		}
		_ = s.publishMQTTControlState(conn, cfg)

	case "led_mode":
		mode := strings.ToLower(val)
		if mode != "auto" && mode != "off" && mode != "schedule" {
			return
		}
		_ = s.updateConfig(func(c *PanelConfig) {
			c.LED.Mode = mode
		})
		s.syncStatusLED()
		_ = s.publishMQTTControlState(conn, cfg)

	case "radar_enabled":
		want := upper == "ON" || upper == "1" || upper == "TRUE"
		if upper != "ON" && upper != "OFF" && upper != "1" && upper != "0" && upper != "TRUE" && upper != "FALSE" {
			return
		}
		pcfg := s.getConfig()
		if want && !reMAC.MatchString(pcfg.Radar.TargetMAC) {
			log.Printf("mqtt: cannot enable radar without target_mac")
			_ = s.publishMQTTControlState(conn, cfg)
			return
		}
		_ = s.updateConfig(func(c *PanelConfig) {
			c.Radar.Enabled = want
		})
		if s.radar != nil {
			s.radar.notifyReload()
		}
		_ = s.publishMQTTControlState(conn, cfg)
		_ = s.publishMQTTRadarState(conn, cfg)

	case "radar_calibrate":
		if s.radar == nil {
			return
		}
		pcfg := s.getConfig().Radar
		if !pcfg.Enabled || !reMAC.MatchString(pcfg.TargetMAC) {
			return
		}
		s.radar.mu.Lock()
		if !s.radar.calActive {
			s.radar.calActive = true
			s.radar.calStart = time.Now()
			s.radar.calDur = 30 * time.Second
			s.radar.calFeats = nil
			s.radar.calLastMsg = "Home Assistant 触发 30 秒空房底噪标定，请保持房间静止…"
		}
		s.radar.mu.Unlock()
	}
}

// ── HTTP API Handlers ───────────────────────────────────────────────────────

func (s *Server) handleMQTT(w http.ResponseWriter, r *http.Request) {
	cfg := normalizeMQTTConfig(s.getConfig().MQTT)
	hasPass := cfg.Password != ""
	cfg.Password = "" // never leak stored broker password over API

	me := s.mqtt
	me.mu.Lock()
	connected := me.connected
	lastErr := me.lastError
	pubCnt := me.pubCount
	rxCnt := me.rxCount
	discCnt := me.discoveryCount
	connSec := int64(0)
	if connected && !me.connectedAt.IsZero() {
		connSec = int64(time.Since(me.connectedAt).Seconds())
	}
	lastPubSec := int64(-1)
	if !me.lastPubAt.IsZero() {
		lastPubSec = int64(time.Since(me.lastPubAt).Seconds())
	}
	me.mu.Unlock()

	writeJSON(w, r, http.StatusOK, map[string]any{
		"config":          cfg,
		"has_password":    hasPass,
		"device_id":       s.mqttDeviceID(cfg),
		"connected":       connected,
		"last_error":      lastErr,
		"connected_sec":   connSec,
		"last_pub_sec":    lastPubSec,
		"pub_count":       pubCnt,
		"rx_count":        rxCnt,
		"discovery_count": discCnt,
	})
}

func (s *Server) handleMQTTConfig(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		MQTTConfig
		ClearPassword bool `json:"clear_password"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 8192)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	mc := normalizeMQTTConfig(req.MQTTConfig)
	if mc.Enabled || mc.Broker != "" {
		if _, _, err := parseMQTTBroker(mc.Broker); err != nil {
			writeErr(w, r, http.StatusBadRequest, err.Error())
			return
		}
	}
	if err := s.updateConfig(func(c *PanelConfig) {
		if mc.Password == "" && !req.ClearPassword {
			mc.Password = c.MQTT.Password
		}
		c.MQTT = mc
	}); err != nil {
		writeErr(w, r, http.StatusInternalServerError, err.Error())
		return
	}
	s.notifyMQTTReload()
	mc.Password = ""
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "config": mc})
}

func (s *Server) handleMQTTTest(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		MQTTConfig
		ClearPassword bool `json:"clear_password"`
	}
	_ = json.NewDecoder(io.LimitReader(r.Body, 8192)).Decode(&req)
	saved := s.getConfig().MQTT
	mc := normalizeMQTTConfig(req.MQTTConfig)
	if mc.Broker == "" {
		mc = normalizeMQTTConfig(saved)
	} else if mc.Password == "" && !req.ClearPassword {
		mc.Password = saved.Password
	}
	mc.ClientID = mc.ClientID + "-test"

	t0 := time.Now()
	conn, err := dialMQTT(mc, "", "", 6*time.Second)
	if err != nil {
		writeErr(w, r, http.StatusBadGateway, fmt.Sprintf("连接失败: %v", err))
		return
	}
	conn.disconnect()
	ms := time.Since(t0).Milliseconds()
	writeJSON(w, r, http.StatusOK, map[string]any{
		"ok":         true,
		"elapsed_ms": ms,
		"message":    fmt.Sprintf("MQTT 3.1.1 握手成功（耗时 %dms）", ms),
	})
}

func (s *Server) handleMQTTPublishNow(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	s.mqtt.mu.Lock()
	connected := s.mqtt.connected
	s.mqtt.mu.Unlock()
	if !connected {
		writeErr(w, r, http.StatusBadRequest, "MQTT 尚未连接，请先启用并保存有效配置")
		return
	}
	s.notifyMQTTPush()
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "message": "已触发立即推送 Discovery 与全部实体状态"})
}
