package api

import (
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"video-server/internal/metadata"
)

// handSessionFileVersion is bumped only if the exported file's SHAPE changes in
// a way a previous reader would misread. Adding a field does not qualify.
const handSessionFileVersion = 1

// lookupCamera resolves a path id the way the read endpoints do: a registered
// camera id, or a stream path that maps to one. Unlike the ingest path there is
// no permissive fallback - a read for a camera nobody knows about is a 404, not
// an empty result, because returning "0 sessions" for a typo would look like a
// working but empty recorder.
func (h *Handler) lookupCamera(w http.ResponseWriter, id string) (string, bool) {
	cam, err := h.repo.Get(id)
	if err != nil {
		if cam, err = h.repo.GetByStreamPath(id); err != nil {
			http.Error(w, err.Error(), http.StatusNotFound)
			return "", false
		}
	}
	return cam.ID, true
}

// handGap parses the optional gap_ms override, falling back to the configured
// default. The gap is a property of the analysis, not of the recording, so it
// is a request parameter - but an invalid one is a 400 rather than a silent
// fallback, since a typo would otherwise quietly change which sessions exist.
func (h *Handler) handGap(w http.ResponseWriter, r *http.Request) (int, bool) {
	raw := r.URL.Query().Get("gap_ms")
	if raw == "" {
		return h.cfg.Metadata.HandSessionGapMS, true
	}
	n, err := strconv.Atoi(raw)
	if err != nil || n <= 0 {
		http.Error(w, "gap_ms must be a positive integer", http.StatusBadRequest)
		return 0, false
	}
	return n, true
}

// listHandSessions godoc: GET /api/cameras/{id}/hand/sessions
//
// Sessions of the append-only hand recorder for one camera. A session is a run
// of frames in which a hand was actually detected (see metadata.splitHandSessions),
// so this is the index the export endpoint indexes into.
//
// Optional: ?gap_ms=N overrides the gap that separates two sessions.
func (h *Handler) listHandSessions(w http.ResponseWriter, r *http.Request) {
	if h.meta == nil {
		http.Error(w, "metadata ingest is disabled", http.StatusServiceUnavailable)
		return
	}
	id, ok := h.lookupCamera(w, r.PathValue("id"))
	if !ok {
		return
	}
	gap, ok := h.handGap(w, r)
	if !ok {
		return
	}
	list, err := h.meta.HandSessions(id, gap)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, list)
}

// getHandSession godoc: GET /api/cameras/{id}/hand/sessions/{index}
//
// One session with every recorded frame in it, saved by the browser as
// hand_session_<camera>_<index>.json.
//
// index is the 1-based "index" from the list endpoint. It is deliberately an
// index rather than a timestamp range: the list and the export then agree by
// construction, and the two calls use the same gap_ms, so a client cannot ask
// for a session that "exists" under one split and not another.
func (h *Handler) getHandSession(w http.ResponseWriter, r *http.Request) {
	if h.meta == nil {
		http.Error(w, "metadata ingest is disabled", http.StatusServiceUnavailable)
		return
	}
	id, ok := h.lookupCamera(w, r.PathValue("id"))
	if !ok {
		return
	}
	index, err := strconv.Atoi(r.PathValue("index"))
	if err != nil || index < 1 {
		http.Error(w, "session index must be a positive integer", http.StatusBadRequest)
		return
	}
	gap, ok := h.handGap(w, r)
	if !ok {
		return
	}
	session, rows, err := h.meta.HandSessionRows(id, index, gap)
	if errors.Is(err, metadata.ErrNoSuchSession) {
		http.Error(w, fmt.Sprintf("camera %s has no session %d", id, index), http.StatusNotFound)
		return
	}
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	file := metadata.HandSessionFile{
		Version:    handSessionFileVersion,
		CameraID:   id,
		ExportedAt: time.Now().UTC(),
		Session:    session,
		GapMS:      gap,
		Frames:     rows,
	}
	w.Header().Set("Content-Disposition",
		fmt.Sprintf("attachment; filename=%q", handSessionFilename(id, index)))
	writeJSON(w, http.StatusOK, file)
}

// clearHandLog godoc: DELETE /api/cameras/{id}/hand/log
//
// Drops every recorded hand frame for one camera and reports the count. The
// recorder is always on, so without this a calibration run would have to start
// from a log that already contains the previous one - and the only alternative
// would be deleting the database.
func (h *Handler) clearHandLog(w http.ResponseWriter, r *http.Request) {
	if h.meta == nil {
		http.Error(w, "metadata ingest is disabled", http.StatusServiceUnavailable)
		return
	}
	id, ok := h.lookupCamera(w, r.PathValue("id"))
	if !ok {
		return
	}
	n, err := h.meta.ClearHandLog(id)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"camera_id": id, "cleared": n})
}

// handSessionFilename builds the download name. Camera ids come from the path
// and can in principle hold anything, so anything outside a conservative set is
// replaced: a "/" or ".." in header value would be a path-traversal primitive
// in whatever directory the file lands in.
func handSessionFilename(cameraID string, index int) string {
	var b strings.Builder
	for _, r := range cameraID {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9',
			r == '-', r == '_', r == '.':
			b.WriteRune(r)
		default:
			b.WriteByte('_')
		}
	}
	return fmt.Sprintf("hand_session_%s_%d.json", b.String(), index)
}
