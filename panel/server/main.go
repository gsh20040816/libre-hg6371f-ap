package main

import (
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"mime"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

const (
	defaultListen    = "0.0.0.0:80"
	defaultTLSListen = "0.0.0.0:443"
	defaultBase      = "/opt/upt/apps/ap-panel"
	defaultExecPath  = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/opt/bcm/bin"
)

var (
	reMAC = regexp.MustCompile(`^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$`)
	allowedServices = map[string]bool{
		"hostapd.wl0": true,
		"hostapd.wl1": true,
		"chronyd":     true,
		"haveged":     true,
		"sshd":        true,
		"ap-led":      true,
	}
)

type Server struct {
	baseDir     string
	webDir      string
	passFile    string
	notesFile   string
	configFile  string
	tlsListen   string
	tlsCertFile string
	tlsKeyFile  string

	mux *http.ServeMux

	tlsMu     sync.Mutex
	tlsActive bool

	cfgMu          sync.RWMutex
	cfg            PanelConfig
	schedForcedOff bool

	passMu    sync.RWMutex
	passMtime time.Time
	passHash  string

	notesMu     sync.RWMutex
	clientNotes map[string]string

	arpMu   sync.RWMutex
	macToIP map[string]string

	hostMu         sync.RWMutex
	macToHost      map[string]string
	ipToHost       map[string]string
	hostQueried    map[string]time.Time
	hostCacheDirty bool

	cpuMu   sync.Mutex
	prevTot uint64
	prevIdl uint64
	lastCPU float64

	trafMu   sync.Mutex
	trafAt   time.Time
	trafPrev map[string][2]uint64
	trafHist []TrafficSample

	scanMu sync.Mutex
	otaMu  sync.Mutex

	ovMu       sync.Mutex
	ovCache    map[string]any
	ovCacheAt  time.Time
	kernelVer  string
	slotMu     sync.Mutex
	slotCache  SlotStatus
	slotCached bool

	ledMu    sync.Mutex
	ledReady bool
	ledColor string

	rfMu          sync.Mutex
	rfSamples     []RFSample
	rfEvents      []RFEvent
	rfClients     map[string]trackedClient
	rfRecentLeave map[string]recentLeave
	rfPrevValid   bool
	rfPrevF5      uint64
	rfPrevR5      uint64
	rfPrevF2      uint64
	rfPrevR2      uint64

	radar       *radarEngine
	stamon      *staMonEngine
	mqtt        *mqttEngine
	wtMu        sync.Mutex
	wtSpots     []WalkTestSpot
	startupDone chan struct{}

	staticMu    sync.RWMutex
	staticCache map[string]*staticFile
}

type staticFile struct {
	mtime time.Time
	raw   []byte
	gz    []byte
	etag  string
	ctype string
}

type reloadableCert struct {
	certPath string
	keyPath  string
	mu       sync.RWMutex
	cert     *tls.Certificate
	mtime    time.Time
}

func (rc *reloadableCert) get(*tls.ClientHelloInfo) (*tls.Certificate, error) {
	fi, err := os.Stat(rc.certPath)
	if err == nil {
		rc.mu.RLock()
		ok := rc.cert != nil && fi.ModTime().Equal(rc.mtime)
		c := rc.cert
		rc.mu.RUnlock()
		if ok {
			return c, nil
		}
	}
	pair, err := tls.LoadX509KeyPair(rc.certPath, rc.keyPath)
	if err != nil {
		rc.mu.RLock()
		c := rc.cert
		rc.mu.RUnlock()
		if c != nil {
			return c, nil
		}
		return nil, err
	}
	rc.mu.Lock()
	rc.cert = &pair
	if fi != nil {
		rc.mtime = fi.ModTime()
	}
	rc.mu.Unlock()
	return &pair, nil
}

func newServer(baseDir, tlsListen, certFile, keyFile string, tlsRedirectFlag bool) *Server {
	s := &Server{
		baseDir:     baseDir,
		webDir:      filepath.Join(baseDir, "web"),
		passFile:    filepath.Join(baseDir, "panel.pass"),
		notesFile:   filepath.Join(baseDir, "clients.json"),
		configFile:  filepath.Join(baseDir, "config.json"),
		tlsListen:   tlsListen,
		tlsCertFile: certFile,
		tlsKeyFile:  keyFile,
		clientNotes: make(map[string]string),
		macToIP:     make(map[string]string),
		macToHost:   make(map[string]string),
		ipToHost:    make(map[string]string),
		hostQueried: make(map[string]time.Time),
		radar:       newRadarEngine(),
		stamon:      newStaMonEngine(),
		mqtt:        newMQTTEngine(),
		startupDone: make(chan struct{}),
		staticCache: make(map[string]*staticFile),
	}
	s.loadConfig()
	if tlsRedirectFlag {
		_ = s.updateConfig(func(c *PanelConfig) { c.TLSRedirect = true })
	}
	s.loadNotes()
	s.loadHostCache()
	s.loadWalkTestSpots()
	s.loadVolatileState()
	s.kernelVer = strings.TrimSpace(runCmdOut(2*time.Second, "uname", "-r"))
	s.refreshARPTable()
	s.sampleCPU()
	s.sampleTraffic()
	s.syncStatusLED()
	// Hardware RF & PHY tuning are owned by OpenRC + /usr/local/sbin/bcm-rf;
	// the panel performs zero hardware initialization at startup.
	close(s.startupDone)
	go s.backgroundSampler()
	go s.watchHardwareButton()
	go s.startARPSnooper()
	go s.startHostDiscoverer()
	go s.startRFRecorder()
	go s.startCSIRadar()
	go s.startStaMonitor()
	go s.startMQTT()
	return s
}

func (s *Server) backgroundSampler() {
	tk := time.NewTicker(2 * time.Second)
	defer tk.Stop()
	ticks := 0
	for range tk.C {
		s.sampleCPU()
		s.sampleTraffic()
		s.syncStatusLED()
		ticks++
		if ticks%15 == 0 {
			s.refreshARPTable()
			s.syncWifiSchedule()
		}
		if ticks%30 == 0 {
			s.saveVolatileState()
		}
	}
}

func (s *Server) isTLSActive() bool {
	s.tlsMu.Lock()
	defer s.tlsMu.Unlock()
	return s.tlsActive
}

func (s *Server) ensureHTTPSListener() {
	s.tlsMu.Lock()
	if s.tlsActive || s.mux == nil {
		s.tlsMu.Unlock()
		return
	}
	if _, err := os.Stat(s.tlsCertFile); err != nil {
		s.tlsMu.Unlock()
		return
	}
	if _, err := os.Stat(s.tlsKeyFile); err != nil {
		s.tlsMu.Unlock()
		return
	}
	rc := &reloadableCert{certPath: s.tlsCertFile, keyPath: s.tlsKeyFile}
	if _, err := rc.get(nil); err != nil {
		s.tlsMu.Unlock()
		log.Printf("TLS cert load failed (%v), continuing HTTP only", err)
		return
	}
	s.tlsActive = true
	s.tlsMu.Unlock()

	tlsSrv := &http.Server{
		Addr:              s.tlsListen,
		Handler:           s.mux,
		ReadHeaderTimeout: 10 * time.Second,
		TLSConfig: &tls.Config{
			MinVersion:     tls.VersionTLS12,
			GetCertificate: rc.get,
		},
	}
	go func() {
		log.Printf("ap-panel-srv HTTPS listening on https://%s (cert=%s)", s.tlsListen, s.tlsCertFile)
		if err := tlsSrv.ListenAndServeTLS("", ""); err != nil {
			log.Printf("HTTPS server error: %v", err)
			s.tlsMu.Lock()
			s.tlsActive = false
			s.tlsMu.Unlock()
		}
	}()
}

func (s *Server) getPassHash() string {
	fi, err := os.Stat(s.passFile)
	if err != nil {
		return ""
	}
	s.passMu.RLock()
	if fi.ModTime().Equal(s.passMtime) {
		h := s.passHash
		s.passMu.RUnlock()
		return h
	}
	s.passMu.RUnlock()

	b, err := os.ReadFile(s.passFile)
	if err != nil {
		return ""
	}
	h := strings.TrimSpace(string(b))
	s.passMu.Lock()
	s.passMtime = fi.ModTime()
	s.passHash = h
	s.passMu.Unlock()
	return h
}

func (s *Server) checkAuth(r *http.Request) bool {
	want := s.getPassHash()
	if want == "" {
		return true
	}
	got := r.Header.Get("X-Token")
	if got == "" {
		if auth := r.Header.Get("Authorization"); strings.HasPrefix(auth, "Bearer ") {
			got = strings.TrimSpace(strings.TrimPrefix(auth, "Bearer "))
		}
	}
	if got == "" {
		got = r.URL.Query().Get("t")
	}
	return len(got) == len(want) && subtle.ConstantTimeCompare([]byte(got), []byte(want)) == 1
}

func setSecurityHeaders(w http.ResponseWriter, isHTML bool) {
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("X-Frame-Options", "DENY")
	w.Header().Set("Referrer-Policy", "no-referrer")
	if isHTML {
		w.Header().Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'")
	}
}

func writeJSON(w http.ResponseWriter, r *http.Request, code int, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		http.Error(w, `{"error":"marshal failed"}`, http.StatusInternalServerError)
		return
	}
	setSecurityHeaders(w, false)
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Add("Vary", "Accept-Encoding")
	if len(b) > 512 && strings.Contains(r.Header.Get("Accept-Encoding"), "gzip") {
		w.Header().Set("Content-Encoding", "gzip")
		w.WriteHeader(code)
		gz := gzip.NewWriter(w)
		_, _ = gz.Write(b)
		_ = gz.Close()
		return
	}
	w.WriteHeader(code)
	_, _ = w.Write(b)
}

func writeErr(w http.ResponseWriter, r *http.Request, code int, msg string) {
	writeJSON(w, r, code, map[string]string{"error": msg})
}

func (s *Server) authWrap(fn http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !s.checkAuth(r) {
			writeErr(w, r, http.StatusUnauthorized, "未授权，请重新登录")
			return
		}
		fn(w, r)
	}
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Pass string `json:"pass"`
	}
	_ = json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req)
	want := s.getPassHash()
	if want == "" {
		writeJSON(w, r, http.StatusOK, map[string]string{"token": "open"})
		return
	}
	sum := sha256.Sum256([]byte(req.Pass))
	got := hex.EncodeToString(sum[:])
	if subtle.ConstantTimeCompare([]byte(got), []byte(want)) == 1 {
		writeJSON(w, r, http.StatusOK, map[string]string{"token": got})
		return
	}
	writeErr(w, r, http.StatusUnauthorized, "密码错误")
}

func (s *Server) handleOverview(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, r, http.StatusOK, s.collectOverview())
}

func (s *Server) handleTraffic(w http.ResponseWriter, r *http.Request) {
	hist := s.getTrafficHistory()
	var latest TrafficSample
	if len(hist) > 0 {
		latest = hist[len(hist)-1]
	}
	writeJSON(w, r, http.StatusOK, map[string]any{
		"latest":  latest,
		"history": hist,
	})
}

func (s *Server) handleWifi(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, r, http.StatusOK, s.collectWifi())
}

func (s *Server) handleWifiSave(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req WifiSaveReq
	if err := json.NewDecoder(io.LimitReader(r.Body, 16384)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	if err := s.saveWifiConfig(req); err != nil {
		writeErr(w, r, http.StatusBadRequest, err.Error())
		return
	}
	s.invalidateOverviewCache()
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleWifiToggle(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Iface   string `json:"iface"`
		Enabled bool   `json:"enabled"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 2048)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	if err := s.toggleRadio(req.Iface, req.Enabled); err != nil {
		writeErr(w, r, http.StatusBadRequest, err.Error())
		return
	}
	s.invalidateOverviewCache()
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleWifiGuest(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Iface string         `json:"iface"`
		Guest GuestBSSConfig `json:"guest"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 8192)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	if err := s.saveGuestConfig(req.Iface, req.Guest); err != nil {
		writeErr(w, r, http.StatusBadRequest, err.Error())
		return
	}
	s.invalidateOverviewCache()
	s.notifyMQTTPush()
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleWifiKick(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Iface string `json:"iface"`
		MAC   string `json:"mac"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 2048)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	validIf := req.Iface == "wl0" || req.Iface == "wl1" || req.Iface == "wl0.1" || req.Iface == "wl1.1"
	if !validIf || !reMAC.MatchString(req.MAC) {
		writeErr(w, r, http.StatusBadRequest, "无效的接口或 MAC 地址")
		return
	}
	out := runCmd(5*time.Second, "hostapd_cli", "-p", "/run/hostapd", "-i", req.Iface, "deauthenticate", strings.ToLower(req.MAC))
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "output": strings.TrimSpace(out)})
}

func (s *Server) handleWifiNote(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		MAC  string `json:"mac"`
		Note string `json:"note"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 2048)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	mac := strings.ToUpper(strings.TrimSpace(req.MAC))
	if !reMAC.MatchString(mac) {
		writeErr(w, r, http.StatusBadRequest, "无效的 MAC 地址")
		return
	}
	if err := s.setNote(mac, strings.TrimSpace(req.Note)); err != nil {
		writeErr(w, r, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleWifiScan(w http.ResponseWriter, r *http.Request) {
	iface := r.URL.Query().Get("iface")
	if iface != "wl0" && iface != "wl1" {
		iface = "wl1"
	}
	res, err := s.scanWifi(iface)
	if err != nil {
		writeErr(w, r, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, r, http.StatusOK, res)
}

func (s *Server) handleWifiWalkTest(w http.ResponseWriter, r *http.Request) {
	callerIP := r.RemoteAddr
	if host, _, err := net.SplitHostPort(r.RemoteAddr); err == nil {
		callerIP = host
	}
	reqMAC := strings.TrimSpace(r.URL.Query().Get("mac"))
	writeJSON(w, r, http.StatusOK, s.sampleWalkTest(callerIP, reqMAC))
}

func (s *Server) handleWifiWalkTestSpots(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, r, http.StatusOK, map[string]any{"spots": s.getWalkTestSpots()})
	case http.MethodPost:
		var req WalkTestSpot
		if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
			writeErr(w, r, http.StatusBadRequest, "请求格式错误")
			return
		}
		spots, err := s.addWalkTestSpot(req)
		if err != nil {
			writeErr(w, r, http.StatusInternalServerError, err.Error())
			return
		}
		writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "spots": spots})
	case http.MethodDelete:
		idStr := strings.TrimSpace(r.URL.Query().Get("id"))
		var id int64
		if idStr != "" {
			v, err := strconv.ParseInt(idStr, 10, 64)
			if err != nil {
				writeErr(w, r, http.StatusBadRequest, "无效的点位 ID")
				return
			}
			id = v
		}
		spots, err := s.deleteWalkTestSpot(id)
		if err != nil {
			writeErr(w, r, http.StatusInternalServerError, err.Error())
			return
		}
		writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "spots": spots})
	default:
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
	}
}

func (s *Server) handleTune(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, r, http.StatusOK, s.collectTune())
}

func (s *Server) handleTuneApply(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req TuneApplyReq
	_ = json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req)
	if err := s.applyTune(req); err != nil {
		writeErr(w, r, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "tune": s.collectTune()})
}

func (s *Server) handlePing(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("ok"))
}

func (s *Server) handleTuneSnapshot(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, r, http.StatusOK, s.collectRFSnapshot())
}

func (s *Server) handleRFHistory(w http.ResponseWriter, r *http.Request) {
	rng := r.URL.Query().Get("range")
	if rng != "1h" && rng != "24h" {
		rng = "6h"
	}
	writeJSON(w, r, http.StatusOK, s.getRFHistory(rng))
}

func (s *Server) handleRFSample(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	smp := s.captureRFSample()
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "sample": smp})
}

func (s *Server) handleRFExport(w http.ResponseWriter, r *http.Request) {
	data := s.exportRFCSV()
	fn := fmt.Sprintf("hg6371f-rf-%s.csv", time.Now().Format("20060102-150405"))
	w.Header().Set("Content-Type", "text/csv; charset=utf-8")
	w.Header().Set("Content-Disposition", `attachment; filename="`+fn+`"`)
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(data)
}

func (s *Server) handleTuneAnalyze(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	res, err := s.analyzeAutoOptimize()
	if err != nil {
		writeErr(w, r, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, r, http.StatusOK, res)
}

func (s *Server) handleTuneAutoApply(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req AutoApplyReq
	_ = json.NewDecoder(io.LimitReader(r.Body, 8192)).Decode(&req)
	res, err := s.applyAutoOptimize(req)
	if err != nil {
		writeErr(w, r, http.StatusBadRequest, err.Error())
		return
	}
	s.invalidateOverviewCache()
	writeJSON(w, r, http.StatusOK, res)
}

func (s *Server) handleNetwork(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost {
		var req NetworkConfig
		if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
			writeErr(w, r, http.StatusBadRequest, "请求格式错误")
			return
		}
		if err := s.saveNetworkConfig(req); err != nil {
			writeErr(w, r, http.StatusBadRequest, err.Error())
			return
		}
		writeJSON(w, r, http.StatusOK, map[string]any{"ok": true})
		return
	}
	writeJSON(w, r, http.StatusOK, s.collectNetwork())
}

func (s *Server) handleDiagPing(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Target string `json:"target"`
		Count  int    `json:"count"`
	}
	_ = json.NewDecoder(io.LimitReader(r.Body, 2048)).Decode(&req)
	out, err := s.runPing(req.Target, req.Count)
	if err != nil {
		writeErr(w, r, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "output": out})
}

func (s *Server) handleSystem(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, r, http.StatusOK, s.collectSystem())
}

func (s *Server) handleSystemPrefs(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		LED          *LEDConfig          `json:"led"`
		Button       *ButtonConfig       `json:"button"`
		WifiSchedule *WifiScheduleConfig `json:"wifi_schedule"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	if req.LED != nil {
		if err := validateLEDConfig(*req.LED); err != nil {
			writeErr(w, r, http.StatusBadRequest, err.Error())
			return
		}
	}
	if req.WifiSchedule != nil && req.WifiSchedule.Enabled {
		if _, ok := parseHHMM(req.WifiSchedule.OffStart); !ok {
			writeErr(w, r, http.StatusBadRequest, "定时关闭开始时间格式应为 HH:MM")
			return
		}
		if _, ok := parseHHMM(req.WifiSchedule.OffEnd); !ok {
			writeErr(w, r, http.StatusBadRequest, "定时关闭结束时间格式应为 HH:MM")
			return
		}
	}
	if err := s.updateConfig(func(c *PanelConfig) {
		if req.LED != nil {
			next := *req.LED
			if next.NightStart == "" {
				next.NightStart = c.LED.NightStart
			}
			if next.NightEnd == "" {
				next.NightEnd = c.LED.NightEnd
			}
			c.LED = next
		}
		if req.Button != nil {
			c.Button = *req.Button
		}
		if req.WifiSchedule != nil {
			c.WifiSchedule = *req.WifiSchedule
		}
	}); err != nil {
		writeErr(w, r, http.StatusInternalServerError, err.Error())
		return
	}
	s.syncStatusLED()
	s.syncWifiSchedule()
	s.invalidateOverviewCache()
	s.notifyMQTTPush()
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "led_state": s.getLEDState()})
}

func (s *Server) handleSystemTLS(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		CertPEM  string `json:"cert_pem"`
		KeyPEM   string `json:"key_pem"`
		Redirect bool   `json:"redirect"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 256*1024)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	if err := s.saveTLSConfig(req.CertPEM, req.KeyPEM, req.Redirect); err != nil {
		writeErr(w, r, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, r, http.StatusOK, map[string]any{
		"ok":           true,
		"tls_enabled":  s.isTLSActive(),
		"tls_redirect": s.getConfig().TLSRedirect,
	})
}

func (s *Server) handleSystemBackup(w http.ResponseWriter, r *http.Request) {
	data, err := s.buildBackupTarGz()
	if err != nil {
		writeErr(w, r, http.StatusInternalServerError, err.Error())
		return
	}
	fn := fmt.Sprintf("hg6371f-backup-%s.tar.gz", time.Now().Format("20060102-150405"))
	w.Header().Set("Content-Type", "application/gzip")
	w.Header().Set("Content-Disposition", `attachment; filename="`+fn+`"`)
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(data)
}

func (s *Server) handleSystemRestore(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	n, err := s.restoreBackupTarGz(io.LimitReader(r.Body, 2*1024*1024))
	if err != nil {
		writeErr(w, r, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, r, http.StatusOK, map[string]any{
		"ok":       true,
		"restored": n,
		"message":  fmt.Sprintf("已恢复 %d 个配置文件，建议重启服务或重启 AP 生效", n),
	})
}

func (s *Server) handleSystemOTA(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	res, err := s.flashOTA(r.Body)
	if err != nil {
		writeErr(w, r, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, r, http.StatusOK, res)
}

func (s *Server) handleSystemService(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Name   string `json:"name"`
		Action string `json:"action"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 2048)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	if !allowedServices[req.Name] || (req.Action != "restart" && req.Action != "reload") {
		writeErr(w, r, http.StatusBadRequest, "不支持的服务或动作")
		return
	}
	if req.Name == "hostapd.wl0" {
		s.suspendRadar("wl0")
		defer s.resumeRadar("wl0")
	} else if req.Name == "hostapd.wl1" {
		s.suspendRadar("wl1")
		defer s.resumeRadar("wl1")
	}
	out := runCmd(15*time.Second, "rc-service", req.Name, req.Action)
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "output": strings.TrimSpace(out)})
}

func (s *Server) handleSystemSlot(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Slot int    `json:"slot"`
		Mode string `json:"mode"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 2048)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	slots, err := s.switchBootSlot(req.Slot, req.Mode)
	if err != nil {
		writeErr(w, r, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "slots": slots})
}

func (s *Server) handleSystemLogs(w http.ResponseWriter, r *http.Request) {
	src := r.URL.Query().Get("src")
	if src != "dmesg" {
		src = "syslog"
	}
	writeJSON(w, r, http.StatusOK, map[string]any{
		"src":   src,
		"lines": s.readLogs(src, 160),
	})
}

func (s *Server) handlePasswd(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		OldPass string `json:"old_pass"`
		NewPass string `json:"new_pass"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		writeErr(w, r, http.StatusBadRequest, "请求格式错误")
		return
	}
	if len(req.NewPass) < 4 {
		writeErr(w, r, http.StatusBadRequest, "新密码至少 4 位")
		return
	}
	want := s.getPassHash()
	if want != "" {
		sum := sha256.Sum256([]byte(req.OldPass))
		got := hex.EncodeToString(sum[:])
		if subtle.ConstantTimeCompare([]byte(got), []byte(want)) != 1 {
			writeErr(w, r, http.StatusBadRequest, "当前密码不正确")
			return
		}
	}
	nsum := sha256.Sum256([]byte(req.NewPass))
	nhash := hex.EncodeToString(nsum[:])
	if err := os.WriteFile(s.passFile, []byte(nhash+"\n"), 0600); err != nil {
		writeErr(w, r, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true, "token": nhash})
}

func (s *Server) handleReboot(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeErr(w, r, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	writeJSON(w, r, http.StatusOK, map[string]any{"ok": true})
	go func() {
		time.Sleep(1 * time.Second)
		_ = exec.Command("sync").Run()
		_ = exec.Command("reboot").Run()
	}()
}

func (s *Server) handleStatic(w http.ResponseWriter, r *http.Request) {
	p := r.URL.Path
	if p == "/" || p == "" {
		p = "/index.html"
	}
	clean := filepath.Clean(p)
	if strings.Contains(clean, "..") {
		http.NotFound(w, r)
		return
	}
	full := filepath.Join(s.webDir, strings.TrimPrefix(clean, "/"))
	fi, err := os.Stat(full)
	if err != nil || fi.IsDir() {
		http.NotFound(w, r)
		return
	}

	s.staticMu.RLock()
	sf, ok := s.staticCache[full]
	if ok && sf.mtime.Equal(fi.ModTime()) {
		s.staticMu.RUnlock()
	} else {
		s.staticMu.RUnlock()
		raw, rerr := os.ReadFile(full)
		if rerr != nil {
			http.Error(w, "read error", http.StatusInternalServerError)
			return
		}
		var gzBuf bytes.Buffer
		var gzBytes []byte
		if len(raw) > 512 {
			gw := gzip.NewWriter(&gzBuf)
			_, _ = gw.Write(raw)
			_ = gw.Close()
			gzBytes = gzBuf.Bytes()
		}
		sum := sha256.Sum256(raw)
		ctype := mime.TypeByExtension(filepath.Ext(full))
		if ctype == "" {
			ctype = http.DetectContentType(raw)
		}
		sf = &staticFile{
			mtime: fi.ModTime(),
			raw:   raw,
			gz:    gzBytes,
			etag:  `"` + hex.EncodeToString(sum[:8]) + `"`,
			ctype: ctype,
		}
		s.staticMu.Lock()
		s.staticCache[full] = sf
		s.staticMu.Unlock()
	}

	setSecurityHeaders(w, strings.HasPrefix(sf.ctype, "text/html"))
	w.Header().Set("Content-Type", sf.ctype)
	w.Header().Set("ETag", sf.etag)
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Add("Vary", "Accept-Encoding")
	if r.Header.Get("If-None-Match") == sf.etag {
		w.WriteHeader(http.StatusNotModified)
		return
	}
	if len(sf.gz) > 0 && strings.Contains(r.Header.Get("Accept-Encoding"), "gzip") {
		w.Header().Set("Content-Encoding", "gzip")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write(sf.gz)
		return
	}
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(sf.raw)
}

func main() {
	listen := flag.String("listen", defaultListen, "HTTP listen address")
	tlsListen := flag.String("tls-listen", defaultTLSListen, "HTTPS listen address")
	tlsCert := flag.String("tls-cert", "/opt/upt/apps/ap-panel/etc/tls.crt", "TLS certificate path")
	tlsKey := flag.String("tls-key", "/opt/upt/apps/ap-panel/etc/tls.key", "TLS private key path")
	tlsRedirect := flag.Bool("tls-redirect", false, "redirect HTTP to HTTPS when TLS is active")
	base := flag.String("base", defaultBase, "panel base directory")
	flag.Parse()

	srv := newServer(*base, *tlsListen, *tlsCert, *tlsKey, *tlsRedirect)

	mux := http.NewServeMux()
	mux.HandleFunc("/api/login", srv.handleLogin)
	mux.HandleFunc("/api/overview", srv.authWrap(srv.handleOverview))
	mux.HandleFunc("/api/traffic", srv.authWrap(srv.handleTraffic))

	mux.HandleFunc("/api/wifi", srv.authWrap(srv.handleWifi))
	mux.HandleFunc("/api/wifi/save", srv.authWrap(srv.handleWifiSave))
	mux.HandleFunc("/api/wifi/toggle", srv.authWrap(srv.handleWifiToggle))
	mux.HandleFunc("/api/wifi/guest", srv.authWrap(srv.handleWifiGuest))
	mux.HandleFunc("/api/wifi/kick", srv.authWrap(srv.handleWifiKick))
	mux.HandleFunc("/api/wifi/note", srv.authWrap(srv.handleWifiNote))
	mux.HandleFunc("/api/wifi/scan", srv.authWrap(srv.handleWifiScan))
	mux.HandleFunc("/api/wifi/walktest", srv.authWrap(srv.handleWifiWalkTest))
	mux.HandleFunc("/api/wifi/walktest/spots", srv.authWrap(srv.handleWifiWalkTestSpots))

	mux.HandleFunc("/api/ping", srv.authWrap(srv.handlePing))
	mux.HandleFunc("/api/tune", srv.authWrap(srv.handleTune))
	mux.HandleFunc("/api/tune/apply", srv.authWrap(srv.handleTuneApply))
	mux.HandleFunc("/api/tune/analyze", srv.authWrap(srv.handleTuneAnalyze))
	mux.HandleFunc("/api/tune/auto-apply", srv.authWrap(srv.handleTuneAutoApply))
	mux.HandleFunc("/api/tune/snapshot", srv.authWrap(srv.handleTuneSnapshot))
	mux.HandleFunc("/api/rf/history", srv.authWrap(srv.handleRFHistory))
	mux.HandleFunc("/api/rf/sample", srv.authWrap(srv.handleRFSample))
	mux.HandleFunc("/api/rf/export", srv.authWrap(srv.handleRFExport))
	mux.HandleFunc("/api/radar", srv.authWrap(srv.handleRadar))
	mux.HandleFunc("/api/radar/config", srv.authWrap(srv.handleRadarConfig))
	mux.HandleFunc("/api/radar/calibrate", srv.authWrap(srv.handleRadarCalibrate))
	mux.HandleFunc("/api/radar/test-webhook", srv.authWrap(srv.handleRadarTestWebhook))
	mux.HandleFunc("/api/stamon", srv.authWrap(srv.handleStaMon))
	mux.HandleFunc("/api/stamon/config", srv.authWrap(srv.handleStaMonConfig))
	mux.HandleFunc("/api/stamon/test-webhook", srv.authWrap(srv.handleStaMonTestWebhook))
	mux.HandleFunc("/api/mqtt", srv.authWrap(srv.handleMQTT))
	mux.HandleFunc("/api/mqtt/config", srv.authWrap(srv.handleMQTTConfig))
	mux.HandleFunc("/api/mqtt/test", srv.authWrap(srv.handleMQTTTest))
	mux.HandleFunc("/api/mqtt/publish", srv.authWrap(srv.handleMQTTPublishNow))
	mux.HandleFunc("/metrics", srv.authWrap(srv.handleMetrics))

	mux.HandleFunc("/api/network", srv.authWrap(srv.handleNetwork))
	mux.HandleFunc("/api/diag/ping", srv.authWrap(srv.handleDiagPing))

	mux.HandleFunc("/api/system", srv.authWrap(srv.handleSystem))
	mux.HandleFunc("/api/system/prefs", srv.authWrap(srv.handleSystemPrefs))
	mux.HandleFunc("/api/system/tls", srv.authWrap(srv.handleSystemTLS))
	mux.HandleFunc("/api/system/backup", srv.authWrap(srv.handleSystemBackup))
	mux.HandleFunc("/api/system/restore", srv.authWrap(srv.handleSystemRestore))
	mux.HandleFunc("/api/system/ota", srv.authWrap(srv.handleSystemOTA))
	mux.HandleFunc("/api/system/service", srv.authWrap(srv.handleSystemService))
	mux.HandleFunc("/api/system/slot", srv.authWrap(srv.handleSystemSlot))
	mux.HandleFunc("/api/system/logs", srv.authWrap(srv.handleSystemLogs))
	mux.HandleFunc("/api/system/passwd", srv.authWrap(srv.handlePasswd))
	mux.HandleFunc("/api/system/reboot", srv.authWrap(srv.handleReboot))
	mux.HandleFunc("/", srv.handleStatic)

	srv.mux = mux
	srv.ensureHTTPSListener()

	httpHandler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if srv.isTLSActive() && srv.getConfig().TLSRedirect {
			target := "https://" + r.Host + r.URL.RequestURI()
			http.Redirect(w, r, target, http.StatusPermanentRedirect)
			return
		}
		mux.ServeHTTP(w, r)
	})

	httpSrv := &http.Server{
		Addr:              *listen,
		Handler:           httpHandler,
		ReadHeaderTimeout: 10 * time.Second,
	}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGTERM, syscall.SIGINT)
	go func() {
		sig := <-sigCh
		log.Printf("ap-panel-srv received %v, disarming CSIMON and saving volatile state", sig)
		srv.shutdownRadar()
		srv.saveVolatileState()
		srv.shutdownMQTT()
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cancel()
		_ = httpSrv.Shutdown(ctx)
	}()

	log.Printf("ap-panel-srv HTTP listening on http://%s (base=%s, tls=%v)", *listen, *base, srv.isTLSActive())
	if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
