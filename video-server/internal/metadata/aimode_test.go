package metadata

import (
	"database/sql"
	"encoding/json"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Keypoints must survive the round trip: the agent emits [x,y,conf] per COCO
// joint for pose models, and the web UI draws them. Dropping the column would
// silently turn ai-y-pose into a detection-only view.
func TestSaveFrameRoundTripsKeypoints(t *testing.T) {
	r := newTestRepo(t, 0)
	f := sampleFrame("camera01", 15230)
	f.Objects = []Object{{
		Class: "person", Confidence: 0.91, TrackID: 7, BBox: [4]int{100, 200, 300, 500},
		Keypoints: [][3]float64{
			{110, 210, 0.95}, // nose
			{120, 240, 0.88}, // eye
			{0, 0, 0.01},     // low-conf ankle, kept verbatim
		},
	}}
	if err := r.SaveFrame(f, time.Unix(1700000000, 0).UTC()); err != nil {
		t.Fatalf("SaveFrame: %v", err)
	}
	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if len(snap.Frame.Objects) != 1 {
		t.Fatalf("objects = %d, want 1", len(snap.Frame.Objects))
	}
	kp := snap.Frame.Objects[0].Keypoints
	if len(kp) != 3 {
		t.Fatalf("keypoints = %d, want 3", len(kp))
	}
	if kp[0] != [3]float64{110, 210, 0.95} {
		t.Errorf("kp[0] = %v, want [110 210 0.95]", kp[0])
	}
	if kp[2] != [3]float64{0, 0, 0.01} {
		t.Errorf("kp[2] = %v, want [0 0 0.01] (low conf kept verbatim)", kp[2])
	}
}

// A detection model (no keypoints) must store NULL, not "[]"/"null", so the
// column stays cheap and the read path yields a nil (not a parsed) slice.
func TestSaveFrameOmitsKeypointsWhenAbsent(t *testing.T) {
	r := newTestRepo(t, 0)
	f := sampleFrame("camera01", 1) // no keypoints
	if err := r.SaveFrame(f, time.Unix(1700000000, 0).UTC()); err != nil {
		t.Fatalf("SaveFrame: %v", err)
	}
	snap, _ := r.Latest("camera01")
	if len(snap.Frame.Objects) != 2 {
		t.Fatalf("objects = %d, want 2", len(snap.Frame.Objects))
	}
	for _, o := range snap.Frame.Objects {
		if o.Keypoints != nil {
			t.Errorf("keypoints = %v, want nil (detection model)", o.Keypoints)
		}
	}
}

// AIMode: GET before any POST returns the default, Set/Get persist, an unknown
// camera returns the default (no row yet), and switching overwrites.
func TestAIModeDefaultAndSetGet(t *testing.T) {
	r := newTestRepo(t, 0)

	m, err := r.GetAIMode("camera01")
	if err != nil {
		t.Fatalf("GetAIMode default: %v", err)
	}
	if m != DefaultAIMode {
		t.Errorf("default mode = %q, want %q", m, DefaultAIMode)
	}

	if err := r.SetAIMode("camera01", AIModePose); err != nil {
		t.Fatalf("SetAIMode pose: %v", err)
	}
	if m, _ = r.GetAIMode("camera01"); m != AIModePose {
		t.Errorf("mode after set = %q, want %q", m, AIModePose)
	}

	// Unknown camera still returns default (no row yet).
	if m, _ = r.GetAIMode("other"); m != DefaultAIMode {
		t.Errorf("unknown camera mode = %q, want default %q", m, DefaultAIMode)
	}

	// Switching again overwrites.
	if err := r.SetAIMode("camera01", AIModeOff); err != nil {
		t.Fatalf("SetAIMode off: %v", err)
	}
	if m, _ = r.GetAIMode("camera01"); m != AIModeOff {
		t.Errorf("mode after switch = %q, want %q", m, AIModeOff)
	}
}

// An invalid mode is rejected so the DB never holds a value the agent poller
// cannot interpret, and the previous/default value is preserved.
func TestSetAIModeRejectsInvalid(t *testing.T) {
	r := newTestRepo(t, 0)
	if err := r.SetAIMode("camera01", "ai-bogus"); err == nil {
		t.Error("SetAIMode accepted invalid mode; want error")
	}
	if err := r.SetAIMode("camera01", ""); err == nil {
		t.Error("SetAIMode accepted empty mode; want error")
	}
	if m, _ := r.GetAIMode("camera01"); m != DefaultAIMode {
		t.Errorf("mode after rejected set = %q, want default %q", m, DefaultAIMode)
	}
}

// Deleting a camera must also drop its aimode row (no orphaned mode).
func TestDeleteCameraRemovesAIMode(t *testing.T) {
	r := newTestRepo(t, 0)
	if err := r.SetAIMode("camera01", AIModePose); err != nil {
		t.Fatalf("SetAIMode: %v", err)
	}
	if err := r.DeleteCamera("camera01"); err != nil {
		t.Fatalf("DeleteCamera: %v", err)
	}
	if m, _ := r.GetAIMode("camera01"); m != DefaultAIMode {
		t.Errorf("mode after delete = %q, want default %q", m, DefaultAIMode)
	}
}

// Regression: the per-frame AI mode must survive the round trip so the web can
// drop transition frames. The agent stamps ai_mode with the mode it ACTUALLY
// ran; during a detector swap the still-running previous model emits the old
// mode, and the web discards those until the new mode arrives.
func TestSaveFrameRoundTripsAIMode(t *testing.T) {
	r := newTestRepo(t, 0)
	f := sampleFrame("camera01", 15230)
	f.AIMode = AIModePose
	if err := r.SaveFrame(f, time.Unix(1700000000, 0).UTC()); err != nil {
		t.Fatalf("SaveFrame: %v", err)
	}
	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if snap.Frame == nil {
		t.Fatalf("Frame nil after save")
	}
	if snap.Frame.AIMode != AIModePose {
		t.Errorf("AIMode = %q, want %q", snap.Frame.AIMode, AIModePose)
	}

	// A detection-mode frame overwrites and the mode flips to detect.
	f.AIMode = AIModeDetect
	if err := r.SaveFrame(f, time.Unix(1700000001, 0).UTC()); err != nil {
		t.Fatalf("SaveFrame (detect): %v", err)
	}
	snap, err = r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest (detect): %v", err)
	}
	if snap.Frame.AIMode != AIModeDetect {
		t.Errorf("AIMode = %q, want %q (mode must follow the frame)", snap.Frame.AIMode, AIModeDetect)
	}
}

// Regression: a joint DB created before the keypoints column existed must keep
// working after an upgrade. CREATE TABLE IF NOT EXISTS cannot add a column to a
// table that already exists, so the agent's pose metadata would otherwise fail
// with "table ai_object has no column named keypoints". Migrate must ALTER the
// existing table in place, after which pose frames persist their keypoints.
func TestMigrateAddsKeypointsColumnToExistingDB(t *testing.T) {
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "legacy.db")+"?_pragma=busy_timeout(5000)")
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	t.Cleanup(func() { db.Close() })
	db.SetMaxOpenConns(1)

	// Simulate the older schema: ai_object WITHOUT the keypoints column.
	if _, err := db.Exec(`
		CREATE TABLE ai_object (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			camera_id   TEXT    NOT NULL,
			frame_id    INTEGER NOT NULL,
			class       TEXT    NOT NULL,
			confidence  REAL    NOT NULL,
			track_id    INTEGER NOT NULL DEFAULT 0,
			x1          INTEGER NOT NULL,
			y1          INTEGER NOT NULL,
			x2          INTEGER NOT NULL,
			y2          INTEGER NOT NULL,
			received_at TEXT    NOT NULL
		)`); err != nil {
		t.Fatalf("create legacy ai_object: %v", err)
	}

	// Upgrading an existing DB must not error and must add the column.
	if err := Migrate(db); err != nil {
		t.Fatalf("Migrate on legacy db: %v", err)
	}

	r := NewRepository(db, 0, 1)
	f := sampleFrame("camera01", 15230)
	f.Objects = []Object{{
		Class: "person", Confidence: 0.91, TrackID: 7, BBox: [4]int{100, 200, 300, 500},
		Keypoints: [][3]float64{{110, 210, 0.95}, {120, 240, 0.88}},
	}}
	if err := r.SaveFrame(f, time.Unix(1700000000, 0).UTC()); err != nil {
		t.Fatalf("SaveFrame after migrate: %v", err)
	}
	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if len(snap.Frame.Objects) != 1 {
		t.Fatalf("objects = %d, want 1", len(snap.Frame.Objects))
	}
	if len(snap.Frame.Objects[0].Keypoints) != 2 {
		t.Fatalf("keypoints = %d, want 2 (column added by Migrate)", len(snap.Frame.Objects[0].Keypoints))
	}
}

// Regression: a joint DB created before the per-frame ai_mode column existed
// must keep working after an upgrade. CREATE TABLE IF NOT EXISTS cannot add a
// column to a table that already exists, so the agent's stamped mode would
// otherwise fail with "table ai_frame has no column named ai_mode". Migrate
// must ALTER the existing table in place, after which frames persist their mode.
func TestMigrateAddsAIModeColumnToExistingDB(t *testing.T) {
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "legacy.db")+"?_pragma=busy_timeout(5000)")
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	t.Cleanup(func() { db.Close() })
	db.SetMaxOpenConns(1)

	// Simulate the older schema: ai_frame WITHOUT the ai_mode column.
	if _, err := db.Exec(`
		CREATE TABLE ai_frame (
			camera_id     TEXT PRIMARY KEY,
			frame_id      INTEGER NOT NULL,
			timestamp     INTEGER NOT NULL,
			video_width   INTEGER NOT NULL DEFAULT 0,
			video_height  INTEGER NOT NULL DEFAULT 0,
			object_count  INTEGER NOT NULL DEFAULT 0,
			received_at   TEXT NOT NULL,
			updated_at    TEXT NOT NULL
		)`); err != nil {
		t.Fatalf("create legacy ai_frame: %v", err)
	}

	// Upgrading an existing DB must not error and must add the column.
	if err := Migrate(db); err != nil {
		t.Fatalf("Migrate on legacy db: %v", err)
	}

	r := NewRepository(db, 0, 1)
	f := sampleFrame("camera01", 15230)
	f.AIMode = AIModePose
	if err := r.SaveFrame(f, time.Unix(1700000000, 0).UTC()); err != nil {
		t.Fatalf("SaveFrame after migrate: %v", err)
	}
	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if snap.Frame.AIMode != AIModePose {
		t.Errorf("AIMode = %q, want %q (column added by Migrate)", snap.Frame.AIMode, AIModePose)
	}
}

// Wire format: GET /api/cameras/{id}/metadata marshals a Snapshot exactly as
// cameraMetadata does. A frame stamped with a mode must carry "ai_mode" on the
// wire; a legacy frame (empty mode, agent predating the field) must omit it so
// the web's unified drop rule treats it as a mismatch and discards it.
func TestFrameViewMarshalAIMode(t *testing.T) {
	withMode := Snapshot{Frame: &FrameView{
		FrameID: 1, VideoWidth: 1920, VideoHeight: 1080, AIMode: AIModePose, ObjectCount: 1,
	}}
	b, err := json.Marshal(withMode)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	if !strings.Contains(string(b), `"ai_mode":"ai-y-pose"`) {
		t.Errorf("marshalled frame with mode should include ai_mode: %s", b)
	}

	legacy := Snapshot{Frame: &FrameView{
		FrameID: 1, VideoWidth: 1920, VideoHeight: 1080, ObjectCount: 1,
	}}
	b, err = json.Marshal(legacy)
	if err != nil {
		t.Fatalf("marshal legacy: %v", err)
	}
	if strings.Contains(string(b), "ai_mode") {
		t.Errorf("legacy frame should omit ai_mode (web drops it as mismatch): %s", b)
	}
}
