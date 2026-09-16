package api

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"sync"
	"time"

	"video-server/internal/logger"
	"video-server/internal/metadata"
)

// aiTracker remembers the last known AI liveness per camera so the server logs
// a transition once instead of a line per heartbeat (5/s per camera).
type aiTracker struct {
	mu        sync.Mutex
	running   map[string]bool
	announced map[string]bool
}

func newAITracker() *aiTracker {
	return &aiTracker{running: map[string]bool{}, announced: map[string]bool{}}
}

// transition reports whether the AI running flag changed for this camera.
func (t *aiTracker) transition(camID string, running bool) (changed, first bool) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if _, ok := t.announced[camID]; !ok {
		t.announced[camID] = true
		t.running[camID] = running
		return false, true
	}
	if prev, ok := t.running[camID]; ok && prev == running {
		return false, false
	}
	t.running[camID] = running
	return true, false
}

// ingest godoc: POST /api/metadata
//
// Receiving end of the camera-agent AI metadata protocol. One endpoint serves
// both message kinds; the "type" field selects the decoder.
//
// Contract with the agent (see ../carmera-agent/README.md):
//   - ANY 2xx means "accepted". The response body is irrelevant, so this
//     handler answers 204 - the smallest possible acknowledgement.
//   - frame_id / timestamp come from the capture side and are stored verbatim.
//   - The agent drops (never buffers) results while it is disconnected, so
//     gaps in frame_id are normal and must not be treated as an error here.
func (h *Handler) ingest(w http.ResponseWriter, r *http.Request) {
	if !h.cfg.Metadata.Enabled || h.meta == nil {
		// 503 (not 404) tells the agent "unavailable, retry later" - exactly
		// the semantics its exponential backoff expects.
		w.Header().Set("Retry-After", "30")
		http.Error(w, "metadata ingest is disabled", http.StatusServiceUnavailable)
		return
	}

	// Bound the body BEFORE reading: a hostile or buggy agent must not be able
	// to exhaust memory with a single POST.
	maxBytes := h.cfg.Metadata.MaxBodyBytes
	if maxBytes <= 0 {
		maxBytes = 1 << 20
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxBytes)
	body, err := io.ReadAll(r.Body)
	if err != nil {
		var mbe *http.MaxBytesError
		if errors.As(err, &mbe) {
			http.Error(w, "metadata payload too large", http.StatusRequestEntityTooLarge)
			return
		}
		http.Error(w, "cannot read body: "+err.Error(), http.StatusBadRequest)
		return
	}
	if len(body) == 0 {
		http.Error(w, "empty body", http.StatusBadRequest)
		return
	}

	// First pass: only "type", so we know which struct to decode into.
	var probe metadata.Probe
	if err := json.Unmarshal(body, &probe); err != nil {
		http.Error(w, "invalid json: "+err.Error(), http.StatusBadRequest)
		return
	}

	kind := probe.Type
	if kind == "" {
		// Compatibility: agent builds before the frame discriminator was added
		// send frames WITHOUT "type" (the status/heartbeat always had it).
		// Inferring from the payload shape keeps those devices working instead
		// of making them back off forever on a 400.
		kind = metadata.InferType(body)
		logger.Debug("metadata: message without \"type\" - inferred %q from payload shape", kind)
	}

	switch kind {
	case metadata.TypeFrame:
		h.ingestFrame(w, body)
	case metadata.TypeStatus:
		h.ingestStatus(w, body)
	case "":
		http.Error(w, "missing \"type\" (expected \"frame\" or \"status\")", http.StatusBadRequest)
	default:
		http.Error(w, "unknown metadata type "+probe.Type, http.StatusBadRequest)
	}
}

func (h *Handler) ingestFrame(w http.ResponseWriter, body []byte) {
	var msg metadata.FrameMessage
	if err := json.Unmarshal(body, &msg); err != nil {
		http.Error(w, "invalid frame message: "+err.Error(), http.StatusBadRequest)
		return
	}
	if err := msg.Validate(); err != nil {
		http.Error(w, "invalid frame message: "+err.Error(), http.StatusBadRequest)
		return
	}
	camID, ok := h.resolveCamera(w, msg.CameraID)
	if !ok {
		return
	}
	// Canonicalise: everything downstream (storage, logs, /api/metadata keys)
	// must use the resolved camera id, not whatever string the agent sent.
	msg.CameraID = camID
	// Re-clamp on the server: the agent already does it, but the server must
	// not trust a producer it did not write.
	if dropped := msg.Normalize(); dropped > 0 {
		logger.Warn("metadata %s: dropped %d object(s) with unusable bbox", camID, dropped)
	}
	if err := h.meta.SaveFrame(&msg, time.Now().UTC()); err != nil {
		logger.Error("metadata %s: store frame failed: %v", camID, err)
		http.Error(w, "store failed", http.StatusInternalServerError)
		return
	}
	logger.Debug("metadata %s: frame %d objects=%d %dx%d",
		camID, msg.FrameID, len(msg.Objects), msg.VideoWidth, msg.VideoHeight)
	if changed, first := h.ai.transition(camID, true); first {
		logger.Info("metadata %s: AI results arriving (model data via heartbeat)", camID)
	} else if changed {
		logger.Info("metadata %s: AI results resumed", camID)
	}
	w.WriteHeader(http.StatusNoContent)
}

func (h *Handler) ingestStatus(w http.ResponseWriter, body []byte) {
	var msg metadata.StatusMessage
	if err := json.Unmarshal(body, &msg); err != nil {
		http.Error(w, "invalid status message: "+err.Error(), http.StatusBadRequest)
		return
	}
	if err := msg.Validate(); err != nil {
		http.Error(w, "invalid status message: "+err.Error(), http.StatusBadRequest)
		return
	}
	camID, ok := h.resolveCamera(w, msg.CameraID)
	if !ok {
		return
	}
	msg.CameraID = camID // keep storage keys canonical (see ingestFrame)
	if err := h.meta.SaveStatus(&msg, time.Now().UTC()); err != nil {
		logger.Error("metadata %s: store status failed: %v", camID, err)
		http.Error(w, "store failed", http.StatusInternalServerError)
		return
	}
	changed, first := h.ai.transition(camID, msg.AI.Running)
	switch {
	case first:
		logger.Info("metadata %s: AI %s (fps=%.2f model=%s tracker=%s)",
			camID, onOff(msg.AI.Running), msg.AI.FPS, msg.AI.Model, msg.AI.Tracker)
	case changed:
		logger.Info("metadata %s: AI %s", camID, onOff(msg.AI.Running))
	default:
		logger.Debug("metadata %s: heartbeat running=%v fps=%.2f processed=%d",
			camID, msg.AI.Running, msg.AI.FPS, msg.AI.Processed)
	}
	w.WriteHeader(http.StatusNoContent)
}

// resolveCamera maps the camera_id sent by the agent onto a known camera.
//
// The agent identifies itself with metadata.camera_id, which operators usually
// set equal to the RTSP stream path - and the monitor registers cameras under
// exactly that path. Both lookups are tried so either convention works.
//
// With require_known_camera = false (default) an unknown id is stored as-is:
// metadata normally arrives before the monitor's next 3s poll, and rejecting
// it would only make the agent back off and drop results for nothing.
//
// The bool result means "store it", not "camera exists": on the permissive path
// it returns true together with the raw id.
func (h *Handler) resolveCamera(w http.ResponseWriter, id string) (string, bool) {
	if id == "" {
		http.Error(w, "camera_id is required", http.StatusBadRequest)
		return "", false
	}
	if _, err := h.repo.Get(id); err == nil {
		return id, true
	}
	if cam, err := h.repo.GetByStreamPath(id); err == nil {
		return cam.ID, true
	}
	if h.cfg.Metadata.RequireKnownCamera {
		http.Error(w, "unknown camera "+id, http.StatusNotFound)
		return "", false
	}
	return id, true
}

// cameraMetadata godoc: GET /api/cameras/{id}/metadata
// Latest AI frame + heartbeat for one camera. Both may be absent - AI can be
// disabled while metadata stays on - so the payload uses pointers and the
// caller must nil-check rather than assume.
func (h *Handler) cameraMetadata(w http.ResponseWriter, r *http.Request) {
	if h.meta == nil {
		http.Error(w, "metadata ingest is disabled", http.StatusServiceUnavailable)
		return
	}
	id := r.PathValue("id")
	cam, err := h.repo.Get(id)
	if err != nil {
		if cam, err = h.repo.GetByStreamPath(id); err != nil {
			http.Error(w, err.Error(), http.StatusNotFound)
			return
		}
	}
	snap, err := h.meta.Latest(cam.ID)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, snap)
}

// listMetadata godoc: GET /api/metadata
// Snapshot for every camera that has ever pushed metadata.
func (h *Handler) listMetadata(w http.ResponseWriter, r *http.Request) {
	if h.meta == nil {
		writeJSON(w, http.StatusOK, map[string]any{"enabled": false, "cameras": []metadata.Snapshot{}})
		return
	}
	snaps, err := h.meta.List()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if snaps == nil {
		snaps = []metadata.Snapshot{}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"enabled": h.cfg.Metadata.Enabled,
		"cameras": snaps,
	})
}

func onOff(b bool) string {
	if b {
		return "running"
	}
	return "stopped"
}
