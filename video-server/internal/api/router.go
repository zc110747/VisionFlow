package api

import "net/http"

// Register mounts all /api/* routes onto the provided mux. The server package
// registers the SPA static handler separately; more specific /api patterns take
// precedence over the "/" fallback.
func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/health", h.health)
	mux.HandleFunc("GET /api/net/addresses", h.networkAddresses)
	mux.HandleFunc("GET /api/cameras", h.listCameras)
	mux.HandleFunc("POST /api/cameras", h.createCamera)
	mux.HandleFunc("GET /api/cameras/{id}", h.getCamera)
	mux.HandleFunc("PUT /api/cameras/{id}", h.updateCamera)
	mux.HandleFunc("DELETE /api/cameras/{id}", h.deleteCamera)
	mux.HandleFunc("GET /api/cameras/{id}/status", h.cameraStatus)
	mux.HandleFunc("GET /api/cameras/{id}/stream", h.cameraStream)
	mux.HandleFunc("POST /api/cameras/{id}/webrtc", h.cameraWebRTC)
	// AI metadata: ingest (agent -> server) and the read side the UI polls.
	mux.HandleFunc("POST /api/metadata", h.ingest)
	mux.HandleFunc("GET /api/metadata", h.listMetadata)
	mux.HandleFunc("GET /api/cameras/{id}/metadata", h.cameraMetadata)
	// AI mode selection (web UI -> persisted -> polled by the agent):
	//   GET  returns the last desired mode (default ai-y)
	//   POST sets it (validated: ai-off | ai-y | ai-y-pose | ai-hand)
	mux.HandleFunc("GET /api/cameras/{id}/aimode", h.getAIMode)
	mux.HandleFunc("POST /api/cameras/{id}/aimode", h.setAIMode)
	// Hand recorder read side (P2). The recorder itself is append-only and lives
	// in the ingest path; these three only read or clear it.
	//   GET    sessions            -> the index the export indexes into
	//   GET    sessions/{index}    -> one session, as hand_session_*.json
	//   DELETE log                 -> start a fresh calibration run
	mux.HandleFunc("GET /api/cameras/{id}/hand/sessions", h.listHandSessions)
	mux.HandleFunc("GET /api/cameras/{id}/hand/sessions/{index}", h.getHandSession)
	mux.HandleFunc("DELETE /api/cameras/{id}/hand/log", h.clearHandLog)
	// Same-origin HLS fallback (see hls.go for why it is proxied rather than
	// letting the browser hit MediaMTX's HLS port directly).
	mux.HandleFunc("GET /hls/{stream}", h.hlsProxy)
	mux.HandleFunc("GET /hls/{stream}/{file...}", h.hlsProxy)
}
