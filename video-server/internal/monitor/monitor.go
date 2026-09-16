// Package monitor periodically polls MediaMTX and keeps the camera table in
// sync: it auto-registers streams that have an active publisher and flips
// cameras to offline when no data has been seen for a timeout.
package monitor

import (
	"context"
	"fmt"
	"math"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"

	"video-server/internal/camera"
	"video-server/internal/config"
	"video-server/internal/logger"
	"video-server/internal/mediamtx"
	"video-server/internal/stream"
)

type Monitor struct {
	repo           *camera.Repository
	streams        *stream.Manager
	cfg            *config.Config
	interval       time.Duration
	offlineTimeout time.Duration

	// Bitrate is derived from MediaMTX's cumulative inbound byte count, sampled
	// across scans. We keep the previous sample per path.
	lastBytes  map[string]int64
	lastBytesT map[string]time.Time

	// fps is probed asynchronously with ffprobe (MediaMTX does not expose it).
	// fpsCache holds the most recent value per path; the probe is throttled.
	fpsCache     map[string]int
	lastFpsProbe map[string]time.Time
	mu           sync.Mutex
}

func New(repo *camera.Repository, mtx *mediamtx.Manager, cfg *config.Config) *Monitor {
	return &Monitor{
		repo:           repo,
		streams:        stream.New(mtx),
		cfg:            cfg,
		interval:       3 * time.Second,
		offlineTimeout: 10 * time.Second,
		lastBytes:      map[string]int64{},
		lastBytesT:     map[string]time.Time{},
		fpsCache:       map[string]int{},
		lastFpsProbe:   map[string]time.Time{},
	}
}

// Run scans immediately and then on a ticker until ctx is cancelled.
func (m *Monitor) Run(ctx context.Context) {
	ticker := time.NewTicker(m.interval)
	defer ticker.Stop()
	m.scan(ctx)
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			m.scan(ctx)
		}
	}
}

func (m *Monitor) scan(ctx context.Context) {
	streams, err := m.streams.List(ctx)
	if err != nil {
		logger.Warn("monitor: stream list failed: %v", err)
		return
	}
	now := time.Now()
	for _, p := range streams {
		// Only consider paths that actually have a publisher (a Camera Agent
		// pushing). Idle paths (no source) are skipped to avoid phantom cameras.
		if !p.HasSource {
			continue
		}
		// online (publisher present and serving) -> ONLINE, otherwise OFFLINE.
		status := camera.StatusOffline
		if p.Ready {
			status = camera.StatusOnline
		}

		// Bitrate from the inbound byte-count delta. Skip until we have two
		// samples; a zero/negative delta simply means "do not update yet".
		bitrate := 0
		if p.Ready && p.BytesReceived > 0 {
			if lb, ok := m.lastBytes[p.Path]; ok {
				dt := now.Sub(m.lastBytesT[p.Path]).Seconds()
				if dt > 0.5 {
					kb := int(float64(p.BytesReceived-lb) * 8.0 / 1000.0 / dt)
					if kb > 0 {
						bitrate = kb
					}
				}
			}
			m.lastBytes[p.Path] = p.BytesReceived
			m.lastBytesT[p.Path] = now
		}

		// fps from the async ffprobe cache (filled when the probe completes).
		m.mu.Lock()
		fps := m.fpsCache[p.Path]
		m.mu.Unlock()

		// p.Resolution() is what the publisher actually negotiated (read back
		// from the MediaMTX track). It is empty until MediaMTX has parsed the
		// codec properties, so the repository only overwrites it when known.
		// fps/bitrate are likewise only written when > 0.
		if _, err := m.repo.UpsertByStreamPath(p.Path, status, now, p.Resolution(), fps, bitrate); err != nil {
			logger.Error("monitor: upsert %s failed: %v", p.Path, err)
			continue
		}
		logger.Debug("monitor: %s -> %s %s fps=%d bitrate=%dkbps", p.Path, status, p.Resolution(), fps, bitrate)

		// Trigger an async fps probe if one is due.
		m.maybeProbeFps(ctx, p.Path, now)
	}
	// Any camera not seen within the offline timeout is marked offline.
	if err := m.repo.MarkOfflineIfNotSeen(now.Add(-m.offlineTimeout)); err != nil {
		logger.Error("monitor: mark offline failed: %v", err)
	}
}

// maybeProbeFps launches a throttled ffprobe goroutine to recover the stream
// framerate (MediaMTX exposes width/height but not fps). The result is cached
// and written on the next scan.
func (m *Monitor) maybeProbeFps(ctx context.Context, path string, now time.Time) {
	m.mu.Lock()
	last := m.lastFpsProbe[path]
	due := now.Sub(last) > 120*time.Second
	if due {
		m.lastFpsProbe[path] = now
	}
	m.mu.Unlock()
	if !due {
		return
	}
	go m.probeFps(path)
}

func (m *Monitor) probeFps(path string) {
	url := fmt.Sprintf("rtsp://127.0.0.1:%d/%s", m.cfg.RTSP.Port, path)
	fps, err := ffprobeFps(context.Background(), url)
	if err != nil {
		logger.Debug("monitor: ffprobe fps for %s unavailable: %v", path, err)
		return
	}
	m.mu.Lock()
	m.fpsCache[path] = fps
	m.mu.Unlock()
	logger.Debug("monitor: ffprobe %s -> %dfps", path, fps)
}

// ffprobeFps reads the average frame rate of an RTSP stream. MediaMTX does not
// report fps, so we ask ffprobe to parse the SDP/codec. A short analyze window
// keeps the probe fast (<~5s). Returns an error if ffprobe is missing or the
// stream cannot be probed.
func ffprobeFps(ctx context.Context, url string) (int, error) {
	bin, err := exec.LookPath("ffprobe")
	if err != nil {
		return 0, fmt.Errorf("ffprobe not found: %w", err)
	}
	pctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	cmd := exec.CommandContext(pctx, bin,
		"-v", "error",
		"-rtsp_transport", "tcp",
		"-analyzeduration", "2500000",
		"-probesize", "500000",
		"-show_entries", "stream=avg_frame_rate",
		"-of", "csv=p=0",
		"-select_streams", "v:0",
		url,
	)
	out, err := cmd.Output()
	if err != nil {
		return 0, fmt.Errorf("ffprobe failed: %w", err)
	}
	s := strings.TrimSpace(string(out))
	if s == "" {
		return 0, fmt.Errorf("ffprobe returned empty avg_frame_rate")
	}
	// avg_frame_rate looks like "8/1" or "30000/1001".
	parts := strings.SplitN(s, "/", 2)
	num, err1 := strconv.ParseFloat(strings.TrimSpace(parts[0]), 64)
	if err1 != nil {
		return 0, fmt.Errorf("bad numerator %q", parts[0])
	}
	den := 1.0
	if len(parts) == 2 {
		if d, err2 := strconv.ParseFloat(strings.TrimSpace(parts[1]), 64); err2 == nil && d > 0 {
			den = d
		}
	}
	if den <= 0 || num <= 0 {
		return 0, fmt.Errorf("bad frame rate %q", s)
	}
	fps := int(math.Round(num / den))
	if fps <= 0 || fps > 1000 {
		return 0, fmt.Errorf("implausible frame rate %d", fps)
	}
	return fps, nil
}
