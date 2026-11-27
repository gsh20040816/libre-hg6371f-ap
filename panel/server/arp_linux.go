//go:build linux

package main

import (
	"encoding/binary"
	"fmt"
	"log"
	"net"
	"syscall"
	"unsafe"
)

type sockFilter struct {
	Code uint16
	Jt   uint8
	Jf   uint8
	K    uint32
}

type sockFprog struct {
	Len    uint16
	_      uint16
	Filter *sockFilter
}

// Kernel-level cBPF filter: accepts ONLY ARP (0x0806) and non-fragmented IPv4 UDP
// packets on ports 67/68 (DHCP), 5353 (mDNS), or 5355 (LLMNR).
// All normal data traffic is rejected inside the kernel with zero userspace copies.
var controlPlaneBPF = []sockFilter{
	{0x28, 0, 0, 12},        //  0: ldh [12] (EtherType)
	{0x15, 17, 0, 0x0806},   //  1: jeq #0x0806 -> 19 (ACCEPT ARP)
	{0x15, 0, 17, 0x0800},   //  2: jeq #0x0800 -> next, else -> 20 (REJECT)
	{0x30, 0, 0, 23},        //  3: ldb [23] (IPv4 protocol)
	{0x15, 0, 15, 17},       //  4: jeq #17 (UDP) -> next, else -> 20 (REJECT)
	{0x28, 0, 0, 20},        //  5: ldh [20] (flags + frag offset)
	{0x45, 13, 0, 0x1fff},   //  6: jset #0x1fff -> 20 (REJECT fragmented)
	{0xb1, 0, 0, 14},        //  7: ldx 4*([14]&0xf) (IPv4 header len)
	{0x48, 0, 0, 14},        //  8: ldh [x+14] (UDP src port)
	{0x15, 9, 0, 67},        //  9: jeq #67 -> 19
	{0x15, 8, 0, 68},        // 10: jeq #68 -> 19
	{0x15, 7, 0, 5353},      // 11: jeq #5353 -> 19
	{0x15, 6, 0, 5355},      // 12: jeq #5355 -> 19
	{0x48, 0, 0, 16},        // 13: ldh [x+16] (UDP dst port)
	{0x15, 4, 0, 67},        // 14: jeq #67 -> 19
	{0x15, 3, 0, 68},        // 15: jeq #68 -> 19
	{0x15, 2, 0, 5353},      // 16: jeq #5353 -> 19
	{0x15, 1, 0, 5355},      // 17: jeq #5355 -> 19
	{0x05, 0, 0, 1},         // 18: jmp -> 20 (REJECT)
	{0x06, 0, 0, 1600},      // 19: ret #1600 (ACCEPT)
	{0x06, 0, 0, 0},         // 20: ret #0 (REJECT)
}

func attachControlBPF(fd int) error {
	prog := sockFprog{
		Len:    uint16(len(controlPlaneBPF)),
		Filter: &controlPlaneBPF[0],
	}
	_, _, errno := syscall.Syscall6(
		syscall.SYS_SETSOCKOPT,
		uintptr(fd),
		uintptr(syscall.SOL_SOCKET),
		uintptr(syscall.SO_ATTACH_FILTER),
		uintptr(unsafe.Pointer(&prog)),
		unsafe.Sizeof(prog),
		0,
	)
	if errno != 0 {
		return errno
	}
	return nil
}

func htons(v uint16) int {
	return int((v<<8)&0xff00 | (v>>8)&0x00ff)
}

func isValidClientIP(ip net.IP) bool {
	if ip == nil {
		return false
	}
	ip4 := ip.To4()
	if ip4 == nil || ip4.IsUnspecified() || ip4.IsLoopback() || ip4.IsMulticast() {
		return false
	}
	// Ignore 255.255.255.255 broadcast and 169.254.x.x link-local
	if ip4[0] == 255 || (ip4[0] == 169 && ip4[1] == 254) {
		return false
	}
	return true
}

func formatMAC(b []byte) string {
	return fmt.Sprintf("%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5])
}

func isZeroOrBcastMAC(b []byte) bool {
	allZero, allFF := true, true
	for _, x := range b {
		if x != 0x00 {
			allZero = false
		}
		if x != 0xff {
			allFF = false
		}
	}
	// Also skip multicast MACs (LSB of first octet == 1)
	return allZero || allFF || (b[0]&0x01) != 0
}

// parseDHCPPayload parses BOOTP/DHCP (UDP 67/68) to passively learn client MAC -> IP & Option 12 hostname.
func (s *Server) parseDHCPPayload(p []byte) {
	// Minimum BOOTP header (236) + 4-byte DHCP magic cookie (99.130.83.99)
	if len(p) < 240 {
		return
	}
	// HTYPE == 1 (Ethernet), HLEN == 6
	if p[1] != 1 || p[2] != 6 {
		return
	}
	if p[236] != 99 || p[237] != 130 || p[238] != 83 || p[239] != 99 {
		return
	}
	chaddr := p[28:34]
	if isZeroOrBcastMAC(chaddr) {
		return
	}
	mac := formatMAC(chaddr)

	// ciaddr (bytes 12..15) and yiaddr (bytes 16..19)
	ciaddr := net.IPv4(p[12], p[13], p[14], p[15])
	yiaddr := net.IPv4(p[16], p[17], p[18], p[19])
	var reqIP net.IP
	var hostName string

	off := 240
	for off < len(p) {
		opt := p[off]
		if opt == 255 { // End
			break
		}
		if opt == 0 { // Pad
			off++
			continue
		}
		if off+1 >= len(p) {
			break
		}
		olen := int(p[off+1])
		off += 2
		if off+olen > len(p) {
			break
		}
		val := p[off : off+olen]
		switch opt {
		case 12: // Host Name
			if olen > 0 {
				hostName = string(val)
			}
		case 50: // Requested IP Address
			if olen == 4 {
				reqIP = net.IPv4(val[0], val[1], val[2], val[3])
			}
		case 81: // Client FQDN (flags + rcode1 + rcode2 + domain)
			if hostName == "" && olen > 3 {
				hostName = string(val[3:])
			}
		}
		off += olen
	}

	var bestIP string
	switch {
	case isValidClientIP(yiaddr):
		bestIP = yiaddr.String()
	case isValidClientIP(ciaddr):
		bestIP = ciaddr.String()
	case isValidClientIP(reqIP):
		bestIP = reqIP.String()
	}
	if bestIP != "" {
		s.recordMACIP(mac, bestIP)
	}
	if hostName != "" {
		s.recordMACHost(mac, hostName)
		if bestIP != "" {
			s.recordIPHost(bestIP, hostName)
		}
	}
}

// startARPSnooper listens passively via a kernel-filtered AF_PACKET socket for:
//   - ARP (0x0806): MAC -> IPv4
//   - DHCP (UDP 67/68): MAC -> IPv4 + Option 12 Hostname (works for IoT/Linux/Windows/Android)
//   - mDNS / LLMNR (UDP 5353/5355): Ethernet src MAC -> src IPv4 + .local / LLMNR Hostname
func (s *Server) startARPSnooper() {
	proto := htons(0x0003) // ETH_P_ALL
	fd, err := syscall.Socket(syscall.AF_PACKET, syscall.SOCK_RAW, proto)
	if err != nil {
		return
	}
	defer syscall.Close(fd)

	// Attach BPF filter BEFORE binding; if kernel rejects it, fall back to ARP-only socket.
	if err := attachControlBPF(fd); err != nil {
		log.Printf("snooper: BPF attach failed (%v), falling back to ARP-only", err)
		_ = syscall.Close(fd)
		proto = htons(0x0806)
		fd, err = syscall.Socket(syscall.AF_PACKET, syscall.SOCK_RAW, proto)
		if err != nil {
			return
		}
	}

	sa := &syscall.SockaddrLinklayer{
		Protocol: uint16(proto),
		Ifindex:  0, // all interfaces (br0, wl0, wl1, lan0)
	}
	if err := syscall.Bind(fd, sa); err != nil {
		return
	}

	buf := make([]byte, 1600)
	for {
		n, err := syscall.Read(fd, buf)
		if err != nil || n < 42 {
			continue
		}
		ethType := binary.BigEndian.Uint16(buf[12:14])
		switch ethType {
		case 0x0806: // ARP
			if buf[14] != 0x00 || buf[15] != 0x01 || buf[16] != 0x08 || buf[17] != 0x00 {
				continue
			}
			ip := net.IPv4(buf[28], buf[29], buf[30], buf[31])
			if !isValidClientIP(ip) || isZeroOrBcastMAC(buf[22:28]) {
				continue
			}
			s.recordMACIP(formatMAC(buf[22:28]), ip.String())

		case 0x0800: // IPv4 UDP (BPF already guarantees proto==17 & non-fragmented)
			if n < 14+20+8 {
				continue
			}
			ihl := int(buf[14]&0x0f) * 4
			if ihl < 20 || 14+ihl+8 > n {
				continue
			}
			udpOff := 14 + ihl
			srcPort := binary.BigEndian.Uint16(buf[udpOff : udpOff+2])
			dstPort := binary.BigEndian.Uint16(buf[udpOff+2 : udpOff+4])
			udpLen := int(binary.BigEndian.Uint16(buf[udpOff+4 : udpOff+6]))
			if udpLen < 8 {
				continue
			}
			payStart := udpOff + 8
			payEnd := udpOff + udpLen
			if payEnd > n {
				payEnd = n
			}
			if payStart >= payEnd {
				continue
			}
			payload := buf[payStart:payEnd]

			if srcPort == 67 || srcPort == 68 || dstPort == 67 || dstPort == 68 {
				s.parseDHCPPayload(payload)
				continue
			}

			if srcPort == 5353 || dstPort == 5353 || srcPort == 5355 || dstPort == 5355 {
				srcMAC := buf[6:12]
				srcIP := net.IPv4(buf[26], buf[27], buf[28], buf[29])
				if !isZeroOrBcastMAC(srcMAC) && isValidClientIP(srcIP) {
					macStr := formatMAC(srcMAC)
					ipStr := srcIP.String()
					s.recordMACIP(macStr, ipStr)
					if h := s.parseMDNSPacket(payload); h != "" {
						// If the mDNS/LLMNR response carried a hostname matching srcIP, also pin it to srcMAC
						s.hostMu.RLock()
						if mh := s.ipToHost[ipStr]; mh != "" {
							s.hostMu.RUnlock()
							s.recordMACHost(macStr, mh)
						} else {
							s.hostMu.RUnlock()
						}
					}
				} else {
					s.parseMDNSPacket(payload)
				}
			}
		}
	}
}
