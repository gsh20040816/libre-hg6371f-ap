package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"net"
	"os"
	"strings"
	"time"
)

const (
	mdnsAddrStr   = "224.0.0.251:5353"
	hostCacheFile = "/tmp/ap-hosts.json" // RAM tmpfs: survives ap-panel restarts with zero flash wear
)

type hostCacheBlob struct {
	MacToIP   map[string]string `json:"mac_to_ip"`
	MacToHost map[string]string `json:"mac_to_host"`
	IPToHost  map[string]string `json:"ip_to_host"`
}

// isUUIDHost reports whether s is a standard 36-char RFC 4122 UUID
// (e.g. ephemeral WebRTC mDNS ICE candidate hostnames like "<uuid>.local").
func isUUIDHost(s string) bool {
	if len(s) != 36 {
		return false
	}
	for i := 0; i < 36; i++ {
		c := s[i]
		if i == 8 || i == 13 || i == 18 || i == 23 {
			if c != '-' {
				return false
			}
			continue
		}
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
			return false
		}
	}
	return true
}

func cleanDiscoveredHost(raw string) string {
	h := strings.TrimSpace(raw)
	h = strings.TrimRight(h, "\x00.")
	low := strings.ToLower(h)
	for _, suf := range []string{".local", ".lan", ".home.arpa", ".home"} {
		if strings.HasSuffix(low, suf) {
			h = h[:len(h)-len(suf)]
			low = strings.ToLower(h)
			break
		}
	}
	// Ignore DNS-SD service names like "_airplay._tcp" or "foo._http._tcp"
	if h == "" || strings.HasPrefix(h, "_") || strings.Contains(h, "._tcp") || strings.Contains(h, "._udp") {
		return ""
	}
	// Ignore ephemeral WebRTC/privacy mDNS UUID hostnames
	if isUUIDHost(h) {
		return ""
	}
	// Strip non-printable ASCII control chars (some IoT DHCP options have stray bytes)
	var b strings.Builder
	for _, r := range h {
		if r >= 0x20 && r != 0x7f {
			b.WriteRune(r)
		}
	}
	h = strings.TrimSpace(b.String())
	if len(h) > 64 {
		h = h[:64]
	}
	return h
}

func (s *Server) markHostCacheDirty() {
	s.hostMu.Lock()
	s.hostCacheDirty = true
	s.hostMu.Unlock()
}

func (s *Server) loadHostCache() {
	raw, err := os.ReadFile(hostCacheFile)
	if err != nil {
		return
	}
	var blob hostCacheBlob
	if json.Unmarshal(raw, &blob) != nil {
		return
	}
	s.arpMu.Lock()
	for mac, ip := range blob.MacToIP {
		mac = strings.ToUpper(strings.TrimSpace(mac))
		if mac != "" && ip != "" {
			s.macToIP[mac] = ip
		}
	}
	s.arpMu.Unlock()

	s.hostMu.Lock()
	if s.macToHost == nil {
		s.macToHost = make(map[string]string)
	}
	if s.ipToHost == nil {
		s.ipToHost = make(map[string]string)
	}
	for mac, h := range blob.MacToHost {
		mac = strings.ToUpper(strings.TrimSpace(mac))
		if h = cleanDiscoveredHost(h); mac != "" && h != "" {
			s.macToHost[mac] = h
		}
	}
	for ip, h := range blob.IPToHost {
		if h = cleanDiscoveredHost(h); ip != "" && h != "" {
			s.ipToHost[ip] = h
		}
	}
	s.hostMu.Unlock()
}

func (s *Server) flushHostCacheIfDirty() {
	s.hostMu.Lock()
	if !s.hostCacheDirty {
		s.hostMu.Unlock()
		return
	}
	s.hostCacheDirty = false
	m2h := make(map[string]string, len(s.macToHost))
	for k, v := range s.macToHost {
		m2h[k] = v
	}
	i2h := make(map[string]string, len(s.ipToHost))
	for k, v := range s.ipToHost {
		i2h[k] = v
	}
	s.hostMu.Unlock()

	s.arpMu.RLock()
	m2i := make(map[string]string, len(s.macToIP))
	for k, v := range s.macToIP {
		m2i[k] = v
	}
	s.arpMu.RUnlock()

	b, err := json.Marshal(hostCacheBlob{
		MacToIP:   m2i,
		MacToHost: m2h,
		IPToHost:  i2h,
	})
	if err != nil {
		return
	}
	tmp := hostCacheFile + ".tmp"
	if os.WriteFile(tmp, b, 0600) == nil {
		_ = os.Rename(tmp, hostCacheFile)
	}
}

func (s *Server) recordMACHost(mac, host string) {
	mac = strings.ToUpper(strings.TrimSpace(mac))
	host = cleanDiscoveredHost(host)
	if mac == "" || host == "" {
		return
	}
	s.hostMu.Lock()
	if s.macToHost == nil {
		s.macToHost = make(map[string]string)
	}
	if s.macToHost[mac] != host {
		s.macToHost[mac] = host
		s.hostCacheDirty = true
	}
	s.hostMu.Unlock()
}

func (s *Server) recordIPHost(ip, host string) {
	host = cleanDiscoveredHost(host)
	if ip == "" || host == "" {
		return
	}
	s.hostMu.Lock()
	if s.ipToHost == nil {
		s.ipToHost = make(map[string]string)
	}
	if s.ipToHost[ip] != host {
		s.ipToHost[ip] = host
		s.hostCacheDirty = true
	}
	s.hostMu.Unlock()
}

func (s *Server) lookupHostname(mac string) string {
	mac = strings.ToUpper(strings.TrimSpace(mac))
	if mac == "" {
		return ""
	}
	s.hostMu.RLock()
	if h := s.macToHost[mac]; h != "" {
		s.hostMu.RUnlock()
		return h
	}
	s.hostMu.RUnlock()

	ip := s.lookupIP(mac)
	if ip == "" {
		return ""
	}
	s.hostMu.RLock()
	h := s.ipToHost[ip]
	s.hostMu.RUnlock()
	return h
}

// displayName returns user-defined note if present, otherwise auto-discovered DHCP/mDNS/DNS hostname.
func (s *Server) displayName(mac string) string {
	if n := s.getNote(mac); n != "" {
		return n
	}
	return s.lookupHostname(mac)
}

// readDNSName decodes an RFC 1035 domain name with pointer compression support.
func readDNSName(pkt []byte, off int) (string, int, bool) {
	var labels []string
	origNext := -1
	visited := 0
	for off < len(pkt) {
		visited++
		if visited > 64 {
			return "", 0, false
		}
		l := int(pkt[off])
		if l == 0 {
			if origNext < 0 {
				origNext = off + 1
			}
			break
		}
		if l&0xc0 == 0xc0 {
			if off+1 >= len(pkt) {
				return "", 0, false
			}
			ptr := int(binary.BigEndian.Uint16(pkt[off:off+2]) & 0x3fff)
			if origNext < 0 {
				origNext = off + 2
			}
			off = ptr
			continue
		}
		if l&0xc0 != 0 || off+1+l > len(pkt) {
			return "", 0, false
		}
		labels = append(labels, string(pkt[off+1:off+1+l]))
		off += 1 + l
	}
	if origNext < 0 {
		return "", 0, false
	}
	return strings.Join(labels, "."), origNext, true
}

func parseInAddrArpa(name string) string {
	low := strings.ToLower(strings.TrimSuffix(name, "."))
	if !strings.HasSuffix(low, ".in-addr.arpa") {
		return ""
	}
	prefix := strings.TrimSuffix(low, ".in-addr.arpa")
	parts := strings.Split(prefix, ".")
	if len(parts) != 4 {
		return ""
	}
	ipStr := parts[3] + "." + parts[2] + "." + parts[1] + "." + parts[0]
	ip := net.ParseIP(ipStr).To4()
	if ip == nil {
		return ""
	}
	return ip.String()
}

// parseMDNSPacket extracts IPv4 <-> hostname mappings from mDNS/LLMNR response packets.
// Returns the last valid hostname learned from the packet (if any).
func (s *Server) parseMDNSPacket(pkt []byte) string {
	if len(pkt) < 12 {
		return ""
	}
	flags := binary.BigEndian.Uint16(pkt[2:4])
	if flags&0x8000 == 0 {
		return "" // ignore queries, parse responses only
	}
	qdCount := int(binary.BigEndian.Uint16(pkt[4:6]))
	anCount := int(binary.BigEndian.Uint16(pkt[6:8]))
	nsCount := int(binary.BigEndian.Uint16(pkt[8:10]))
	arCount := int(binary.BigEndian.Uint16(pkt[10:12]))

	off := 12
	for i := 0; i < qdCount; i++ {
		_, next, ok := readDNSName(pkt, off)
		if !ok || next+4 > len(pkt) {
			return ""
		}
		off = next + 4
	}

	var learned string
	totalRR := anCount + nsCount + arCount
	for i := 0; i < totalRR && off < len(pkt); i++ {
		name, next, ok := readDNSName(pkt, off)
		if !ok || next+10 > len(pkt) {
			return learned
		}
		rtype := binary.BigEndian.Uint16(pkt[next : next+2])
		rdlen := int(binary.BigEndian.Uint16(pkt[next+8 : next+10]))
		rdataOff := next + 10
		if rdataOff+rdlen > len(pkt) {
			return learned
		}
		switch rtype {
		case 1: // A record: foo.local -> IPv4
			if rdlen == 4 {
				ip := net.IPv4(pkt[rdataOff], pkt[rdataOff+1], pkt[rdataOff+2], pkt[rdataOff+3])
				if !ip.IsUnspecified() && !ip.IsLoopback() && !ip.IsMulticast() {
					if h := cleanDiscoveredHost(name); h != "" {
						s.recordIPHost(ip.String(), h)
						learned = h
					}
				}
			}
		case 12: // PTR record: d.c.b.a.in-addr.arpa -> foo.local
			if ip := parseInAddrArpa(name); ip != "" {
				if target, _, ok2 := readDNSName(pkt, rdataOff); ok2 {
					if h := cleanDiscoveredHost(target); h != "" {
						s.recordIPHost(ip, h)
						learned = h
					}
				}
			}
		}
		off = rdataOff + rdlen
	}
	return learned
}

func buildDNSQuery(labels []string, qtype uint16) []byte {
	var buf []byte
	buf = append(buf,
		0x00, 0x00, // ID = 0 (mDNS)
		0x00, 0x00, // Flags = standard query
		0x00, 0x01, // QDCOUNT = 1
		0x00, 0x00, // ANCOUNT = 0
		0x00, 0x00, // NSCOUNT = 0
		0x00, 0x00, // ARCOUNT = 0
	)
	for _, lbl := range labels {
		buf = append(buf, byte(len(lbl)))
		buf = append(buf, lbl...)
	}
	buf = append(buf, 0x00, byte(qtype>>8), byte(qtype&0xff), 0x00, 0x01)
	return buf
}

func (s *Server) sendMDNSQuery(labels []string, qtype uint16) {
	q := buildDNSQuery(labels, qtype)
	dst, err := net.ResolveUDPAddr("udp4", mdnsAddrStr)
	if err != nil {
		return
	}
	conn, err := net.DialUDP("udp4", nil, dst)
	if err != nil {
		return
	}
	defer conn.Close()
	_ = conn.SetWriteDeadline(time.Now().Add(500 * time.Millisecond))
	_, _ = conn.Write(q)
}

func (s *Server) sendMDNSPTRQuery(ipStr string) {
	ip := net.ParseIP(ipStr).To4()
	if ip == nil {
		return
	}
	parts := strings.Split(ip.String(), ".")
	if len(parts) != 4 {
		return
	}
	s.sendMDNSQuery([]string{parts[3], parts[2], parts[1], parts[0], "in-addr", "arpa"}, 12)
}

func (s *Server) probeHostnamesForKnownIPs() {
	s.arpMu.RLock()
	ips := make([]string, 0, len(s.macToIP))
	seen := make(map[string]bool)
	for _, ip := range s.macToIP {
		if ip != "" && !seen[ip] {
			seen[ip] = true
			ips = append(ips, ip)
		}
	}
	s.arpMu.RUnlock()

	now := time.Now()
	for _, ip := range ips {
		s.hostMu.Lock()
		has := s.ipToHost[ip] != ""
		last := s.hostQueried[ip]
		if has || now.Sub(last) < 5*time.Minute {
			s.hostMu.Unlock()
			continue
		}
		if s.hostQueried == nil {
			s.hostQueried = make(map[string]time.Time)
		}
		s.hostQueried[ip] = now
		s.hostMu.Unlock()

		// 1) Try upstream router PTR DNS (e.g. OpenWrt dnsmasq DHCP hostnames)
		ctx, cancel := context.WithTimeout(context.Background(), 1200*time.Millisecond)
		names, err := net.DefaultResolver.LookupAddr(ctx, ip)
		cancel()
		if err == nil && len(names) > 0 {
			for _, n := range names {
				if h := cleanDiscoveredHost(n); h != "" {
					s.recordIPHost(ip, h)
					break
				}
			}
		}

		// 2) If still unknown, send a lightweight mDNS reverse PTR query (response caught by listener)
		s.hostMu.RLock()
		stillEmpty := s.ipToHost[ip] == ""
		s.hostMu.RUnlock()
		if stillEmpty {
			s.sendMDNSPTRQuery(ip)
		}
	}
}

// startHostDiscoverer passively listens for mDNS (224.0.0.251:5353) responses on br0,
// periodically resolves unknown client IPs via upstream DNS PTR + mDNS reverse PTR,
// and persists learned MAC/IP/hostname tables to RAM tmpfs (/tmp/ap-hosts.json).
func (s *Server) startHostDiscoverer() {
	// Passive mDNS multicast listener (catches responses even on non-Linux or if BPF falls back)
	go func() {
		addr, err := net.ResolveUDPAddr("udp4", mdnsAddrStr)
		if err != nil {
			return
		}
		for {
			ifi, _ := net.InterfaceByName("br0")
			conn, err := net.ListenMulticastUDP("udp4", ifi, addr)
			if err != nil && ifi != nil {
				conn, err = net.ListenMulticastUDP("udp4", nil, addr)
			}
			if err != nil {
				time.Sleep(15 * time.Second)
				continue
			}
			_ = conn.SetReadBuffer(65536)
			buf := make([]byte, 4096)
			for {
				n, _, err := conn.ReadFromUDP(buf)
				if err != nil {
					break
				}
				s.parseMDNSPacket(buf[:n])
			}
			_ = conn.Close()
			time.Sleep(5 * time.Second)
		}
	}()

	// Periodic lightweight PTR resolver + RAM tmpfs cache flusher
	time.Sleep(4 * time.Second)
	// Gentle one-shot mDNS browse on startup to elicit .local announcements from active LAN hosts
	s.sendMDNSQuery([]string{"_services", "_dns-sd", "_udp", "local"}, 12)
	s.probeHostnamesForKnownIPs()
	s.flushHostCacheIfDirty()

	tk := time.NewTicker(15 * time.Second)
	defer tk.Stop()
	ticks := 0
	for range tk.C {
		ticks++
		if ticks%2 == 0 {
			s.probeHostnamesForKnownIPs()
		}
		s.flushHostCacheIfDirty()
	}
}
