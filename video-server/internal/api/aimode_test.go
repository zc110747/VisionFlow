package api

import (
	"bytes"
	"database/sql"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"video-server/internal/metadata"

	_ "modernc.org/sqlite"
)

// newTestHandler wires a real in-memory file DB (so each connection sees the
// same schema) and only the metadata repo - the aimode handlers touch nothing
// else, so cfg/repo/mtx can stay nil.
func newTestHandler(t *testing.T) *Handler {
	t.Helper()
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "api.db")+"?_pragma=busy_timeout(5000)")
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	t.Cleanup(func() { db.Close() })
	db.SetMaxOpenConns(1)
	if err := metadata.Migrate(db); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	meta := metadata.NewRepository(db, 0, 1)
	return New(nil, nil, nil, db, meta)
}

// GET before any POST returns the default mode the agent should boot into.
func TestAIModeGetDefault(t *testing.T) {
	h := newTestHandler(t)
	mux := http.NewServeMux()
	h.Register(mux)

	req := httptest.NewRequest(http.MethodGet, "/api/cameras/camera01/aimode", nil)
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body=%s)", w.Code, w.Body.String())
	}
	var body struct {
		Mode string `json:"mode"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if body.Mode != metadata.DefaultAIMode {
		t.Errorf("mode = %q, want default %q", body.Mode, metadata.DefaultAIMode)
	}
}

// POST then GET confirms the mode persists and is echoed back, exactly what the
// agent poller will read to swap its detector.
func TestAIModeSetThenGet(t *testing.T) {
	h := newTestHandler(t)
	mux := http.NewServeMux()
	h.Register(mux)

	postBody, _ := json.Marshal(map[string]string{"mode": metadata.AIModePose})
	req := httptest.NewRequest(http.MethodPost, "/api/cameras/camera01/aimode", bytes.NewReader(postBody))
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Fatalf("POST status = %d (body=%s)", w.Code, w.Body.String())
	}

	req2 := httptest.NewRequest(http.MethodGet, "/api/cameras/camera01/aimode", nil)
	w2 := httptest.NewRecorder()
	mux.ServeHTTP(w2, req2)
	if w2.Code != http.StatusOK {
		t.Fatalf("GET status = %d", w2.Code)
	}
	var body struct {
		Mode string `json:"mode"`
	}
	json.Unmarshal(w2.Body.Bytes(), &body)
	if body.Mode != metadata.AIModePose {
		t.Errorf("mode = %q, want %q", body.Mode, metadata.AIModePose)
	}
}

// An invalid mode must be rejected with 400 and must not change the stored
// value (the agent poller only understands the three known modes).
func TestAIModeSetInvalidRejected(t *testing.T) {
	h := newTestHandler(t)
	mux := http.NewServeMux()
	h.Register(mux)

	postBody, _ := json.Marshal(map[string]string{"mode": "ai-bogus"})
	req := httptest.NewRequest(http.MethodPost, "/api/cameras/camera01/aimode", bytes.NewReader(postBody))
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400 (body=%s)", w.Code, w.Body.String())
	}

	// Stored value must remain the default.
	req2 := httptest.NewRequest(http.MethodGet, "/api/cameras/camera01/aimode", nil)
	w2 := httptest.NewRecorder()
	mux.ServeHTTP(w2, req2)
	var body struct {
		Mode string `json:"mode"`
	}
	json.Unmarshal(w2.Body.Bytes(), &body)
	if body.Mode != metadata.DefaultAIMode {
		t.Errorf("mode after rejected POST = %q, want default %q", body.Mode, metadata.DefaultAIMode)
	}
}

// Missing mode field is also a 400, not a 500.
func TestAIModeSetMissingBody(t *testing.T) {
	h := newTestHandler(t)
	mux := http.NewServeMux()
	h.Register(mux)

	req := httptest.NewRequest(http.MethodPost, "/api/cameras/camera01/aimode", bytes.NewReader([]byte(`{}`)))
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, req)
	if w.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400", w.Code)
	}
}
