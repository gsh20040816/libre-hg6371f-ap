package main

import (
	"fmt"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
	"time"
)

// HG6371F (hwcfg=0xba) front-panel GPIOs:
//   GPIO 10 (OUT_LOS_LED)    = Red   (active-low: 0=ON, 1=OFF)
//   GPIO 11 (OUT_PON_LED)    = Blue  (active-low: 0=ON, 1=OFF)
//   GPIO  9 (IN_LED_BUTTON)  = Front tactile button (active-low: 0=pressed, 1=released)
const (
	gpioLEDRed    = "10"
	gpioLEDBlue   = "11"
	gpioLEDButton = "9"
)

func ensureGPIODir(pin, dir string) {
	dirPath := "/sys/class/gpio/gpio" + pin + "/direction"
	if _, err := os.Stat(dirPath); err != nil {
		_ = os.WriteFile("/sys/class/gpio/export", []byte(pin), 0644)
	}
	_ = os.WriteFile(dirPath, []byte(dir), 0644)
}

func hasBridgeIPv4(ifaceName string) bool {
	ifi, err := net.InterfaceByName(ifaceName)
	if err != nil {
		return false
	}
	addrs, err := ifi.Addrs()
	if err != nil {
		return false
	}
	for _, a := range addrs {
		if ipnet, ok := a.(*net.IPNet); ok {
			if ip4 := ipnet.IP.To4(); ip4 != nil && !ip4.IsLoopback() {
				return true
			}
		}
	}
	return false
}

func parseHHMM(s string) (int, bool) {
	parts := strings.Split(strings.TrimSpace(s), ":")
	if len(parts) != 2 {
		return 0, false
	}
	h, e1 := strconv.Atoi(parts[0])
	m, e2 := strconv.Atoi(parts[1])
	if e1 != nil || e2 != nil || h < 0 || h > 23 || m < 0 || m > 59 {
		return 0, false
	}
	return h*60 + m, true
}

func inTimeWindow(now time.Time, startStr, endStr string) bool {
	st, ok1 := parseHHMM(startStr)
	en, ok2 := parseHHMM(endStr)
	if !ok1 || !ok2 || st == en {
		return false
	}
	cur := now.Hour()*60 + now.Minute()
	if st < en {
		return cur >= st && cur < en
	}
	// Overnight window, e.g. 23:00 -> 07:00
	return cur >= st || cur < en
}

func (s *Server) syncStatusLED() {
	cfg := s.getConfig()
	want := "red"
	switch cfg.LED.Mode {
	case "off":
		want = "off"
	case "schedule":
		if inTimeWindow(time.Now(), cfg.LED.NightStart, cfg.LED.NightEnd) {
			want = "off"
		} else if readTrim("/sys/class/net/lan0/carrier") == "1" && hasBridgeIPv4("br0") {
			want = "blue"
		}
	default: // "auto"
		if readTrim("/sys/class/net/lan0/carrier") == "1" && hasBridgeIPv4("br0") {
			want = "blue"
		}
	}

	s.ledMu.Lock()
	defer s.ledMu.Unlock()
	if !s.ledReady {
		ensureGPIODir(gpioLEDRed, "out")
		ensureGPIODir(gpioLEDBlue, "out")
		s.ledReady = true
	}
	if s.ledColor == want {
		return
	}
	r, b := "1", "1"
	switch want {
	case "blue":
		b = "0"
	case "red":
		r = "0"
	}
	_ = os.WriteFile("/sys/class/gpio/gpio"+gpioLEDRed+"/value", []byte(r), 0644)
	_ = os.WriteFile("/sys/class/gpio/gpio"+gpioLEDBlue+"/value", []byte(b), 0644)
	s.ledColor = want
}

func (s *Server) getLEDState() string {
	s.ledMu.Lock()
	defer s.ledMu.Unlock()
	return s.ledColor
}

// watchHardwareButton polls GPIO 9 (front LED/WPS tactile button, active-low).
// Short press (<1.8s) runs Button.ShortAction; long press (>=1.8s) runs Button.LongAction.
func (s *Server) watchHardwareButton() {
	ensureGPIODir(gpioLEDButton, "in")
	valPath := "/sys/class/gpio/gpio" + gpioLEDButton + "/value"
	if _, err := os.Stat(valPath); err != nil {
		return
	}

	tk := time.NewTicker(150 * time.Millisecond)
	defer tk.Stop()

	var pressedAt time.Time
	longFired := false

	for range tk.C {
		v := readTrim(valPath)
		if v == "0" {
			if pressedAt.IsZero() {
				pressedAt = time.Now()
				longFired = false
			} else if !longFired && time.Since(pressedAt) >= 1800*time.Millisecond {
				longFired = true
				cfg := s.getConfig()
				s.runButtonAction("long", cfg.Button.LongAction)
			}
		} else {
			if !pressedAt.IsZero() {
				if !longFired && time.Since(pressedAt) >= 80*time.Millisecond {
					cfg := s.getConfig()
					s.runButtonAction("short", cfg.Button.ShortAction)
				}
				pressedAt = time.Time{}
				longFired = false
			}
		}
	}
}

func (s *Server) runButtonAction(kind, action string) {
	if action == "" || action == "none" {
		return
	}
	log.Printf("hardware button (%s press) -> %s", kind, action)
	switch action {
	case "toggle_led":
		_ = s.updateConfig(func(c *PanelConfig) {
			if c.LED.Mode == "off" {
				c.LED.Mode = "auto"
			} else {
				c.LED.Mode = "off"
			}
		})
		s.syncStatusLED()
	case "toggle_guest":
		cfg := s.getConfig()
		rc := cfg.Radios["wl1"]
		if rc.Guest.SSID == "" {
			rc.Guest.SSID = "OpenWrt-Guest"
		}
		if len(rc.Guest.Passphrase) < 8 {
			// Fall back to wl0 guest if wl1 guest has no passphrase configured
			rc0 := cfg.Radios["wl0"]
			if len(rc0.Guest.Passphrase) >= 8 {
				rc0.Guest.Enabled = !rc0.Guest.Enabled
				if err := s.saveGuestConfig("wl0", rc0.Guest); err != nil {
					log.Printf("button toggle_guest wl0 failed: %v", err)
				}
			}
			return
		}
		rc.Guest.Enabled = !rc.Guest.Enabled
		if err := s.saveGuestConfig("wl1", rc.Guest); err != nil {
			log.Printf("button toggle_guest wl1 failed: %v", err)
		}
	case "restart_wifi":
		s.suspendRadar("wl1")
		s.suspendRadar("wl0")
		defer s.resumeRadar("wl0")
		defer s.resumeRadar("wl1")
		_ = runCmd(15*time.Second, "rc-service", "hostapd.wl1", "restart")
		_ = runCmd(15*time.Second, "rc-service", "hostapd.wl0", "restart")
	}
}

// syncWifiSchedule checks the optional Wi-Fi night-off schedule every minute.
func (s *Server) syncWifiSchedule() {
	cfg := s.getConfig()
	if !cfg.WifiSchedule.Enabled {
		if s.schedForcedOff {
			s.schedForcedOff = false
			for _, iface := range []string{"wl0", "wl1"} {
				if cfg.Radios[iface].Enabled {
					_ = s.applyRadioPower(iface, true)
				}
			}
		}
		return
	}
	shouldOff := inTimeWindow(time.Now(), cfg.WifiSchedule.OffStart, cfg.WifiSchedule.OffEnd)
	if shouldOff == s.schedForcedOff {
		return
	}
	s.schedForcedOff = shouldOff
	targets := []string{}
	switch cfg.WifiSchedule.Band {
	case "wl0":
		targets = []string{"wl0"}
	case "wl1":
		targets = []string{"wl1"}
	default:
		targets = []string{"wl0", "wl1"}
	}
	for _, iface := range targets {
		if shouldOff {
			log.Printf("wifi schedule: turning %s OFF", iface)
			_ = s.applyRadioPower(iface, false)
		} else if cfg.Radios[iface].Enabled {
			log.Printf("wifi schedule: turning %s ON", iface)
			_ = s.applyRadioPower(iface, true)
		}
	}
}

func validateLEDConfig(req LEDConfig) error {
	switch req.Mode {
	case "auto", "off", "schedule":
	default:
		return fmt.Errorf("不支持的指示灯模式: %s", req.Mode)
	}
	if req.Mode == "schedule" {
		if _, ok := parseHHMM(req.NightStart); !ok {
			return fmt.Errorf("熄灯开始时间格式应为 HH:MM")
		}
		if _, ok := parseHHMM(req.NightEnd); !ok {
			return fmt.Errorf("熄灯结束时间格式应为 HH:MM")
		}
	}
	return nil
}
