package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"video-server/internal/camera"
	"video-server/internal/config"
	"video-server/internal/database"
	"video-server/internal/metadata"
)

// newHandHandler wires everything the hand endpoints touch. Unlike the aimode
// handlers these read h.cfg (the session gap default lives there) and h.repo
// (a session list is a 404 for a camera nobody knows), so cfg and the camera
// repo cannot be nil here.
func newHandHandler(t *testing.T, opts metadata.Options) *Handler {
	t.Helper()
	db, err := database.Open(filepath.Join(t.TempDir(), "hand.db"))
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	t.Cleanup(func() { db.Close() })
	// One connection: the test drives ingest and reads sequentially, and it
	// removes any chance of a lock-contention flake.
	db.SetMaxOpenConns(1)

	cams := camera.NewRepository(db)
	if err := cams.Create(camera.Camera{ID: "camera01", StreamPath: "camera01"}); err != nil {
		t.Fatalf("create camera: %v", err)
	}
	cfg := config.Default()
	cfg.Metadata.HandLogMaxRows = opts.HandLogMaxRows
	cfg.Metadata.HandLogPruneEvery = 1
	cfg.Metadata.HandLogDetectedOnly = opts.HandLogDetectedOnly
	cfg.Metadata.HandSessionGapMS = 1000
	return New(cfg, cams, nil, db, metadata.NewRepository(db, metadata.Options{
		HandLogMaxRows:      cfg.Metadata.HandLogMaxRows,
		HandLogPruneEvery:   cfg.Metadata.HandLogPruneEvery,
		HandLogDetectedOnly: cfg.Metadata.HandLogDetectedOnly,
	}))
}

// handFrameBody is the payload the agent actually posts for an ai-hand frame.
func handFrameBody(cam string, frameID, ts int64, detected bool, gesture string) string {
	return `{"version":1,"type":"frame","camera_id":"` + cam + `",` +
		`"frame_id":` + strconv.FormatInt(frameID, 10) + `,` +
		`"timestamp":` + strconv.FormatInt(ts, 10) + `,` +
		`"video_width":1280,"video_height":720,"ai_mode":"ai-hand",` +
		`"objects":[],"hand":{"detected":` + strconv.FormatBool(detected) + `,` +
		`"confidence":0.93,"handedness":"right","reference_initialized":true,` +
		`"position":[0.5213,0.4123,-0.1234],"reference":[0.5,0.4,-0.1],` +
		`"delta":[0.0213,0.0123,-0.0234],` +
		`"motion":{"x":"LEFT","y":"FORWARD","z":"UP"},` +
		`"gesture":"` + gesture + `","gesture_candidate":"` + gesture + `",` +
		`"gesture_confidence":0.75}}`
}

// serve drives one request through the real router, so path params and method
// matching are exercised rather than hand-set.
func serve(t *testing.T, h *Handler, method, target, body string) *httptest.ResponseRecorder {
	t.Helper()
	mux := http.NewServeMux()
	h.Register(mux)
	var r *http.Request
	if body == "" {
		r = httptest.NewRequest(method, target, nil)
	} else {
		r = httptest.NewRequest(method, target, strings.NewReader(body))
		r.Header.Set("Content-Type", "application/json")
	}
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, r)
	return w
}

func mustIngest(t *testing.T, h *Handler, body string) {
	t.Helper()
	if w := serve(t, h, http.MethodPost, "/api/metadata", body); w.Code != http.StatusNoContent {
		t.Fatalf("ingest status = %d, want 204 (body=%s)", w.Code, w.Body.String())
	}
}

// ---------------------------------------------------------------------------
// ai-hand is accepted end to end
// ---------------------------------------------------------------------------

// Before this, the agent's aimode poller overrode its own --ai-mode hand flag
// with the stored ai-y within about a second, because the server refused to
// store ai-hand at all. Hand mode on the real rig was impossible.
func TestAIModeAcceptsHand(t *testing.T) {
	h := newHandHandler(t, metadata.Options{HandLogMaxRows: 100})

	w := serve(t, h, http.MethodPost, "/api/cameras/camera01/aimode", `{"mode":"ai-hand"}`)
	if w.Code != http.StatusOK {
		t.Fatalf("POST ai-hand status = %d, want 200 (body=%s)", w.Code, w.Body.String())
	}
	w = serve(t, h, http.MethodGet, "/api/cameras/camera01/aimode", "")
	var body struct {
		Mode string `json:"mode"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if body.Mode != metadata.AIModeHand {
		t.Errorf("stored mode = %q, want %q", body.Mode, metadata.AIModeHand)
	}
	// The agent must still be able to get back out of hand mode.
	if w := serve(t, h, http.MethodPost, "/api/cameras/camera01/aimode", `{"mode":"ai-y"}`); w.Code != http.StatusOK {
		t.Errorf("POST ai-y after hand status = %d, want 200", w.Code)
	}
}

func TestMetadataIngestCarriesHandBlockThrough(t *testing.T) {
	h := newHandHandler(t, metadata.Options{HandLogMaxRows: 100})
	mustIngest(t, h, handFrameBody("camera01", 660, 12345, true, metadata.GestureFist))

	w := serve(t, h, http.MethodGet, "/api/cameras/camera01/metadata", "")
	if w.Code != http.StatusOK {
		t.Fatalf("metadata status = %d, want 200 (body=%s)", w.Code, w.Body.String())
	}
	var snap struct {
		Frame struct {
			AIMode string `json:"ai_mode"`
			Hand   *struct {
				Detected bool       `json:"detected"`
				Gesture  string     `json:"gesture"`
				Position [3]float64 `json:"position"`
				Motion   struct {
					Y string `json:"y"`
				} `json:"motion"`
			} `json:"hand"`
		} `json:"frame"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &snap); err != nil {
		t.Fatalf("decode snapshot: %v", err)
	}
	if snap.Frame.AIMode != metadata.AIModeHand {
		t.Errorf("ai_mode = %q, want %q", snap.Frame.AIMode, metadata.AIModeHand)
	}
	if snap.Frame.Hand == nil {
		t.Fatal("hand missing from GET /metadata")
	}
	if !snap.Frame.Hand.Detected || snap.Frame.Hand.Gesture != metadata.GestureFist {
		t.Errorf("hand = %+v, want detected FIST", snap.Frame.Hand)
	}
	if snap.Frame.Hand.Motion.Y != "FORWARD" {
		t.Errorf("motion.y = %q, want FORWARD (the logical forward axis)", snap.Frame.Hand.Motion.Y)
	}
	if snap.Frame.Hand.Position[2] != -0.1234 {
		t.Errorf("position.z = %v, want -0.1234", snap.Frame.Hand.Position[2])
	}
}

// A detect / pose frame must not grow a hand block on the way out either.
func TestMetadataIngestLeavesDetectFramesUnchanged(t *testing.T) {
	h := newHandHandler(t, metadata.Options{HandLogMaxRows: 100})
	body := `{"version":1,"type":"frame","camera_id":"camera01","frame_id":1,"timestamp":1,` +
		`"video_width":1280,"video_height":720,"ai_mode":"ai-y",` +
		`"objects":[{"class":"person","confidence":0.9,"track_id":1,"bbox":[10,20,110,220]}]}`
	mustIngest(t, h, body)

	w := serve(t, h, http.MethodGet, "/api/cameras/camera01/metadata", "")
	if strings.Contains(w.Body.String(), `"hand"`) {
		t.Errorf("a detect frame grew a hand key: %s", w.Body.String())
	}
}

// ---------------------------------------------------------------------------
// Sessions / export / clear
// ---------------------------------------------------------------------------

func seedTwoSessions(t *testing.T, h *Handler) {
	t.Helper()
	for i, id := range []int64{660, 661, 662, 663} {
		mustIngest(t, h, handFrameBody("camera01", id, 1000+int64(i)*125, true, metadata.GestureFist))
	}
	mustIngest(t, h, handFrameBody("camera01", 664, 1500, false, metadata.GestureUnknown))
	mustIngest(t, h, handFrameBody("camera01", 700, 5000, true, metadata.GestureOpenPalm))
	mustIngest(t, h, handFrameBody("camera01", 701, 5125, true, metadata.GestureOpenPalm))
}

func TestHandSessionsEndpointListsSplits(t *testing.T) {
	h := newHandHandler(t, metadata.Options{HandLogMaxRows: 100})
	seedTwoSessions(t, h)

	w := serve(t, h, http.MethodGet, "/api/cameras/camera01/hand/sessions", "")
	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body=%s)", w.Code, w.Body.String())
	}
	var list metadata.HandSessionList
	if err := json.Unmarshal(w.Body.Bytes(), &list); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if list.TotalRows != 7 {
		t.Errorf("total_rows = %d, want 7", list.TotalRows)
	}
	if len(list.Sessions) != 2 {
		t.Fatalf("sessions = %d, want 2", len(list.Sessions))
	}
	if list.Sessions[0].FrameCount != 4 || list.Sessions[1].FrameCount != 2 {
		t.Errorf("frame counts = %d,%d want 4,2", list.Sessions[0].FrameCount, list.Sessions[1].FrameCount)
	}
	// The response echoes the split parameters, so the client can explain why it
	// saw this many sessions without knowing the server default.
	if list.GapMS != 1000 {
		t.Errorf("gap_ms = %d, want the configured 1000", list.GapMS)
	}

	// A wider gap must NOT merge these two holds: frame 664 says the hand left
	// the frame, and that ends a hold whatever the clock says. The gap only
	// decides whether rows that are ALL "detected" are close enough to be one
	// hold, so it can never revive a hold the camera watched end.
	w = serve(t, h, http.MethodGet, "/api/cameras/camera01/hand/sessions?gap_ms=10000", "")
	if err := json.Unmarshal(w.Body.Bytes(), &list); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(list.Sessions) != 2 {
		t.Errorf("with gap_ms=10000 sessions = %d, want 2 (a %q frame ends a hold)", len(list.Sessions), "detected=false")
	}

	// Now the case the gap is actually for: the same two holds with nothing
	// recorded in between - the recorder skipped those frames, or
	// detected-only is on. Only the agent clock separates them, so the gap is
	// the sole arbiter and the two values must disagree.
	if w := serve(t, h, http.MethodDelete, "/api/cameras/camera01/hand/log", ""); w.Code != http.StatusOK {
		t.Fatalf("clear log status = %d, want 200", w.Code)
	}
	for i, id := range []int64{660, 661, 662, 663} {
		mustIngest(t, h, handFrameBody("camera01", id, 1000+int64(i)*125, true, metadata.GestureFist))
	}
	mustIngest(t, h, handFrameBody("camera01", 700, 5000, true, metadata.GestureOpenPalm))

	for _, tc := range []struct {
		gapMS int
		want  int
	}{{1000, 2}, {10000, 1}} {
		w := serve(t, h, http.MethodGet,
			"/api/cameras/camera01/hand/sessions?gap_ms="+strconv.Itoa(tc.gapMS), "")
		if w.Code != http.StatusOK {
			t.Fatalf("gap_ms=%d status = %d, want 200 (body=%s)", tc.gapMS, w.Code, w.Body.String())
		}
		if err := json.Unmarshal(w.Body.Bytes(), &list); err != nil {
			t.Fatalf("decode: %v", err)
		}
		if len(list.Sessions) != tc.want {
			t.Errorf("gap_ms=%d sessions = %d, want %d", tc.gapMS, len(list.Sessions), tc.want)
		}
		if list.GapMS != tc.gapMS {
			t.Errorf("gap_ms echo = %d, want %d", list.GapMS, tc.gapMS)
		}
	}

	w = serve(t, h, http.MethodGet, "/api/cameras/camera01/hand/sessions?gap_ms=abc", "")
	if w.Code != http.StatusBadRequest {
		t.Errorf("gap_ms=abc status = %d, want 400", w.Code)
	}
}

func TestHandSessionExportIsADownloadableFile(t *testing.T) {
	h := newHandHandler(t, metadata.Options{HandLogMaxRows: 100})
	seedTwoSessions(t, h)

	w := serve(t, h, http.MethodGet, "/api/cameras/camera01/hand/sessions/2", "")
	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body=%s)", w.Code, w.Body.String())
	}
	// The name the browser will save is part of the contract: the task calls for
	// hand_session_*.json on disk.
	if cd := w.Header().Get("Content-Disposition"); !strings.Contains(cd, "hand_session_camera01_2.json") {
		t.Errorf("Content-Disposition = %q, want it to name hand_session_camera01_2.json", cd)
	}

	var file metadata.HandSessionFile
	if err := json.Unmarshal(w.Body.Bytes(), &file); err != nil {
		t.Fatalf("decode export: %v", err)
	}
	if file.Version != 1 {
		t.Errorf("version = %d, want 1", file.Version)
	}
	if file.CameraID != "camera01" || file.Session.Index != 2 {
		t.Errorf("file header = %+v, want camera01 session 2", file)
	}
	if len(file.Frames) != 2 {
		t.Fatalf("frames = %d, want 2", len(file.Frames))
	}
	if file.Frames[0].Hand == nil || file.Frames[0].Hand.Gesture != metadata.GestureOpenPalm {
		t.Errorf("exported frame 0 = %+v, want a hand block with OPEN_PALM", file.Frames[0])
	}
	// Keyed on the recorder's id, not on frame_id: the agent restarts frame_id
	// from 1 on every launch, so a reader sorting on it interleaves runs.
	if file.Frames[0].ID == 0 || file.Frames[0].ID >= file.Frames[1].ID {
		t.Errorf("exported frame ids = %d,%d, want non-zero and increasing",
			file.Frames[0].ID, file.Frames[1].ID)
	}
	if file.ExportedAt.IsZero() {
		t.Error("exported_at must be set so a saved file says when it was taken")
	}

	// Out-of-range and non-numeric indices are client errors, not 500s.
	for _, target := range []string{
		"/api/cameras/camera01/hand/sessions/3",
		"/api/cameras/camera01/hand/sessions/0",
		"/api/cameras/camera01/hand/sessions/abc",
	} {
		if w := serve(t, h, http.MethodGet, target, ""); w.Code != http.StatusNotFound && w.Code != http.StatusBadRequest {
			t.Errorf("%s status = %d, want 404 or 400", target, w.Code)
		}
	}
}

func TestHandSessionsUnknownCameraIs404(t *testing.T) {
	h := newHandHandler(t, metadata.Options{HandLogMaxRows: 100})
	w := serve(t, h, http.MethodGet, "/api/cameras/nope/hand/sessions", "")
	if w.Code != http.StatusNotFound {
		t.Errorf("status = %d, want 404 (an unknown camera is not an empty recorder)", w.Code)
	}
}

func TestClearHandLogEndpoint(t *testing.T) {
	h := newHandHandler(t, metadata.Options{HandLogMaxRows: 100})
	seedTwoSessions(t, h)

	w := serve(t, h, http.MethodDelete, "/api/cameras/camera01/hand/log", "")
	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body=%s)", w.Code, w.Body.String())
	}
	var res struct {
		Cleared int64 `json:"cleared"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &res); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if res.Cleared != 7 {
		t.Errorf("cleared = %d, want 7", res.Cleared)
	}
	w = serve(t, h, http.MethodGet, "/api/cameras/camera01/hand/sessions", "")
	var list metadata.HandSessionList
	if err := json.Unmarshal(w.Body.Bytes(), &list); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if list.TotalRows != 0 || len(list.Sessions) != 0 {
		t.Errorf("after clear: %d rows / %d sessions, want 0/0", list.TotalRows, len(list.Sessions))
	}
}

// The camera id comes from the URL path, so it must not be able to escape the
// directory the file lands in.
func TestHandSessionFilenameIsSanitised(t *testing.T) {
	cases := map[string]string{
		"camera01":      "hand_session_camera01_3.json",
		"../etc/passwd": "hand_session_.._etc_passwd_3.json",
		`cam\..\win`:    "hand_session_cam_.._win_3.json",
		"cam 01":        "hand_session_cam_01_3.json",
	}
	for in, want := range cases {
		if got := handSessionFilename(in, 3); got != want {
			t.Errorf("handSessionFilename(%q) = %q, want %q", in, got, want)
		}
		if strings.ContainsAny(handSessionFilename(in, 3), `/\`) {
			t.Errorf("handSessionFilename(%q) still contains a path separator", in)
		}
	}
}
