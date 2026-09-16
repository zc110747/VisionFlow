// Package mediamtx manages the MediaMTX subprocess: it launches MediaMTX with a
// generated config, watches its health, polls the control API for path discovery,
// and proxies WebRTC signaling offers to MediaMTX's WebRTC endpoint.
package mediamtx

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"time"

	"video-server/internal/config"
	"video-server/internal/logger"
	"video-server/internal/netiface"
)

// Path mirrors the relevant fields of a MediaMTX v3 API path object.
type Path struct {
	Name   string `json:"name"`
	Online bool   `json:"online"`
	Source *struct {
		Type string `json:"type"`
		ID   string `json:"id"`
	} `json:"source"`
	ReadyTime string `json:"readyTime"`
	// Tracks2 carries per-track codec properties. This is the only place the
	// resolution actually negotiated by the publisher is exposed: an RTSP
	// publisher (the Camera Agent) never reports it through the control API,
	// so we read it back from the media track instead.
	Tracks2 []struct {
		Codec      string         `json:"codec"`
		CodecProps map[string]any `json:"codecProps"`
	} `json:"tracks2"`
	// BytesReceived is the cumulative inbound byte count for the path. The
	// monitor samples it across scans to derive a live bitrate, which the
	// publisher never reports on its own.
	BytesReceived int64 `json:"bytesReceived"`
}

// PathsListResponse is the response of GET /v3/paths/list.
type PathsListResponse struct {
	Items []Path `json:"items"`
}

// Manager controls the MediaMTX lifecycle.
type Manager struct {
	cfg           *config.Config
	cmd           *exec.Cmd
	generatedPath string
	apiBase       string
	webrtcBase    string
	httpClient    *http.Client
}

func New(cfg *config.Config) *Manager {
	return &Manager{
		cfg:        cfg,
		apiBase:    "http://" + cfg.APIAddr(),
		webrtcBase: "http://" + cfg.WebRTCAPIAddr(),
		httpClient: &http.Client{Timeout: 5 * time.Second},
	}
}

// resolveBinary returns an existing binary path, appending .exe on Windows when
// needed, so a single cross-platform config value works on both platforms.
func resolveBinary(p string) string {
	if _, err := os.Stat(p); err == nil {
		return p
	}
	if runtime.GOOS == "windows" {
		if _, err := os.Stat(p + ".exe"); err == nil {
			return p + ".exe"
		}
	}
	return p
}

// findBinary resolves the MediaMTX binary using a fallback chain so the server
// starts on any machine without manual config edits:
//  1. the explicit path from config (with .exe appended on Windows if missing)
//  2. the workspace-bundled copy at ./mediamtx/mediamtx(.exe)
//  3. mediamtx(.exe) resolved via the system PATH
//
// This is what lets a config that points at a developer's local install
// (e.g. D:/data/agent-tools/...) still boot on a machine where that path does
// not exist: it transparently falls back to the bundled or PATH binary.
func findBinary(cfg *config.Config) (string, error) {
	candidates := make([]string, 0, 3)
	if cfg.MediaMTX.Binary != "" {
		candidates = append(candidates, resolveBinary(cfg.MediaMTX.Binary))
	}
	bundled := "mediamtx/mediamtx"
	if runtime.GOOS == "windows" {
		bundled += ".exe"
	}
	candidates = append(candidates, bundled)
	if p, err := exec.LookPath("mediamtx"); err == nil {
		candidates = append(candidates, p)
	}

	seen := make(map[string]struct{}, len(candidates))
	for _, c := range candidates {
		if _, dup := seen[c]; dup {
			continue
		}
		seen[c] = struct{}{}
		if _, err := os.Stat(c); err == nil {
			logger.Info("using mediamtx binary: %s", c)
			return c, nil
		}
	}
	return "", fmt.Errorf("mediamtx binary not found; tried [%s] (set mediamtx.binary in config)", strings.Join(candidates, ", "))
}

// GenerateConfig writes the MediaMTX YAML derived from our Config so that ports
// stay the single source of truth. It is regenerated on every start. The file is
// written to a unique temp name inside the configured config directory so that
// (a) it never clobbers a checked-in config file and (b) repeated starts never
// try to overwrite a pre-existing file (which some sandboxes forbid).
func (m *Manager) GenerateConfig() error {
	// Media ports bind to mediamtx.bind (0.0.0.0 by default) so cameras and
	// players anywhere on the LAN can push/pull. The control API is a separate
	// bind address and defaults to loopback - only this server uses it, so
	// there is no reason to hand full stream control to the whole network.
	// Notes on the non-obvious entries:
	//  - rtmp/srt/moq are turned OFF. This project ingests RTSP and serves
	//    RTSP/WebRTC/HLS only; leaving the others on opens three more listening
	//    sockets per start and makes a second instance impossible.
	//  - webrtcLocalTCPAddress enables ICE over TCP. Without it WebRTC can only
	//    use UDP, which is precisely what phones on Wi-Fi (or behind routers
	//    that filter client-to-client UDP) often cannot get through - the
	//    classic "fine on the desktop, black rectangle on the phone" symptom.
	//  - webrtcIPsFromInterfaces turns every local interface IP into an ICE
	//    candidate, so a phone can pick the one it can actually route to.
	body := fmt.Sprintf(`logLevel: info
api: true
apiAddress: %s
rtsp: true
rtspAddress: %s
rtpAddress: :%d
rtcpAddress: :%d
rtmp: false
srt: false
moq: false
webrtc: true
webrtcAddress: %s
webrtcLocalUDPAddress: :%d
webrtcLocalTCPAddress: :%d
webrtcIPsFromInterfaces: true
hls: true
hlsVariant: mpegts
hlsAddress: %s
playback: false
record: false
paths:
  all_others:
    source: publisher
`,
		m.cfg.APIListenAddr(),
		m.cfg.MediaListenAddr(m.cfg.RTSP.Port),
		m.cfg.MediaMTX.RTPPort,
		m.cfg.MediaMTX.RTCPPort,
		m.cfg.MediaListenAddr(m.cfg.WebRTC.Port),
		m.cfg.MediaMTX.ICEUDPPort,
		m.cfg.MediaMTX.ICETCPPort,
		m.cfg.MediaListenAddr(m.cfg.MediaMTX.HLSPort),
	)

	dir := dirOf(m.cfg.MediaMTX.Config)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return fmt.Errorf("create config dir: %w", err)
	}
	f, err := os.CreateTemp(dir, "mediamtx-*.yml")
	if err != nil {
		return fmt.Errorf("write mediamtx config: %w", err)
	}
	if _, err := f.WriteString(body); err != nil {
		f.Close()
		return fmt.Errorf("write mediamtx config: %w", err)
	}
	if err := f.Close(); err != nil {
		return fmt.Errorf("write mediamtx config: %w", err)
	}
	m.generatedPath = f.Name()
	logger.Debug("mediamtx config written to %s", m.generatedPath)
	return nil
}

// Start launches MediaMTX and waits (briefly) for its API to become ready.
func (m *Manager) Start(ctx context.Context) error {
	if err := m.GenerateConfig(); err != nil {
		return err
	}
	binary, err := findBinary(m.cfg)
	if err != nil {
		return err
	}
	cmd := exec.Command(binary, m.generatedPath)
	cmd.Stdout = &logWriter{}
	cmd.Stderr = &logWriter{}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("start mediamtx: %w", err)
	}
	m.cmd = cmd
	logger.Info("mediamtx started (binary=%s config=%s)", binary, m.cfg.MediaMTX.Config)

	if !m.waitReady(ctx, 12*time.Second) {
		logger.Warn("mediamtx api not reachable within timeout; continuing")
	}
	return nil
}

// Stop terminates the MediaMTX subprocess.
func (m *Manager) Stop() {
	if m.cmd == nil || m.cmd.Process == nil {
		return
	}
	_ = m.cmd.Process.Kill()
	_, _ = m.cmd.Process.Wait()
	m.cmd = nil
	if m.generatedPath != "" {
		_ = os.Remove(m.generatedPath)
		m.generatedPath = ""
	}
	logger.Info("mediamtx stopped")
}

func (m *Manager) waitReady(ctx context.Context, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if m.Health() {
			return true
		}
		select {
		case <-ctx.Done():
			return false
		case <-time.After(300 * time.Millisecond):
		}
	}
	return false
}

// Health reports whether the MediaMTX control API responds.
func (m *Manager) Health() bool {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, m.apiBase+"/v3/paths/list", nil)
	if err != nil {
		return false
	}
	resp, err := m.httpClient.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	return resp.StatusCode == http.StatusOK
}

// PathsList returns the current MediaMTX paths (publishers/streams).
func (m *Manager) PathsList(ctx context.Context) (*PathsListResponse, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, m.apiBase+"/v3/paths/list?itemsPerPage=1000", nil)
	if err != nil {
		return nil, err
	}
	resp, err := m.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("paths/list returned status %d", resp.StatusCode)
	}
	var out PathsListResponse
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, fmt.Errorf("decode paths/list: %w", err)
	}
	return &out, nil
}

// WebRTCOffer forwards a browser SDP offer to MediaMTX's WebRTC read endpoint and
// returns the SDP answer. This keeps browser<->mediamtx signaling server-side so
// the WebRTC port need not be CORS-exposed.
func (m *Manager) WebRTCOffer(ctx context.Context, streamPath, offer string) (string, error) {
	// MediaMTX exposes WebRTC read (play) via the WHEP endpoint
	// (POST /<path>/whep) on its WebRTC port. It answers with 201 Created
	// and the SDP answer in the body.
	url := fmt.Sprintf("%s/%s/whep", m.webrtcBase, streamPath)
	// MediaMTX's WHEP endpoint can bind a few hundred ms AFTER its control API
	// reports ready, so the first offer right after a server start may hit a
	// not-yet-listening port (connection refused). Retry only transient
	// transport errors for a short window so the browser never sees a one-shot
	// 502 - logical status errors (4xx/5xx) are returned immediately.
	const tries = 4
	var lastErr error
	for attempt := 0; attempt < tries; attempt++ {
		if attempt > 0 {
			select {
			case <-ctx.Done():
				return "", ctx.Err()
			case <-time.After(250 * time.Millisecond):
			}
		}
		req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewBufferString(offer))
		if err != nil {
			return "", err
		}
		req.Header.Set("Content-Type", "application/sdp")
		resp, err := m.httpClient.Do(req)
		if err != nil {
			lastErr = err
			continue // upstream not reachable yet - retry
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusCreated {
			b, _ := io.ReadAll(resp.Body)
			return "", fmt.Errorf("webrtc signaling status %d: %s", resp.StatusCode, string(b))
		}
		buf := new(bytes.Buffer)
		if _, err := buf.ReadFrom(resp.Body); err != nil {
			return "", err
		}
		return buf.String(), nil
	}
	return "", fmt.Errorf("webrtc signaling failed after %d tries: %w", tries, lastErr)
}

// logWriter adapts MediaMTX's stdout/stderr to our leveled logger, line by line.
type logWriter struct{ buf []byte }

func (w *logWriter) Write(p []byte) (int, error) {
	w.buf = append(w.buf, p...)
	for {
		idx := bytes.IndexByte(w.buf, '\n')
		if idx < 0 {
			break
		}
		line := strings.TrimRight(string(w.buf[:idx]), "\r")
		w.buf = w.buf[idx+1:]
		if line != "" {
			logger.Info("[mediamtx] %s", line)
		}
	}
	return len(p), nil
}

func dirOf(p string) string {
	if i := strings.LastIndexAny(p, `/\`); i >= 0 {
		return p[:i]
	}
	return "."
}

// LocalIP returns the machine's primary non-loopback IPv4, used to build
// LAN-facing URLs. It is a thin wrapper kept for existing callers.
func LocalIP() string {
	return netiface.Primary()
}
