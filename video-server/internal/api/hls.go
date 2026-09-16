package api

import (
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"

	"video-server/internal/config"
	"video-server/internal/logger"
)

// hlsProxy godoc: GET /hls/{stream}/{file}
//
// Serves MediaMTX's HLS output through our own HTTP port.
//
// Why proxy instead of pointing the browser straight at :8888:
//   - same origin, so there is no CORS preflight and no mixed-content block;
//   - the phone only ever needs ONE port open (the HTTP port), which matters a
//     lot when a firewall rule or a router is in the way;
//   - HLS is plain HTTP over TCP, so it keeps working in exactly the cases where
//     WebRTC fails on phones: UDP/ICE blocked between wireless clients, and
//     mobile WebKit refusing WebRTC on an insecure (http) origin.
//
// It is the fallback path, not the primary one: WebRTC stays the default
// because it is the only sub-second option.
func (h *Handler) hlsProxy(w http.ResponseWriter, r *http.Request) {
	stream := r.PathValue("stream")
	file := strings.TrimPrefix(r.PathValue("file"), "/")
	if stream == "" {
		http.Error(w, "missing stream", http.StatusBadRequest)
		return
	}
	if file == "" {
		file = "index.m3u8"
	}

	target := fmt.Sprintf("http://%s/%s/%s", h.cfg.HLSUpstreamAddr(), stream, file)
	if r.URL.RawQuery != "" {
		target += "?" + r.URL.RawQuery
	}

	// Session affinity: MediaMTX mints per-session HLS sessions and expects the
	// same reader to come back, so forward the query it gave us verbatim (done
	// above) and keep the request anonymous beyond that.
	// Retry only transient transport errors for a short window - MediaMTX's HLS
	// endpoint can bind a few hundred ms after its control API is ready, so the
	// first request right after a server start would otherwise 502. Logical
	// status codes (e.g. 404 when a stream is not yet published) return at once.
	const tries = 4
	var lastErr error
	var resp *http.Response
	for attempt := 0; attempt < tries; attempt++ {
		if attempt > 0 {
			select {
			case <-r.Context().Done():
				http.Error(w, "request canceled", http.StatusBadGateway)
				return
			case <-time.After(250 * time.Millisecond):
			}
		}
		req, err := http.NewRequestWithContext(r.Context(), http.MethodGet, target, nil)
		if err != nil {
			http.Error(w, "bad upstream request", http.StatusBadGateway)
			return
		}
		resp, err = hlsClient.Do(req)
		if err != nil {
			lastErr = err
			continue // upstream not reachable yet - retry
		}
		break
	}
	if resp == nil {
		logger.Debug("hls proxy %s -> %v", target, lastErr)
		http.Error(w, "hls upstream unreachable: "+lastErr.Error(), http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()

	copyHeader(w.Header(), resp.Header, "Content-Type", "Content-Length", "Cache-Control", "ETag", "Last-Modified")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(resp.StatusCode)
	if r.Method != http.MethodHead {
		_, _ = io.Copy(w, resp.Body)
	}
}

// hlsClient is kept separate from the API client: HLS polling is chatty and
// must never outlive a stuck segment fetch for long.
var hlsClient = &http.Client{Timeout: 30 * time.Second}

func copyHeader(dst, src http.Header, keys ...string) {
	for _, k := range keys {
		if v := src.Get(k); v != "" {
			dst.Set(k, v)
		}
	}
}

// HLSURL builds the same-origin HLS URL the browser should hand to hls.js.
func HLSURL(cfg *config.Config, streamPath string) string {
	return fmt.Sprintf("/hls/%s/index.m3u8", streamPath)
}

// HLSPlaylistAddr is the absolute HLS URL (direct MediaMTX port), used in logs
// and in the API so a human can test the stream with a plain player.
func HLSPlaylistAddr(cfg *config.Config, streamPath string) string {
	return fmt.Sprintf("http://%s/%s/index.m3u8",
		joinHostPort(cfg.PublicHost(), cfg.MediaMTX.HLSPort), streamPath)
}

func joinHostPort(host string, port int) string {
	if strings.Contains(host, ":") {
		return "[" + host + "]:" + strconv.Itoa(port)
	}
	return host + ":" + strconv.Itoa(port)
}
