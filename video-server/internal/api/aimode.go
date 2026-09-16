package api

import (
	"net/http"
)

// getAIMode godoc: GET /api/cameras/{id}/aimode
//
// Returns the desired AI mode the web UI last selected for this camera. The
// camera-agent polls this endpoint and switches its detector accordingly, so
// the response is intentionally tiny: just {"mode": "..."}.
//
// Modes:
//
//	ai-off    -> agent keeps its loaded model; the UI only hides the overlay
//	ai-y      -> person detection (yolo11n)
//	ai-y-pose -> person detection + 17 COCO keypoints (yolo11n-pose)
//
// Before any POST has arrived the default (ai-y) is returned, so a fresh
// camera boots into detection without the UI having to set anything.
func (h *Handler) getAIMode(w http.ResponseWriter, r *http.Request) {
	if h.meta == nil {
		http.Error(w, "metadata feature disabled", http.StatusServiceUnavailable)
		return
	}
	id := r.PathValue("id")
	mode, err := h.meta.GetAIMode(id)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"mode": mode})
}

// setAIMode godoc: POST /api/cameras/{id}/aimode
//
//	body: {"mode": "ai-off" | "ai-y" | "ai-y-pose"}
//
// Persists the desired mode. The camera-agent discovers the change by polling;
// there is no push channel from server to agent. Invalid modes are rejected so
// the DB never holds a value the poller cannot interpret.
func (h *Handler) setAIMode(w http.ResponseWriter, r *http.Request) {
	if h.meta == nil {
		http.Error(w, "metadata feature disabled", http.StatusServiceUnavailable)
		return
	}
	id := r.PathValue("id")
	var body struct {
		Mode string `json:"mode"`
	}
	if err := readJSON(r, &body); err != nil || body.Mode == "" {
		http.Error(w, "invalid or missing mode", http.StatusBadRequest)
		return
	}
	if err := h.meta.SetAIMode(id, body.Mode); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"mode": body.Mode})
}
