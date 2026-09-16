// Package api implements the HTTP REST API and (via the server package) the
// embedded Web UI. All endpoints live under /api/*; static assets are served by
// the server package's SPA handler.
package api

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"strconv"

	"video-server/internal/camera"
	"video-server/internal/config"
	"video-server/internal/logger"
	"video-server/internal/mediamtx"
	"video-server/internal/metadata"
	"video-server/internal/netiface"
)

// Handler holds the dependencies shared by all API endpoints.
type Handler struct {
	cfg  *config.Config
	repo *camera.Repository
	mtx  *mediamtx.Manager
	db   *sql.DB
	// meta stores the AI metadata pushed by camera-agents. It is nil when the
	// feature is disabled; the ingest handler answers 503 in that case.
	meta *metadata.Repository
	// ai deduplicates AI liveness log lines (see aiTracker).
	ai *aiTracker
}

func New(cfg *config.Config, repo *camera.Repository, mtx *mediamtx.Manager, db *sql.DB, meta *metadata.Repository) *Handler {
	return &Handler{cfg: cfg, repo: repo, mtx: mtx, db: db, meta: meta, ai: newAITracker()}
}

// decorate fills computed/transport fields (e.g. rtsp_url) on a camera.
func (h *Handler) decorate(c *camera.Camera) {
	c.RTSPURL = h.cfg.RTSPURL(c.StreamPath)
}

// writeJSON writes v as JSON with the given status code.
func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

// readJSON decodes the request body into v.
func readJSON(r *http.Request, v any) error {
	defer r.Body.Close()
	return json.NewDecoder(r.Body).Decode(v)
}

func boolStr(b bool) string {
	if b {
		return "ok"
	}
	return "error"
}

// health godoc: GET /api/health
func (h *Handler) health(w http.ResponseWriter, r *http.Request) {
	dbOK := h.db.Ping() == nil
	mediaOK := h.mtx.Health()
	status := "ok"
	if !dbOK || !mediaOK {
		status = "error"
	}
	writeJSON(w, http.StatusOK, map[string]string{
		"status":       status,
		"database":     boolStr(dbOK),
		"media_server": boolStr(mediaOK),
	})
}

// listCameras godoc: GET /api/cameras
func (h *Handler) listCameras(w http.ResponseWriter, r *http.Request) {
	cams, err := h.repo.List()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	for i := range cams {
		h.decorate(&cams[i])
	}
	writeJSON(w, http.StatusOK, cams)
}

// createCamera godoc: POST /api/cameras
func (h *Handler) createCamera(w http.ResponseWriter, r *http.Request) {
	var c camera.Camera
	if err := readJSON(r, &c); err != nil {
		http.Error(w, "invalid json: "+err.Error(), http.StatusBadRequest)
		return
	}
	if c.ID == "" {
		http.Error(w, "id is required", http.StatusBadRequest)
		return
	}
	c.Status = camera.NormalizeStatus(string(c.Status))
	if err := h.repo.Create(c); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	created, err := h.repo.Get(c.ID)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	h.decorate(&created)
	writeJSON(w, http.StatusCreated, created)
}

// getCamera godoc: GET /api/cameras/{id}
func (h *Handler) getCamera(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	c, err := h.repo.Get(id)
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	h.decorate(&c)
	writeJSON(w, http.StatusOK, c)
}

// updateCamera godoc: PUT /api/cameras/{id}
func (h *Handler) updateCamera(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	var c camera.Camera
	if err := readJSON(r, &c); err != nil {
		http.Error(w, "invalid json: "+err.Error(), http.StatusBadRequest)
		return
	}
	c.ID = id
	c.Status = camera.NormalizeStatus(string(c.Status))
	if err := h.repo.Update(c); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	updated, err := h.repo.Get(id)
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	h.decorate(&updated)
	writeJSON(w, http.StatusOK, updated)
}

// deleteCamera godoc: DELETE /api/cameras/{id}
func (h *Handler) deleteCamera(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if err := h.repo.Delete(id); err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	// Best effort: a camera row removed while its AI rows linger would leave
	// orphaned metadata that /api/metadata keeps reporting forever.
	if h.meta != nil {
		if err := h.meta.DeleteCamera(id); err != nil {
			logger.Warn("cleanup metadata for camera %s failed: %v", id, err)
		}
	}
	w.WriteHeader(http.StatusNoContent)
}

// cameraStatus godoc: GET /api/cameras/{id}/status
func (h *Handler) cameraStatus(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	c, err := h.repo.Get(id)
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"id": c.ID, "status": string(c.Status)})
}

// cameraStream godoc: GET /api/cameras/{id}/stream
func (h *Handler) cameraStream(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	c, err := h.repo.Get(id)
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	h.decorate(&c)
	writeJSON(w, http.StatusOK, map[string]any{
		"id":          c.ID,
		"name":        c.Name,
		"stream_path": c.StreamPath,
		"status":      string(c.Status),
		"rtsp_url":    c.RTSPURL,
		"resolution":  c.Resolution,
		"fps":         c.FPS,
		"bitrate":     c.Bitrate,
		"webrtc": map[string]string{
			"signaling": "/api/cameras/" + c.ID + "/webrtc",
			"path":      c.StreamPath,
		},
		// Same-origin fallback for phones: plain HTTP/TCP, so it survives
		// blocked UDP/ICE and WebKit's refusal to run WebRTC on http.
		"hls_url": HLSURL(h.cfg, c.StreamPath),
		// Direct MediaMTX port, handy for debugging with an external player.
		"hls_direct_url": HLSPlaylistAddr(h.cfg, c.StreamPath),
	})
}

// networkAddresses godoc: GET /api/net/addresses
// Reports how the server is bound and every URL a client can use to reach it.
// The UI (and anyone debugging LAN access) can call this instead of guessing
// which of the machine's addresses is the right one.
func (h *Handler) networkAddresses(w http.ResponseWriter, r *http.Request) {
	addrs := netiface.Enumerate()
	if addrs == nil {
		addrs = []netiface.Address{}
	}
	urls := make([]string, 0, len(addrs))
	wildcard := netiface.IsWildcard(h.cfg.Server.Bind)
	for _, a := range addrs {
		if netiface.IsLoopbackBind(h.cfg.Server.Bind) && !a.Loopback {
			continue
		}
		if !wildcard && a.IP != h.cfg.Server.Bind {
			continue
		}
		urls = append(urls, fmt.Sprintf("http://%s/", net.JoinHostPort(a.IP, strconv.Itoa(h.cfg.Server.HTTPPort))))
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"bind":        h.cfg.Server.Bind,
		"http_port":   h.cfg.Server.HTTPPort,
		"public_host": h.cfg.PublicHost(),
		"rtsp_url":    h.cfg.RTSPURL("<stream-path>"),
		"media_bind":  h.cfg.MediaMTX.Bind,
		"rtsp_port":   h.cfg.RTSP.Port,
		"webrtc_port": h.cfg.WebRTC.Port,
		"hls_port":    h.cfg.MediaMTX.HLSPort,
		"api_listen":  h.cfg.APIListenAddr(),
		"addresses":   addrs,
		"web_ui_urls": urls,
	})
}

// cameraWebRTC godoc: POST /api/cameras/{id}/webrtc  body: {"sdp": "..."}
// Proxies the SDP offer to MediaMTX's WebRTC endpoint and returns the answer.
func (h *Handler) cameraWebRTC(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	var body struct {
		SDP string `json:"sdp"`
	}
	if err := readJSON(r, &body); err != nil || body.SDP == "" {
		http.Error(w, "invalid or missing sdp", http.StatusBadRequest)
		return
	}
	streamPath := id
	if cam, err := h.repo.Get(id); err == nil {
		streamPath = cam.StreamPath
	}
	answer, err := h.mtx.WebRTCOffer(r.Context(), streamPath, body.SDP)
	if err != nil {
		http.Error(w, "webrtc signaling failed: "+err.Error(), http.StatusBadGateway)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"type": "answer", "sdp": answer})
}
