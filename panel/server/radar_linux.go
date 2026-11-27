//go:build linux

package main

import (
	"fmt"
	"log"
	"os"
	"syscall"
	"time"
)

func (s *Server) startCSIRadar() {
	// Wait only for startup RF parameter application to finish (zero arbitrary boot sleep).
	<-s.startupDone

	wasWaiting := false
	for {
		cfg := s.getConfig().Radar
		if !cfg.Enabled || !reMAC.MatchString(cfg.TargetMAC) {
			wasWaiting = false
			s.radar.mu.Lock()
			prevIf, prevMac := s.radar.activeIface, s.radar.activeMAC
			s.radar.activeIface = ""
			s.radar.activeMAC = ""
			s.radar.state = "empty"
			s.radar.occupied = false
			s.radar.lastEmptyAt = time.Time{}
			s.radar.faultMsg = ""
			s.radar.mu.Unlock()
			if prevIf != "" {
				s.disarmCSIMON(prevIf, prevMac)
			}
			select {
			case <-s.radar.reloadCh:
			case <-time.After(10 * time.Second):
			}
			continue
		}

		// Never arm CSIMON while the interface is suspended for radio reload / power-off / shutdown.
		if s.radar.isSuspended(cfg.Iface) {
			wasWaiting = true
			s.radar.mu.Lock()
			s.radar.state = "waiting"
			wasOcc := s.radar.occupied
			wh := s.radar.expireOccupiedIfDueLocked(cfg, time.Now(), "射频重载暂停")
			expired := wasOcc && !s.radar.occupied
			s.radar.mu.Unlock()
			if expired {
				s.notifyMQTTRadarEvent()
			}
			if wh != nil && cfg.WebhookURL != "" {
				go postRadarWebhook(cfg.WebhookURL, wh)
			}
			select {
			case <-s.radar.reloadCh:
			case <-time.After(2 * time.Second):
			}
			continue
		}

		proto := s.detectCSIMONProto(cfg.Iface)
		if proto <= 0 {
			log.Printf("csi-radar: %s CSIMON Netlink unavailable (proto=%d), retrying in 30s", cfg.Iface, proto)
			select {
			case <-s.radar.reloadCh:
			case <-time.After(30 * time.Second):
			}
			continue
		}

		// CRITICAL: Never arm Broadcom CSIMON for an unassociated STA — wl.ko can hang
		// if csimon is enabled while target MAC is absent from assoclist.
		if !s.isSTAAssociated(cfg.Iface, cfg.TargetMAC) {
			wasWaiting = true
			s.radar.mu.Lock()
			prevIf, prevMac := s.radar.activeIface, s.radar.activeMAC
			s.radar.activeIface = ""
			s.radar.activeMAC = ""
			s.radar.state = "waiting"
			wasOcc := s.radar.occupied
			wh := s.radar.expireOccupiedIfDueLocked(cfg, time.Now(), "目标终端离线")
			expired := wasOcc && !s.radar.occupied
			s.radar.mu.Unlock()
			if expired {
				s.notifyMQTTRadarEvent()
			}
			if wh != nil && cfg.WebhookURL != "" {
				go postRadarWebhook(cfg.WebhookURL, wh)
			}
			if prevIf != "" {
				s.disarmCSIMON(prevIf, prevMac)
			}
			select {
			case <-s.radar.reloadCh:
			case <-time.After(5 * time.Second):
			}
			continue
		}

		// Brief settle after a fresh association or radio reload so WPA 4-way handshake completes cleanly.
		if wasWaiting {
			wasWaiting = false
			time.Sleep(2 * time.Second)
			if s.radar.isSuspended(cfg.Iface) || !s.isSTAAssociated(cfg.Iface, cfg.TargetMAC) {
				wasWaiting = true
				continue
			}
		}

		if dmaFault := s.runRadarSession(cfg, proto); dmaFault {
			// Back off instead of repeatedly hammering armCSIMON on a wedged M2M DMA core.
			select {
			case <-s.radar.reloadCh:
			case <-time.After(60 * time.Second):
			}
		}
	}
}

func (s *Server) runRadarSession(cfg RadarConfig, proto int) bool {
	fd, err := syscall.Socket(syscall.AF_NETLINK, syscall.SOCK_RAW, proto)
	if err != nil {
		log.Printf("csi-radar: netlink socket proto=%d failed: %v", proto, err)
		time.Sleep(10 * time.Second)
		return false
	}
	defer syscall.Close(fd)

	_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_RCVBUF, 512*1024)
	sa := &syscall.SockaddrNetlink{
		Family: syscall.AF_NETLINK,
		Pid:    uint32(os.Getpid()) ^ (uint32(proto) << 20),
		Groups: 0xff,
	}
	if err := syscall.Bind(fd, sa); err != nil {
		log.Printf("csi-radar: bind proto=%d failed: %v", proto, err)
		time.Sleep(10 * time.Second)
		return false
	}
	tv := syscall.Timeval{Sec: 1, Usec: 0}
	_ = syscall.SetsockoptTimeval(fd, syscall.SOL_SOCKET, syscall.SO_RCVTIMEO, &tv)

	doneCh := make(chan struct{})
	s.radar.mu.Lock()
	if s.radar.suspendCount[cfg.Iface] > 0 {
		s.radar.mu.Unlock()
		return false
	}
	prevIf, prevMac := s.radar.activeIface, s.radar.activeMAC
	s.radar.activeIface = cfg.Iface
	s.radar.activeMAC = cfg.TargetMAC
	s.radar.sessionDone = doneCh
	s.radar.netlinkProto = proto
	s.radar.ringLen = 0
	s.radar.ringIdx = 0
	s.radar.evMotion = 0
	s.radar.evMicro = 0
	s.radar.lastEvalAt = time.Time{}
	s.radar.state = "empty"
	s.radar.faultMsg = ""
	s.radar.mu.Unlock()

	defer func() {
		s.radar.mu.Lock()
		if s.radar.sessionDone == doneCh {
			s.radar.sessionDone = nil
		}
		if s.radar.activeIface == cfg.Iface {
			s.radar.activeIface = ""
			s.radar.activeMAC = ""
		}
		s.radar.mu.Unlock()
		close(doneCh)
	}()

	if prevIf != "" && (prevIf != cfg.Iface || prevMac != cfg.TargetMAC) {
		s.disarmCSIMON(prevIf, prevMac)
	}
	s.armCSIMON(cfg.Iface, cfg.TargetMAC, cfg.IntervalMs)
	defer s.disarmCSIMON(cfg.Iface, cfg.TargetMAC)
	log.Printf("csi-radar: armed %s target=%s interval=%dms proto=%d", cfg.Iface, cfg.TargetMAC, cfg.IntervalMs, proto)

	baseStats := s.readCSIMONStats(cfg.Iface)
	buf := make([]byte, 8192)
	idleTicks := 0

	for {
		if s.radar.isSuspended(cfg.Iface) {
			return false
		}
		select {
		case <-s.radar.reloadCh:
			return false
		default:
		}

		n, _, err := syscall.Recvfrom(fd, buf, 0)
		if err != nil {
			if err == syscall.EAGAIN || err == syscall.EWOULDBLOCK || err == syscall.EINTR {
				if s.radar.isSuspended(cfg.Iface) {
					return false
				}
				idleTicks++
				// If no frames for 5s, check if STA disconnected or M2M DMA stalled.
				if idleTicks >= 5 {
					if !s.isSTAAssociated(cfg.Iface, cfg.TargetMAC) {
						log.Printf("csi-radar: target %s left %s, disarming", cfg.TargetMAC, cfg.Iface)
						return false
					}
					curStats := s.readCSIMONStats(cfg.Iface)
					var dDDR, dFail uint64
					if curStats.ddrCnt >= baseStats.ddrCnt {
						dDDR = curStats.ddrCnt - baseStats.ddrCnt
					}
					if curStats.ddrFailCnt >= baseStats.ddrFailCnt {
						dFail = curStats.ddrFailCnt - baseStats.ddrFailCnt
					}
					if dDDR == 0 && dFail >= 15 {
						msg := fmt.Sprintf("%s 硬件 M2M DMA 搬运失败（+%d 帧未写入 DDR），已自动撤防保护；请重启 AP 或切换另一频段", cfg.Iface, dFail)
						log.Printf("csi-radar: %s M2M DMA stall detected (xfer_to_ddr=0, fail=+%d), disarming", cfg.Iface, dFail)
						s.radar.mu.Lock()
						s.radar.state = "dma_fault"
						s.radar.occupied = false
						s.radar.lastEmptyAt = time.Time{}
						s.radar.faultMsg = msg
						s.radar.mu.Unlock()
						return true
					}
					if idleTicks >= 15 {
						idleTicks = 0
						s.armCSIMON(cfg.Iface, cfg.TargetMAC, cfg.IntervalMs)
						baseStats = s.readCSIMONStats(cfg.Iface)
					}
				}
				continue
			}
			log.Printf("csi-radar: recv error: %v", err)
			time.Sleep(2 * time.Second)
			return false
		}
		idleTicks = 0
		if n > 16 {
			s.ingestCSIPayload(buf[16:n])
		}
	}
}
