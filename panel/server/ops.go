package main

import (
	"archive/tar"
	"bufio"
	"bytes"
	"compress/gzip"
	"crypto/tls"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

type NetworkConfig struct {
	Mode     string `json:"mode"` // "dhcp" | "static"
	IP       string `json:"ip"`   // e.g. "192.168.1.2/24"
	Gateway  string `json:"gateway"`
	DNS      string `json:"dns"`
	Hostname string `json:"hostname"`
	CurIP    string `json:"cur_ip"`
	CurGW    string `json:"cur_gw"`
	BrMAC    string `json:"br_mac"`
}

type FDBEntry struct {
	MAC   string `json:"mac"`
	Port  string `json:"port"`
	State string `json:"state"`
	IP    string `json:"ip"`
	Note  string `json:"note"`
}

var (
	reHostname = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9-]{0,31}$`)
	rePingHost = regexp.MustCompile(`^[a-zA-Z0-9._:-]{1,64}$`)
)

func (s *Server) collectNetwork() map[string]any {
	nc := NetworkConfig{
		Mode:     "dhcp",
		Hostname: readTrim("/etc/hostname"),
		BrMAC:    strings.ToUpper(readTrim("/sys/class/net/br0/address")),
	}
	ipOut := runCmdOut(2*time.Second, "ip", "-4", "addr", "show", "br0")
	if m := reIPv4.FindStringSubmatch(ipOut); len(m) == 2 {
		nc.CurIP = m[1]
	}
	rt := runCmdOut(2*time.Second, "ip", "-4", "route", "show", "default")
	for _, f := range strings.Fields(rt) {
		if net.ParseIP(f) != nil {
			nc.CurGW = f
			break
		}
	}
	if b, err := os.ReadFile("/etc/network/interfaces"); err == nil {
		inBr0 := false
		sc := bufio.NewScanner(strings.NewReader(string(b)))
		for sc.Scan() {
			line := strings.TrimSpace(sc.Text())
			if strings.HasPrefix(line, "iface br0 inet ") {
				inBr0 = true
				if strings.Contains(line, "static") {
					nc.Mode = "static"
				} else {
					nc.Mode = "dhcp"
				}
				continue
			}
			if strings.HasPrefix(line, "iface ") || strings.HasPrefix(line, "auto ") {
				inBr0 = false
				continue
			}
			if inBr0 {
				f := strings.Fields(line)
				if len(f) >= 2 {
					switch f[0] {
					case "address":
						nc.IP = f[1]
					case "gateway":
						nc.Gateway = f[1]
					}
				}
			}
		}
	}
	if b, err := os.ReadFile("/etc/resolv.conf"); err == nil {
		sc := bufio.NewScanner(strings.NewReader(string(b)))
		for sc.Scan() {
			f := strings.Fields(sc.Text())
			if len(f) >= 2 && f[0] == "nameserver" {
				nc.DNS = f[1]
				break
			}
		}
	}
	if nc.IP == "" {
		nc.IP = nc.CurIP
	}
	if nc.Gateway == "" {
		nc.Gateway = nc.CurGW
	}

	return map[string]any{
		"config": nc,
		"ports": []EthPortInfo{
			collectEthPort("lan0", "2.5G 上联口"),
			collectEthPort("lan1", "1G 扩展口"),
		},
		"fdb": s.collectFDB(),
	}
}

func (s *Server) collectFDB() []FDBEntry {
	s.refreshARPTable()
	raw := runCmdOut(2*time.Second, "bridge", "fdb", "show", "br", "br0")
	var out []FDBEntry
	for _, line := range strings.Split(raw, "\n") {
		f := strings.Fields(line)
		if len(f) < 3 || !reMAC.MatchString(f[0]) {
			continue
		}
		mac := strings.ToUpper(f[0])
		port := ""
		state := "dynamic"
		for i := 1; i < len(f); i++ {
			if f[i] == "dev" && i+1 < len(f) {
				port = f[i+1]
				i++
			} else if f[i] == "permanent" {
				state = "local"
			}
		}
		if port == "" || state == "local" {
			continue
		}
		out = append(out, FDBEntry{
			MAC:   mac,
			Port:  port,
			State: state,
			IP:    s.lookupIP(mac),
			Note:  s.displayName(mac),
		})
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Port != out[j].Port {
			return out[i].Port < out[j].Port
		}
		return out[i].MAC < out[j].MAC
	})
	return out
}

func (s *Server) saveNetworkConfig(req NetworkConfig) error {
	req.Hostname = strings.TrimSpace(req.Hostname)
	if !reHostname.MatchString(req.Hostname) {
		return errors.New("主机名仅支持字母、数字与短横线（1-32 字符）")
	}
	if req.Mode != "dhcp" && req.Mode != "static" {
		return errors.New("网络模式需为 dhcp 或 static")
	}
	var brBlock string
	if req.Mode == "static" {
		ip, ipNet, err := net.ParseCIDR(strings.TrimSpace(req.IP))
		if err != nil || ip.To4() == nil {
			return errors.New("静态 IP 格式应为 CIDR，例如 192.168.1.2/24")
		}
		gw := net.ParseIP(strings.TrimSpace(req.Gateway))
		if gw == nil || gw.To4() == nil || !ipNet.Contains(gw) {
			return errors.New("网关必须是同网段有效 IPv4 地址")
		}
		brBlock = fmt.Sprintf(`auto br0
iface br0 inet static
	bridge-ports lan0 lan1
	bridge-stp off
	bridge-fd 0
	address %s
	gateway %s
`, strings.TrimSpace(req.IP), strings.TrimSpace(req.Gateway))
		if dns := strings.TrimSpace(req.DNS); dns != "" {
			if net.ParseIP(dns) == nil {
				return errors.New("DNS 必须是有效 IP 地址")
			}
			_ = os.WriteFile("/etc/resolv.conf", []byte("nameserver "+dns+"\n"), 0644)
		}
	} else {
		brBlock = fmt.Sprintf(`auto br0
iface br0 inet dhcp
	bridge-ports lan0 lan1
	bridge-stp off
	bridge-fd 0
	hostname %s
`, req.Hostname)
	}

	content := fmt.Sprintf(`auto lo
iface lo inet loopback

auto lan0
iface lan0 inet manual

auto lan1
iface lan1 inet manual

%s`, brBlock)

	if err := os.WriteFile("/etc/hostname", []byte(req.Hostname+"\n"), 0644); err != nil {
		return err
	}
	_ = runCmd(2*time.Second, "hostname", req.Hostname)

	tmp := "/etc/network/interfaces.tmp"
	if err := os.WriteFile(tmp, []byte(content), 0644); err != nil {
		return err
	}
	if err := os.Rename(tmp, "/etc/network/interfaces"); err != nil {
		return err
	}
	// Apply network config asynchronously after HTTP response flushes
	go func() {
		time.Sleep(800 * time.Millisecond)
		_ = runCmd(15*time.Second, "rc-service", "networking", "restart")
	}()
	return nil
}

func (s *Server) runPing(target string, count int) (string, error) {
	target = strings.TrimSpace(target)
	if target == "" {
		rt := runCmdOut(2*time.Second, "ip", "-4", "route", "show", "default")
		for _, f := range strings.Fields(rt) {
			if net.ParseIP(f) != nil {
				target = f
				break
			}
		}
	}
	if target == "" || !rePingHost.MatchString(target) || strings.HasPrefix(target, "-") {
		return "", errors.New("无效的 Ping 目标地址")
	}
	if count < 1 || count > 10 {
		count = 4
	}
	out := runCmd(15*time.Second, "ping", "-c", strconv.Itoa(count), "-W", "2", target)
	return out, nil
}

func (s *Server) saveTLSConfig(certPEM, keyPEM string, redirect bool) error {
	certPEM = strings.TrimSpace(certPEM)
	keyPEM = strings.TrimSpace(keyPEM)
	if certPEM != "" || keyPEM != "" {
		if certPEM == "" || keyPEM == "" {
			return errors.New("证书和私钥必须同时提供")
		}
		if _, err := tls.X509KeyPair([]byte(certPEM), []byte(keyPEM)); err != nil {
			return fmt.Errorf("证书与私钥校验失败: %v", err)
		}
		if err := os.MkdirAll(filepath.Dir(s.tlsCertFile), 0700); err != nil {
			return err
		}
		if err := os.WriteFile(s.tlsCertFile, []byte(certPEM+"\n"), 0644); err != nil {
			return err
		}
		if err := os.WriteFile(s.tlsKeyFile, []byte(keyPEM+"\n"), 0600); err != nil {
			return err
		}
		s.ensureHTTPSListener()
	}
	if redirect && !s.isTLSActive() {
		return errors.New("尚未启用有效 HTTPS 证书，无法开启强制跳转")
	}
	return s.updateConfig(func(c *PanelConfig) {
		c.TLSRedirect = redirect
	})
}

var backupFiles = []string{
	"/etc/bcm-rf.conf",
	"/etc/hostapd/wl0.conf",
	"/etc/hostapd/wl1.conf",
	"/etc/hostapd/wl0.accept",
	"/etc/hostapd/wl0.deny",
	"/etc/hostapd/wl1.accept",
	"/etc/hostapd/wl1.deny",
	"/etc/network/interfaces",
	"/etc/hostname",
	"/opt/upt/apps/ap-panel/config.json",
	"/opt/upt/apps/ap-panel/clients.json",
}

func (s *Server) buildBackupTarGz() ([]byte, error) {
	var buf bytes.Buffer
	gw := gzip.NewWriter(&buf)
	tw := tar.NewWriter(gw)
	for _, p := range backupFiles {
		data, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		fi, err := os.Stat(p)
		if err != nil {
			continue
		}
		hdr := &tar.Header{
			Name:    strings.TrimPrefix(p, "/"),
			Mode:    int64(fi.Mode().Perm()),
			Size:    int64(len(data)),
			ModTime: fi.ModTime(),
		}
		if err := tw.WriteHeader(hdr); err != nil {
			return nil, err
		}
		if _, err := tw.Write(data); err != nil {
			return nil, err
		}
	}
	if err := tw.Close(); err != nil {
		return nil, err
	}
	if err := gw.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func (s *Server) restoreBackupTarGz(r io.Reader) (int, error) {
	allowed := make(map[string]bool, len(backupFiles))
	for _, p := range backupFiles {
		allowed[strings.TrimPrefix(p, "/")] = true
	}
	gr, err := gzip.NewReader(r)
	if err != nil {
		return 0, fmt.Errorf("无效的 gzip 备份包: %v", err)
	}
	defer gr.Close()

	tr := tar.NewReader(gr)
	restored := 0
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return restored, fmt.Errorf("读取备份包失败: %v", err)
		}
		name := filepath.Clean(strings.TrimPrefix(hdr.Name, "/"))
		if !allowed[name] || hdr.Typeflag != tar.TypeReg {
			continue
		}
		if hdr.Size > 512*1024 {
			return restored, fmt.Errorf("文件过大: %s", name)
		}
		data, err := io.ReadAll(io.LimitReader(tr, 512*1024))
		if err != nil {
			return restored, err
		}
		dst := "/" + name
		_ = os.MkdirAll(filepath.Dir(dst), 0755)
		perm := os.FileMode(0600)
		if strings.HasPrefix(name, "etc/network") || name == "etc/hostname" || name == "etc/bcm-rf.conf" {
			perm = 0644
		}
		if err := os.WriteFile(dst, data, perm); err != nil {
			return restored, err
		}
		restored++
	}
	if restored == 0 {
		return 0, errors.New("备份包中未找到受支持的配置项")
	}
	s.loadConfig()
	s.loadNotes()
	s.syncStatusLED()
	return restored, nil
}

// flashOTA validates a SquashFS XZ rootfs image and flashes it to the inactive A/B UBI volume
// with one-shot boot selection (protected by the 180s deadman rollback guard).
func (s *Server) flashOTA(r io.Reader) (map[string]any, error) {
	const maxRootfsBytes = 44314624 // 349 LEBs on ubi0_4 / ubi0_6

	s.otaMu.Lock()
	defer s.otaMu.Unlock()

	tmpPath := "/tmp/ota-rootfs.sqfs"
	_ = os.Remove(tmpPath)
	defer os.Remove(tmpPath)

	f, err := os.OpenFile(tmpPath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0600)
	if err != nil {
		return nil, err
	}
	n, err := io.Copy(f, io.LimitReader(r, maxRootfsBytes+1))
	_ = f.Close()
	if err != nil {
		return nil, fmt.Errorf("接收镜像失败: %v", err)
	}
	if n < 64*1024 {
		return nil, errors.New("文件过小，不是有效的 SquashFS 根镜像")
	}
	if n > maxRootfsBytes {
		return nil, fmt.Errorf("镜像超过分区上限 (%d > %d 字节)", n, maxRootfsBytes)
	}

	// Verify SquashFS superblock: magic "hsqs" (LE) and compression ID == 4 (XZ)
	hdr := make([]byte, 24)
	hf, err := os.Open(tmpPath)
	if err != nil {
		return nil, err
	}
	_, err = io.ReadFull(hf, hdr)
	_ = hf.Close()
	if err != nil || string(hdr[0:4]) != "hsqs" {
		return nil, errors.New("魔数校验失败：文件不是 SquashFS 镜像")
	}
	compID := binary.LittleEndian.Uint16(hdr[20:22])
	if compID != 4 {
		return nil, fmt.Errorf("压缩格式不兼容（comp=%d，本机内核仅支持 XZ=4）", compID)
	}

	bs := parseBootstate(runCmd(3*time.Second, "bcm_bootstate"))
	if bs.Booted != 1 && bs.Booted != 2 {
		return nil, errors.New("无法识别当前运行槽位")
	}
	targetSlot := 2
	targetVol := "/dev/ubi0_6"
	bootCode := "8" // one-shot slot 2
	if bs.Booted == 2 {
		targetSlot = 1
		targetVol = "/dev/ubi0_4"
		bootCode = "6" // one-shot slot 1
	}

	// Unlock mtd2 if stock board driver is present (no-op on Alpine)
	if _, err := os.Stat("/dev/fhdrv_kdrv_board"); err == nil {
		wp := filepath.Join(s.baseDir, "bin", "fh-mtd-wp")
		_ = runCmd(5*time.Second, wp, "2", "0")
		defer func() { _ = runCmd(5*time.Second, wp, "2", "1") }()
	}

	_ = runCmd(5*time.Second, "ubiblock", "-r", targetVol)
	out := runCmd(90*time.Second, "ubiupdatevol", targetVol, tmpPath)
	if strings.Contains(strings.ToLower(out), "error") || strings.Contains(strings.ToLower(out), "fail") {
		return nil, fmt.Errorf("刷写 %s 失败: %s", targetVol, strings.TrimSpace(out))
	}

	// Remove commit sentinel so 180s rollback guard arms on new slot boot
	_ = os.Remove("/fhconf/ap_alpine_committed")
	_ = runCmd(10*time.Second, "bcm_bootstate", bootCode)
	bsAfter := s.getBootstateCached(true)
	s.invalidateOverviewCache()
	if bsAfter.Reboot != targetSlot {
		return nil, fmt.Errorf("写入成功但切槽失败（当前 Reboot=%d，目标=%d）", bsAfter.Reboot, targetSlot)
	}

	return map[string]any{
		"ok":          true,
		"size_bytes":  n,
		"target_slot": targetSlot,
		"target_vol":  targetVol,
		"slots":       bsAfter,
		"message":     fmt.Sprintf("已刷入 Slot %d（%s），下次启动已设为一次性试跑并启用 180 秒自动回滚保护，请点击重启。", targetSlot, targetVol),
	}, nil
}
