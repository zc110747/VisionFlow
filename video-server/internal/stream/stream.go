// Package stream is the Stream Manager: it presents the live MediaMTX paths in a
// server-friendly shape and is consumed by the monitor (auto-discovery/status) and
// the API (stream metadata).
package stream

import (
	"context"
	"strconv"
	"strings"

	"video-server/internal/mediamtx"
)

// Stream is a normalized view of a MediaMTX path.
type Stream struct {
	Path       string
	State      string
	SourceType string
	HasSource  bool
	Ready      bool
	// Width/Height are the resolution the publisher actually negotiated, read
	// back from the MediaMTX track properties. Zero when unknown.
	Width  int
	Height int
	// BytesReceived is MediaMTX's cumulative inbound count for this path.
	// Sampled across scans to compute a live bitrate.
	BytesReceived int64
}

// Resolution renders Width/Height as "WxH", or "" when MediaMTX has not
// reported track properties yet.
func (s Stream) Resolution() string {
	if s.Width <= 0 || s.Height <= 0 {
		return ""
	}
	return strconv.Itoa(s.Width) + "x" + strconv.Itoa(s.Height)
}

// Manager lists live streams from MediaMTX.
type Manager struct {
	mtx *mediamtx.Manager
}

func New(m *mediamtx.Manager) *Manager {
	return &Manager{mtx: m}
}

// List returns the currently active/known streams.
func (m *Manager) List(ctx context.Context) ([]Stream, error) {
	list, err := m.mtx.PathsList(ctx)
	if err != nil {
		return nil, err
	}
	out := make([]Stream, 0, len(list.Items))
	for _, p := range list.Items {
		has := p.Source != nil
		state := "offline"
		if p.Online {
			state = "online"
		} else if has {
			state = "connecting"
		}
		w, h := videoResolution(p)
		out = append(out, Stream{
			Path:          p.Name,
			State:         state,
			SourceType:    sourceType(p),
			HasSource:     has,
			Ready:         p.Online,
			Width:         w,
			Height:        h,
			BytesReceived: p.BytesReceived,
		})
	}
	return out, nil
}

func sourceType(p mediamtx.Path) string {
	if p.Source == nil {
		return ""
	}
	return p.Source.Type
}

// videoResolution extracts width/height from the first video track MediaMTX
// reports. Returns (0,0) when the track list is empty or carries no usable
// dimensions (e.g. an audio-only stream).
func videoResolution(p mediamtx.Path) (int, int) {
	for _, t := range p.Tracks2 {
		if !strings.EqualFold(t.Codec, "H264") &&
			!strings.EqualFold(t.Codec, "H265") &&
			!strings.EqualFold(t.Codec, "VP8") &&
			!strings.EqualFold(t.Codec, "VP9") &&
			!strings.EqualFold(t.Codec, "AV1") {
			continue
		}
		w := codecPropInt(t.CodecProps, "width")
		h := codecPropInt(t.CodecProps, "height")
		if w > 0 && h > 0 {
			return w, h
		}
	}
	return 0, 0
}

// codecPropInt reads a numeric codec property. MediaMTX emits JSON numbers, but
// the field is typed loosely so both float64 and a numeric string are accepted.
func codecPropInt(props map[string]any, key string) int {
	switch v := props[key].(type) {
	case float64:
		return int(v)
	case int:
		return v
	case string:
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return 0
}
