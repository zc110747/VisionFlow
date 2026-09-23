package metadata

import (
	"database/sql"
	"encoding/json"
	"path/filepath"
	"testing"
	"time"

	_ "modernc.org/sqlite"
)

// newTestRepo opens a throwaway file-backed SQLite DB.
//
// A FILE (not ":memory:") is used on purpose: database/sql keeps a connection
// pool, and with an in-memory DSN each new connection would get its own empty
// database, making the tests pass for the wrong reason.
func newTestRepo(t *testing.T, handLogCap int) *Repository {
	t.Helper()
	return newTestRepoOpts(t, Options{HandLogMaxRows: handLogCap, HandLogPruneEvery: 1})
}

// newTestRepoOpts is newTestRepo for tests that need to vary the recorder
// options rather than just its cap.
func newTestRepoOpts(t *testing.T, opts Options) *Repository {
	t.Helper()
	if opts.HandLogPruneEvery == 0 {
		opts.HandLogPruneEvery = 1
	}
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "test.db")+"?_pragma=busy_timeout(5000)")
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	t.Cleanup(func() { db.Close() })
	// One connection: SQLite DDL/DML in these tests is sequential anyway, and
	// it removes any chance of a lock contention flake.
	db.SetMaxOpenConns(1)
	if err := Migrate(db); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	// pruneEvery=1 so pruning behaviour is exercised deterministically.
	//
	// The first argument used to bound ai_object history; that history is gone
	// (ai_object holds only the latest frame now), so it bounds ai_hand_log
	// instead. Every pre-existing caller passes 0, which means "recorder off" -
	// the same thing it meant before, so those tests keep testing what they
	// were written to test.
	return NewRepository(db, opts)
}

func sampleFrame(cam string, frameID int64) *FrameMessage {
	return &FrameMessage{
		Version:     1,
		Type:        TypeFrame,
		CameraID:    cam,
		FrameID:     frameID,
		Timestamp:   1756773210123,
		VideoWidth:  1280,
		VideoHeight: 720,
		Objects: []Object{
			{Class: "person", Confidence: 0.93, TrackID: 17, BBox: [4]int{812, 210, 1040, 850}},
			{Class: "person", Confidence: 0.74, TrackID: 3, BBox: [4]int{10, 20, 100, 200}},
		},
	}
}

// The agent contract that matters most: frame_id / timestamp are produced by
// the CAPTURE side and must survive the round trip untouched. Any renumbering
// here would silently break frame-accurate correlation on the server.
func TestSaveFrameStoresFrameIdentityVerbatim(t *testing.T) {
	r := newTestRepo(t, 0)
	f := sampleFrame("camera01", 15230)
	if err := r.SaveFrame(f, time.Unix(1700000000, 0).UTC()); err != nil {
		t.Fatalf("SaveFrame: %v", err)
	}
	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if snap.Frame == nil {
		t.Fatal("frame missing from snapshot")
	}
	if snap.Frame.FrameID != 15230 {
		t.Errorf("frame_id = %d, want 15230 (must be stored verbatim)", snap.Frame.FrameID)
	}
	if snap.Frame.Timestamp != 1756773210123 {
		t.Errorf("timestamp = %d, want 1756773210123", snap.Frame.Timestamp)
	}
	if snap.Frame.VideoWidth != 1280 || snap.Frame.VideoHeight != 720 {
		t.Errorf("video size = %dx%d, want 1280x720", snap.Frame.VideoWidth, snap.Frame.VideoHeight)
	}
	if len(snap.Frame.Objects) != 2 {
		t.Fatalf("objects = %d, want 2", len(snap.Frame.Objects))
	}
	if snap.Frame.Objects[0].TrackID != 17 {
		t.Errorf("track_id = %d, want 17", snap.Frame.Objects[0].TrackID)
	}
	if snap.Frame.Objects[0].BBox != [4]int{812, 210, 1040, 850} {
		t.Errorf("bbox = %v, want [812 210 1040 850]", snap.Frame.Objects[0].BBox)
	}
}

// ai_frame holds one row per camera: the newest result replaces the old one, so
// the UI always reads "latest" without scanning history.
func TestFrameIsUpsertedPerCamera(t *testing.T) {
	r := newTestRepo(t, 0)
	if err := r.SaveFrame(sampleFrame("camera01", 1), time.Now()); err != nil {
		t.Fatalf("save 1: %v", err)
	}
	next := sampleFrame("camera01", 2)
	next.Objects = nil
	if err := r.SaveFrame(next, time.Now()); err != nil {
		t.Fatalf("save 2: %v", err)
	}
	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if snap.Frame.FrameID != 2 {
		t.Errorf("frame_id = %d, want 2 (latest must win)", snap.Frame.FrameID)
	}
	// Objects of the OLD frame must not be reported as part of the new one.
	if len(snap.Frame.Objects) != 0 {
		t.Errorf("objects = %d, want 0 (objects are looked up by frame_id)", len(snap.Frame.Objects))
	}
}

func TestSaveStatusStoresHeartbeat(t *testing.T) {
	r := newTestRepo(t, 0)
	s := &StatusMessage{
		Version:   1,
		Type:      TypeStatus,
		CameraID:  "camera01",
		WallClock: 1756773215000,
		AI: AIState{
			Enable: true, Running: true, FPS: 5.0,
			Model: "models/yolov8n.onnx", Tracker: "bytetrack",
			LastFrameID: 15230, LastTimestamp: 1756773210123, Processed: 99,
		},
	}
	if err := r.SaveStatus(s, time.Now()); err != nil {
		t.Fatalf("SaveStatus: %v", err)
	}
	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if snap.Status == nil {
		t.Fatal("status missing from snapshot")
	}
	got := snap.Status
	if !got.Enable || !got.Running {
		t.Errorf("enable/running = %v/%v, want true/true", got.Enable, got.Running)
	}
	if got.FPS != 5.0 || got.Processed != 99 {
		t.Errorf("fps/processed = %.2f/%d, want 5.00/99", got.FPS, got.Processed)
	}
	if got.Model != "models/yolov8n.onnx" || got.Tracker != "bytetrack" {
		t.Errorf("model/tracker = %q/%q", got.Model, got.Tracker)
	}
	if got.LastFrameID != 15230 {
		t.Errorf("last_frame_id = %d, want 15230", got.LastFrameID)
	}
	if snap.Frame != nil {
		t.Error("frame must be nil when only a heartbeat was received")
	}
}

// Latest must be callable before anything arrives: the UI polls it from first
// paint, and a 500 there would be indistinguishable from a broken server.
func TestLatestOnUnknownCameraIsEmptyNotError(t *testing.T) {
	r := newTestRepo(t, 0)
	snap, err := r.Latest("nope")
	if err != nil {
		t.Fatalf("Latest on unknown camera: %v", err)
	}
	if snap.Frame != nil || snap.Status != nil {
		t.Error("expected an empty snapshot for an unknown camera")
	}
}

// ai_object holds ONLY the latest frame's objects per camera (deleted and
// re-inserted on every SaveFrame). This test locks in that contract: a long
// stream must never accumulate history, and the newest frame must always win.
func TestAObjectHoldsOnlyLatestFrame(t *testing.T) {
	r := newTestRepo(t, 0)
	for i := 0; i < 20; i++ {
		f := sampleFrame("camera01", int64(i))
		f.Objects = f.Objects[:1] // one object per frame keeps the count trivial
		if err := r.SaveFrame(f, time.Now()); err != nil {
			t.Fatalf("save %d: %v", i, err)
		}
	}
	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if len(snap.Frame.Objects) != 1 {
		t.Fatalf("objects = %d, want 1 (only the latest frame survives)", len(snap.Frame.Objects))
	}
	if snap.Frame.FrameID != 19 {
		t.Errorf("frame_id = %d, want 19 (latest must win)", snap.Frame.FrameID)
	}
	// DB must hold exactly one frame's worth of objects, not 20 x 1.
	var n int
	if err := r.db.QueryRow(`SELECT COUNT(*) FROM ai_object WHERE camera_id='camera01'`).Scan(&n); err != nil {
		t.Fatalf("count: %v", err)
	}
	if n != 1 {
		t.Errorf("rows = %d, want 1 (no history retained)", n)
	}
}

// REGRESSION: the agent's frame_id counter resets across restarts, so the same
// frame_id can refer to different runs. In the old append-only design, a pose
// run's keypoints (frame_id 4179 in ai-y-pose) leaked onto a later
// person-detect run sharing that same frame_id (4179 in ai-y) - the web then
// drew skeletons while in detect-only mode.
//
// ai_object now holds ONLY the latest frame's objects (delete + insert), so the
// second run must fully replace the first's objects with zero keypoints.
func TestPoseObjectsDoNotLeakIntoDetectMode(t *testing.T) {
	r := newTestRepo(t, 0)

	// Run #1: pose mode, 2 people WITH 17 COCO keypoints each.
	pose := &FrameMessage{
		Version: 1, Type: TypeFrame, CameraID: "camera01",
		FrameID: 4179, Timestamp: 1756773210123,
		VideoWidth: 1280, VideoHeight: 720, AIMode: AIModePose,
		Objects: []Object{
			{Class: "person", Confidence: 0.9, TrackID: 1, BBox: [4]int{10, 20, 100, 200}, Keypoints: samplePose()},
			{Class: "person", Confidence: 0.8, TrackID: 2, BBox: [4]int{110, 120, 200, 300}, Keypoints: samplePose()},
		},
	}
	if err := r.SaveFrame(pose, time.Now()); err != nil {
		t.Fatalf("save pose run: %v", err)
	}

	// Run #2: agent restarts (frame_id counter resets to the SAME value),
	// mode is now person-detection only (no pose head -> no keypoints emitted).
	detect := &FrameMessage{
		Version: 1, Type: TypeFrame, CameraID: "camera01",
		FrameID: 4179, Timestamp: 1756773215000,
		VideoWidth: 1280, VideoHeight: 720, AIMode: AIModeDetect,
		Objects: []Object{
			{Class: "person", Confidence: 0.95, TrackID: 5, BBox: [4]int{40, 50, 140, 240}},
		},
	}
	if err := r.SaveFrame(detect, time.Now()); err != nil {
		t.Fatalf("save detect run: %v", err)
	}

	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if snap.Frame == nil {
		t.Fatal("frame missing from snapshot")
	}
	if snap.Frame.AIMode != AIModeDetect {
		t.Errorf("ai_mode = %q, want %q", snap.Frame.AIMode, AIModeDetect)
	}
	if len(snap.Frame.Objects) != 1 {
		t.Fatalf("objects = %d, want 1 (pose run's objects must be gone)", len(snap.Frame.Objects))
	}
	if len(snap.Frame.Objects[0].Keypoints) != 0 {
		t.Errorf("detect-mode object carries %d keypoints (pose leak)", len(snap.Frame.Objects[0].Keypoints))
	}
	// The DB must hold exactly the one detect-mode object, nothing from pose.
	var n int
	if err := r.db.QueryRow(`SELECT COUNT(*) FROM ai_object WHERE camera_id='camera01'`).Scan(&n); err != nil {
		t.Fatalf("count: %v", err)
	}
	if n != 1 {
		t.Errorf("rows = %d, want 1 (pose rows must be purged)", n)
	}
}

// samplePose returns 17 COCO keypoints ([x,y,conf] each), the marker of a pose
// model's output.
func samplePose() [][3]float64 {
	kp := make([][3]float64, 17)
	for i := range kp {
		kp[i] = [3]float64{float64(i * 10), float64(i * 8), 0.9}
	}
	return kp
}

func TestListAndCameraIDs(t *testing.T) {
	r := newTestRepo(t, 0)
	if err := r.SaveFrame(sampleFrame("camB", 1), time.Now()); err != nil {
		t.Fatalf("save camB: %v", err)
	}
	if err := r.SaveFrame(sampleFrame("camA", 1), time.Now()); err != nil {
		t.Fatalf("save camA: %v", err)
	}
	ids, err := r.CameraIDs()
	if err != nil {
		t.Fatalf("CameraIDs: %v", err)
	}
	if len(ids) != 2 {
		t.Fatalf("ids = %v, want 2 entries", ids)
	}
	snaps, err := r.List()
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(snaps) != 2 {
		t.Fatalf("snapshots = %d, want 2", len(snaps))
	}
}

func TestDeleteCameraRemovesAllMetadata(t *testing.T) {
	r := newTestRepo(t, 0)
	if err := r.SaveFrame(sampleFrame("camera01", 1), time.Now()); err != nil {
		t.Fatalf("save: %v", err)
	}
	if err := r.DeleteCamera("camera01"); err != nil {
		t.Fatalf("DeleteCamera: %v", err)
	}
	ids, err := r.CameraIDs()
	if err != nil {
		t.Fatalf("CameraIDs: %v", err)
	}
	if len(ids) != 0 {
		t.Errorf("ids after delete = %v, want empty", ids)
	}
}

// --------------------------------------------------------------------------
// Wire format: these are the exact payloads the C++ agent produces.
// --------------------------------------------------------------------------

func TestDecodeAgentFramePayload(t *testing.T) {
	raw := `{"version":1,"type":"frame","camera_id":"camera01","frame_id":15230,
	         "timestamp":1756773210123,"video_width":1280,"video_height":720,
	         "objects":[{"class":"person","confidence":0.93,"track_id":17,
	                     "bbox":[812,210,1040,850]}]}`
	var probe Probe
	if err := json.Unmarshal([]byte(raw), &probe); err != nil {
		t.Fatalf("probe: %v", err)
	}
	if probe.Type != TypeFrame {
		t.Fatalf("type = %q, want frame", probe.Type)
	}
	var f FrameMessage
	if err := json.Unmarshal([]byte(raw), &f); err != nil {
		t.Fatalf("decode frame: %v", err)
	}
	if f.FrameID != 15230 || f.Timestamp != 1756773210123 {
		t.Errorf("frame identity = %d/%d", f.FrameID, f.Timestamp)
	}
	if len(f.Objects) != 1 || f.Objects[0].BBox != [4]int{812, 210, 1040, 850} {
		t.Errorf("objects decoded wrong: %+v", f.Objects)
	}
}

func TestDecodeAgentFramePayloadWithKeypoints(t *testing.T) {
	raw := `{"version":1,"type":"frame","camera_id":"camera01","frame_id":15230,
	         "timestamp":1756773210123,"video_width":1280,"video_height":720,
	         "objects":[{"class":"person","confidence":0.93,"track_id":17,
	                     "bbox":[812,210,1040,850],
	                     "keypoints":[[812,210,0.95],[900,400,0.88],[1040,850,0.71]]}]}`
	var f FrameMessage
	if err := json.Unmarshal([]byte(raw), &f); err != nil {
		t.Fatalf("decode frame: %v", err)
	}
	if len(f.Objects) != 1 {
		t.Fatalf("objects = %d, want 1", len(f.Objects))
	}
	o := f.Objects[0]
	if len(o.Keypoints) != 3 {
		t.Fatalf("keypoints = %d, want 3", len(o.Keypoints))
	}
	if o.Keypoints[0] != ([3]float64{812, 210, 0.95}) {
		t.Errorf("keypoint[0] = %v, want [812 210 0.95]", o.Keypoints[0])
	}
	if o.Keypoints[2] != ([3]float64{1040, 850, 0.71}) {
		t.Errorf("keypoint[2] = %v, want [1040 850 0.71]", o.Keypoints[2])
	}
}

func TestDecodeAgentStatusPayload(t *testing.T) {
	raw := `{"version":1,"type":"status","camera_id":"camera01","wall_clock":1756773215000,
	         "ai":{"enable":true,"running":true,"fps":5.00,
	               "model":"models/yolov8n.onnx","tracker":"bytetrack",
	               "last_frame_id":15230,"last_timestamp":1756773210123,"processed":99}}`
	var s StatusMessage
	if err := json.Unmarshal([]byte(raw), &s); err != nil {
		t.Fatalf("decode status: %v", err)
	}
	if s.Type != TypeStatus {
		t.Errorf("type = %q, want status", s.Type)
	}
	if !s.AI.Enable || !s.AI.Running || s.AI.Processed != 99 {
		t.Errorf("ai state decoded wrong: %+v", s.AI)
	}
}

// An empty objects array is meaningful: it tells the server the AI is alive but
// saw nothing. It must be preserved, not normalised away into "no frame".
func TestEmptyObjectsArrayKeepsFrame(t *testing.T) {
	raw := `{"version":1,"type":"frame","camera_id":"c1","frame_id":9,"timestamp":1,
	         "video_width":640,"video_height":480,"objects":[]}`
	var f FrameMessage
	if err := json.Unmarshal([]byte(raw), &f); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if err := f.Validate(); err != nil {
		t.Fatalf("validate: %v", err)
	}
	if dropped := f.Normalize(); dropped != 0 {
		t.Errorf("dropped = %d, want 0", dropped)
	}
	if f.Objects == nil || len(f.Objects) != 0 {
		t.Errorf("objects = %v, want empty non-nil slice", f.Objects)
	}
}

// --------------------------------------------------------------------------
// Defensive normalisation (untrusted input)
// --------------------------------------------------------------------------

func TestNormalizeClampsBoxToFrame(t *testing.T) {
	cases := []struct {
		name string
		in   [4]int
		want [4]int
	}{
		{"already valid", [4]int{10, 20, 100, 200}, [4]int{10, 20, 100, 200}},
		{"negative origin", [4]int{-50, -50, 100, 200}, [4]int{0, 0, 100, 200}},
		{"overflow bottom-right", [4]int{100, 100, 9999, 9999}, [4]int{100, 100, 1280, 720}},
		{"reversed corners", [4]int{300, 300, 100, 100}, [4]int{100, 100, 300, 300}},
		{"degenerate widened", [4]int{50, 50, 50, 50}, [4]int{50, 50, 51, 51}},
		{"clamped then widened", [4]int{1280, 720, 1280, 720}, [4]int{1279, 719, 1280, 720}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			b := tc.in
			if !normalizeBox(&b, 1280, 720) {
				t.Fatalf("normalizeBox rejected %v", tc.in)
			}
			if b != tc.want {
				t.Errorf("got %v, want %v", b, tc.want)
			}
		})
	}
}

func TestNormalizeDropsObjectsOnZeroSizedFrame(t *testing.T) {
	f := &FrameMessage{CameraID: "c1", VideoWidth: 0, VideoHeight: 0,
		Objects: []Object{{Class: "person", BBox: [4]int{1, 2, 3, 4}}}}
	if dropped := f.Normalize(); dropped != 1 {
		t.Errorf("dropped = %d, want 1 (no valid box exists in a 0x0 frame)", dropped)
	}
	if len(f.Objects) != 0 {
		t.Errorf("objects = %v, want empty", f.Objects)
	}
}

func TestNormalizeClampsConfidence(t *testing.T) {
	f := &FrameMessage{CameraID: "c1", VideoWidth: 640, VideoHeight: 480,
		Objects: []Object{
			{Class: "person", Confidence: 5.0, BBox: [4]int{1, 2, 3, 4}},
			{Class: "person", Confidence: -1, BBox: [4]int{1, 2, 3, 4}},
		}}
	f.Normalize()
	if f.Objects[0].Confidence != 1 {
		t.Errorf("confidence = %v, want 1", f.Objects[0].Confidence)
	}
	if f.Objects[1].Confidence != 0 {
		t.Errorf("confidence = %v, want 0", f.Objects[1].Confidence)
	}
}

func TestValidateRejectsIncompleteMessages(t *testing.T) {
	if err := (&FrameMessage{Type: TypeFrame, VideoWidth: 640, VideoHeight: 480}).Validate(); err == nil {
		t.Error("frame without camera_id must be rejected")
	}
	if err := (&FrameMessage{CameraID: "c1", VideoWidth: 640}).Validate(); err == nil {
		t.Error("frame without height must be rejected")
	}
	if err := (&StatusMessage{Type: TypeStatus}).Validate(); err == nil {
		t.Error("status without camera_id must be rejected")
	}
}

// A frame arriving for a camera must never disturb another camera's snapshot.
func TestCamerasAreIsolated(t *testing.T) {
	r := newTestRepo(t, 0)
	if err := r.SaveFrame(sampleFrame("camA", 7), time.Now()); err != nil {
		t.Fatalf("save camA: %v", err)
	}
	if err := r.SaveFrame(sampleFrame("camB", 9), time.Now()); err != nil {
		t.Fatalf("save camB: %v", err)
	}
	a, _ := r.Latest("camA")
	b, _ := r.Latest("camB")
	if a.Frame.FrameID != 7 || b.Frame.FrameID != 9 {
		t.Errorf("cross-camera bleed: camA=%d camB=%d, want 7 and 9", a.Frame.FrameID, b.Frame.FrameID)
	}
}

// InferType exists for agents built before "type" became mandatory. The
// regression it guards is real: a frame with zero objects has an empty
// "objects" array, and json.Unmarshal into a probe sees no discriminating
// field at all.
func TestInferTypeFromPayloadShape(t *testing.T) {
	// Verbatim capture from an agent build that omitted the frame
	// discriminator. It has no "type", and "objects" is empty - only the
	// presence of frame_id distinguishes it from a malformed heartbeat.
	legacyFrame := []byte(`{"version":1,"camera_id":"camtest","frame_id":6,"timestamp":196,` +
		`"video_width":1280,"video_height":720,"objects":[]}`)
	status := []byte(`{"version":1,"type":"status","camera_id":"camtest","wall_clock":0,` +
		`"ai":{"enable":true,"running":true,"fps":0}}`)
	modern := []byte(`{"version":1,"type":"frame","camera_id":"camtest","frame_id":1,` +
		`"timestamp":2,"video_width":8,"video_height":8,"objects":[]}`)

	cases := []struct {
		name string
		body []byte
		want string
	}{
		{"legacy_frame_no_type", legacyFrame, TypeFrame},
		{"legacy_frame_with_object", []byte(`{"objects":[{"class":"person"}]}`), TypeFrame},
		{"status_has_ai_object", status, TypeStatus},
		{"modern_frame_uses_declared_type", modern, TypeFrame},
		{"empty_object_is_not_shape", []byte(`{}`), ""},
		{"broken_json_is_not_shape", []byte(`{"objects":`), ""},
		{"unknown_shape_is_not_guessed", []byte(`{"version":1,"camera_id":"x"}`), ""},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := InferType(c.body); got != c.want {
				t.Errorf("InferType = %q, want %q", got, c.want)
			}
		})
	}
}
