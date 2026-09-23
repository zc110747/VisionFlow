package metadata

import (
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"testing"
	"time"
)

// ---------------------------------------------------------------------------
// Fixtures
//
// These build the JSON as TEXT rather than filling in FrameMessage literals on
// purpose. The property under test is "the server stores the agent's hand block
// without rewriting it", and that can only be observed by starting from bytes
// that look like the real wire format - field order included. Constructing the
// struct directly would skip the decoder and make the test agree with itself.
// ---------------------------------------------------------------------------

// handBlockJSON mirrors carmera-agent/src/metadata/metadata_encoder.cpp
// :: hand_json - same keys, same order, compact, 21 landmarks.
func handBlockJSON(detected bool, gesture string) string {
	var lm strings.Builder
	lm.WriteByte('[')
	for i := 0; i < 21; i++ {
		if i > 0 {
			lm.WriteByte(',')
		}
		fmt.Fprintf(&lm, "[%d.00,%d.50,%d.25]", 300+i, 200+i, i)
	}
	lm.WriteByte(']')
	return `{"detected":` + strconv.FormatBool(detected) + `,` +
		`"confidence":0.91,"handedness":"right","reference_initialized":true,` +
		`"position":[0.5213,0.4123,-0.1234],` +
		`"reference":[0.5000,0.4000,-0.1000],` +
		`"delta":[0.0213,0.0123,-0.0234],` +
		`"motion":{"x":"LEFT","y":"FORWARD","z":"UP"},` +
		`"gesture":"` + gesture + `","gesture_candidate":"` + gesture + `",` +
		`"gesture_confidence":0.75,"landmarks":` + lm.String() + `}`
}

func handFrameJSON(cam string, frameID, ts int64, detected bool, gesture string) string {
	return `{"version":1,"type":"frame","camera_id":"` + cam + `",` +
		`"frame_id":` + strconv.FormatInt(frameID, 10) + `,` +
		`"timestamp":` + strconv.FormatInt(ts, 10) + `,` +
		`"video_width":1280,"video_height":720,"ai_mode":"` + AIModeHand + `",` +
		`"objects":[],"hand":` + handBlockJSON(detected, gesture) + `}`
}

// saveHandFrame runs the same path as the ingest handler: decode -> validate ->
// normalize -> store.
func saveHandFrame(t *testing.T, r *Repository, cam string, frameID, ts int64, detected bool, gesture string) {
	t.Helper()
	var msg FrameMessage
	if err := json.Unmarshal([]byte(handFrameJSON(cam, frameID, ts, detected, gesture)), &msg); err != nil {
		t.Fatalf("decode hand frame (frame %d): %v", frameID, err)
	}
	if err := msg.Validate(); err != nil {
		t.Fatalf("validate hand frame (frame %d): %v", frameID, err)
	}
	msg.Normalize()
	if err := r.SaveFrame(&msg, time.Unix(1700000000, 0).UTC()); err != nil {
		t.Fatalf("SaveFrame (frame %d): %v", frameID, err)
	}
}

// ---------------------------------------------------------------------------
// Decoding and fidelity
// ---------------------------------------------------------------------------

// The recorder exists so a calibration or axis experiment can be replayed
// offline. That only works if the block that comes back is the block that was
// sent - a field the server does not know about must survive it.
func TestHandBlockRoundTripsVerbatim(t *testing.T) {
	want := handBlockJSON(true, GestureFist)

	var msg FrameMessage
	if err := json.Unmarshal([]byte(handFrameJSON("camera01", 660, 12345, true, GestureFist)), &msg); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if msg.Hand == nil {
		t.Fatal("hand block did not decode")
	}
	got, err := json.Marshal(msg.Hand)
	if err != nil {
		t.Fatalf("re-encode: %v", err)
	}
	if string(got) != want {
		t.Errorf("re-encoded hand block differs from what the agent sent:\n got %s\nwant %s", got, want)
	}

	// Unknown keys are the case the fidelity actually protects: a field from a
	// later agent phase must not be dropped on the way to the log.
	withFuture := `{"type":"frame","camera_id":"camera01","frame_id":1,"timestamp":1,` +
		`"video_width":1280,"video_height":720,"objects":[],` +
		`"hand":{"detected":true,"gesture":"FIST","finger_curl":[1,2,3,4]}}`
	var future FrameMessage
	if err := json.Unmarshal([]byte(withFuture), &future); err != nil {
		t.Fatalf("decode future-shaped hand: %v", err)
	}
	out, err := json.Marshal(future.Hand)
	if err != nil {
		t.Fatalf("re-encode future hand: %v", err)
	}
	if !strings.Contains(string(out), "finger_curl") {
		t.Errorf("a field this build does not model was dropped on storage: %s", out)
	}
}

func TestHandBlockSurvivesStorage(t *testing.T) {
	r := newTestRepo(t, 100)
	saveHandFrame(t, r, "camera01", 660, 12345, true, GestureFist)

	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if snap.Frame == nil || snap.Frame.Hand == nil {
		t.Fatal("hand block missing from the stored frame")
	}
	h := snap.Frame.Hand
	if h.Gesture != GestureFist {
		t.Errorf("gesture = %q, want %q", h.Gesture, GestureFist)
	}
	if h.Motion.X != "LEFT" || h.Motion.Y != "FORWARD" || h.Motion.Z != "UP" {
		t.Errorf("motion = %+v, want LEFT/FORWARD/UP", h.Motion)
	}
	if !h.ReferenceInitialized {
		t.Error("reference_initialized was lost")
	}
	if h.Position[2] != -0.1234 {
		t.Errorf("position.z = %v, want -0.1234", h.Position[2])
	}
	if len(h.Landmarks) != HandLandmarkCount {
		t.Fatalf("landmarks = %d, want %d", len(h.Landmarks), HandLandmarkCount)
	}
	// Derive the expectation from the fixture's own formula (x=300+i,
	// y=200+i+.50, z=i+.25) rather than transcribing the numbers: hand-computing
	// them is how this assertion got two different wrong values in a row.
	wantLandmark := func(i int) [3]float64 {
		f := float64(i)
		return [3]float64{300 + f, 200 + f + 0.5, f + 0.25}
	}
	if h.Landmarks[0] != wantLandmark(0) || h.Landmarks[20] != wantLandmark(20) {
		t.Errorf("landmarks not stored as sent: first=%v last=%v", h.Landmarks[0], h.Landmarks[20])
	}
}

// A detect / pose frame must stay exactly what it was before ai-hand existed:
// no hand block, and nothing appended to the recorder.
func TestFrameWithoutHandKeepsHandNull(t *testing.T) {
	r := newTestRepo(t, 100)
	if err := r.SaveFrame(sampleFrame("camera01", 1), time.Now()); err != nil {
		t.Fatalf("SaveFrame: %v", err)
	}
	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if snap.Frame.Hand != nil {
		t.Errorf("a frame with no \"hand\" key produced a hand block: %+v", snap.Frame.Hand)
	}
	list, err := r.HandSessions("camera01", 1000)
	if err != nil {
		t.Fatalf("HandSessions: %v", err)
	}
	if list.TotalRows != 0 {
		t.Errorf("ai_hand_log gained %d rows from a frame that had no hand", list.TotalRows)
	}
}

func TestHandNormalizeSanitisesBlock(t *testing.T) {
	raw := `{"type":"frame","camera_id":"c","frame_id":1,"timestamp":1,` +
		`"video_width":1280,"video_height":720,"objects":[],` +
		`"hand":{"detected":true,"confidence":3.7,"gesture_confidence":-2,` +
		`"gesture":"FIST","landmarks":[[1,2,3],[4,5,6]]}}`
	var msg FrameMessage
	if err := json.Unmarshal([]byte(raw), &msg); err != nil {
		t.Fatalf("decode: %v", err)
	}
	msg.Normalize()
	if msg.Hand == nil {
		t.Fatal("hand block lost")
	}
	if msg.Hand.Confidence != 1 {
		t.Errorf("confidence = %v, want clamped to 1", msg.Hand.Confidence)
	}
	if msg.Hand.GestureConfidence != 0 {
		t.Errorf("gesture_confidence = %v, want clamped to 0", msg.Hand.GestureConfidence)
	}
	// The array is positional (index 4 is the thumb tip), so a wrong arity is
	// not "close enough" - it is dropped rather than stored to be misread.
	if len(msg.Hand.Landmarks) != 0 {
		t.Errorf("landmarks with the wrong arity survived: %d entries", len(msg.Hand.Landmarks))
	}
	// The verdict itself is computed agent-side and must survive the cleanup.
	if msg.Hand.Gesture != GestureFist {
		t.Errorf("gesture = %q, want it untouched", msg.Hand.Gesture)
	}
}

// ---------------------------------------------------------------------------
// Recorder
// ---------------------------------------------------------------------------

func TestHandLogRecordsEveryFrame(t *testing.T) {
	r := newTestRepo(t, 100)
	saveHandFrame(t, r, "camera01", 1, 1000, true, GestureFist)
	saveHandFrame(t, r, "camera01", 2, 1125, false, GestureUnknown)
	saveHandFrame(t, r, "camera01", 3, 1250, true, GestureOpenPalm)

	list, err := r.HandSessions("camera01", 1000)
	if err != nil {
		t.Fatalf("HandSessions: %v", err)
	}
	// Frames with detected=false are recorded too - they are what tells two
	// holds of the same pose apart.
	if list.TotalRows != 3 {
		t.Errorf("total_rows = %d, want 3 (the recorder is lossless)", list.TotalRows)
	}
	if len(list.Sessions) != 2 {
		t.Fatalf("sessions = %d, want 2 (the empty middle frame ends the first)", len(list.Sessions))
	}
}

func TestHandLogDetectedOnlyOption(t *testing.T) {
	r := newTestRepoOpts(t, Options{HandLogMaxRows: 100, HandLogDetectedOnly: true})
	saveHandFrame(t, r, "camera01", 1, 1000, true, GestureFist)
	saveHandFrame(t, r, "camera01", 2, 1125, false, GestureUnknown)
	saveHandFrame(t, r, "camera01", 3, 1250, true, GestureOpenPalm)

	list, err := r.HandSessions("camera01", 1000)
	if err != nil {
		t.Fatalf("HandSessions: %v", err)
	}
	if list.TotalRows != 2 {
		t.Errorf("total_rows = %d, want 2 with detected-only on", list.TotalRows)
	}
}

// The recorder is off when the cap is <= 0, but the latest hand state must
// still be readable - the web needs it regardless of whether history is kept.
func TestHandLogDisabledStillStoresLatestHand(t *testing.T) {
	r := newTestRepo(t, 0)
	saveHandFrame(t, r, "camera01", 1, 1000, true, GestureFist)
	list, err := r.HandSessions("camera01", 1000)
	if err != nil {
		t.Fatalf("HandSessions: %v", err)
	}
	if list.TotalRows != 0 {
		t.Errorf("total_rows = %d, want 0 with the recorder off", list.TotalRows)
	}
	snap, err := r.Latest("camera01")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if snap.Frame == nil || snap.Frame.Hand == nil {
		t.Error("the latest hand state must be available even with the recorder off")
	}
}

func TestHandSessionsSplitAndExport(t *testing.T) {
	r := newTestRepo(t, 1000)

	// Hold 1: four frames at ~8 fps, all FIST.
	for i, id := range []int64{660, 661, 662, 663} {
		saveHandFrame(t, r, "camera01", id, 1000+int64(i)*125, true, GestureFist)
	}
	// Hand leaves the frame.
	saveHandFrame(t, r, "camera01", 664, 1500, false, GestureUnknown)
	// Hold 2, after a pause far longer than the gap.
	saveHandFrame(t, r, "camera01", 700, 5000, true, GestureOpenPalm)
	saveHandFrame(t, r, "camera01", 701, 5125, true, GestureOpenPalm)

	list, err := r.HandSessions("camera01", 1000)
	if err != nil {
		t.Fatalf("HandSessions: %v", err)
	}
	if list.TotalRows != 7 {
		t.Errorf("total_rows = %d, want 7", list.TotalRows)
	}
	if len(list.Sessions) != 2 {
		t.Fatalf("sessions = %d, want 2", len(list.Sessions))
	}

	s1, s2 := list.Sessions[0], list.Sessions[1]
	if s1.Index != 1 || s2.Index != 2 {
		t.Errorf("index = %d,%d want 1,2", s1.Index, s2.Index)
	}
	if s1.StartFrameID != 660 || s1.EndFrameID != 663 || s1.FrameCount != 4 {
		t.Errorf("session 1 = %+v, want frames 660..663 x4", s1)
	}
	if s1.StartTimestamp != 1000 || s1.EndTimestamp != 1375 {
		t.Errorf("session 1 span = %d..%d, want 1000..1375", s1.StartTimestamp, s1.EndTimestamp)
	}
	if s1.Gestures[GestureFist] != 4 {
		t.Errorf("session 1 gestures = %v, want FIST x4", s1.Gestures)
	}
	if s2.StartFrameID != 700 || s2.EndFrameID != 701 || s2.FrameCount != 2 {
		t.Errorf("session 2 = %+v, want frames 700..701 x2", s2)
	}
	if s2.Gestures[GestureOpenPalm] != 2 {
		t.Errorf("session 2 gestures = %v, want OPEN_PALM x2", s2.Gestures)
	}

	// Export must carry the payloads, not just the summary.
	_, rows, err := r.HandSessionRows("camera01", 2, 1000)
	if err != nil {
		t.Fatalf("HandSessionRows: %v", err)
	}
	if len(rows) != 2 {
		t.Fatalf("exported %d frames, want 2", len(rows))
	}
	if rows[0].FrameID != 700 || rows[1].FrameID != 701 {
		t.Errorf("exported frames = %d,%d want 700,701", rows[0].FrameID, rows[1].FrameID)
	}
	for i, row := range rows {
		if row.Hand == nil {
			t.Fatalf("frame %d exported without its hand block", row.FrameID)
		}
		if row.Hand.Gesture != GestureOpenPalm {
			t.Errorf("frame %d gesture = %q, want %q", i, row.Hand.Gesture, GestureOpenPalm)
		}
		if !row.Detected {
			t.Errorf("frame %d exported as not detected", row.FrameID)
		}
	}

	// An index the list never reported is not a session.
	for _, bad := range []int{0, 3, 99} {
		if _, _, err := r.HandSessionRows("camera01", bad, 1000); !errors.Is(err, ErrNoSuchSession) {
			t.Errorf("HandSessionRows(index=%d) error = %v, want ErrNoSuchSession", bad, err)
		}
	}
}

// The agent restarts its frame_id and timestamp from zero on every launch, so
// neither is a key an offline reader can sort or join on. Observed on the real
// rig: (frame_id=77702,timestamp=2591322) is immediately followed by
// (frame_id=1,timestamp=700). The recorder's own id is the only monotonic one,
// and it has to reach the exported file rather than stopping at the Go struct -
// P4's offline analysis reads the file, not the struct.
func TestHandLogRowIDSurvivesAnAgentRestart(t *testing.T) {
	r := newTestRepo(t, 100)

	// Run 1, then a relaunch: the agent's clocks reset, the recorder's does not.
	saveHandFrame(t, r, "camera01", 900, 900000, true, GestureFist)
	saveHandFrame(t, r, "camera01", 901, 900125, true, GestureFist)
	saveHandFrame(t, r, "camera01", 1, 700, true, GestureOpenPalm)
	saveHandFrame(t, r, "camera01", 2, 825, true, GestureOpenPalm)

	list, err := r.HandSessions("camera01", 1000)
	if err != nil {
		t.Fatalf("HandSessions: %v", err)
	}
	if len(list.Sessions) != 2 {
		t.Fatalf("sessions = %d, want 2 (a rewound clock starts a new run)", len(list.Sessions))
	}

	_, first, err := r.HandSessionRows("camera01", 1, 1000)
	if err != nil {
		t.Fatalf("HandSessionRows(1): %v", err)
	}
	_, second, err := r.HandSessionRows("camera01", 2, 1000)
	if err != nil {
		t.Fatalf("HandSessionRows(2): %v", err)
	}
	if len(first) != 2 || len(second) != 2 {
		t.Fatalf("exported %d and %d rows, want 2 and 2", len(first), len(second))
	}
	if first[0].ID == 0 || first[1].ID == 0 {
		t.Fatal("rows were exported without the recorder id")
	}
	// Only id orders these two runs: frame_id reads 900,901 then 1,2, and
	// timestamp reads 900000,900125 then 700,825 - i.e. "then" looks earlier.
	if first[1].ID >= second[0].ID {
		t.Errorf("ids %d then %d are not ordered across the restart", first[1].ID, second[0].ID)
	}
	if second[0].ID >= second[1].ID {
		t.Errorf("ids %d then %d are not increasing within a run", second[0].ID, second[1].ID)
	}

	file := HandSessionFile{
		Version: 1, CameraID: "camera01",
		Session: list.Sessions[1], GapMS: 1000, Frames: second,
	}
	raw, err := json.Marshal(file)
	if err != nil {
		t.Fatalf("marshal export: %v", err)
	}
	if !strings.Contains(string(raw), `"id":`) {
		t.Errorf("the export dropped the only monotonic key: %s", raw)
	}
}

// The split rule is pure logic over the row list, so it is pinned directly
// rather than only through the database.
func TestSplitHandSessionsBoundaries(t *testing.T) {
	det := func(id, ts int64) handLogMeta {
		return handLogMeta{id: id, frameID: id, timestamp: ts, detected: true, gesture: GestureFist}
	}
	blank := func(id, ts int64) handLogMeta {
		return handLogMeta{id: id, frameID: id, timestamp: ts, detected: false}
	}
	cases := []struct {
		name string
		rows []handLogMeta
		gap  int
		want int
	}{
		{"empty log", nil, 1000, 0},
		{"contiguous run", []handLogMeta{det(1, 0), det(2, 100), det(3, 200)}, 1000, 1},
		{"a gap of exactly gap_ms is still one run", []handLogMeta{det(1, 0), det(2, 1000)}, 1000, 1},
		{"rows further apart than the gap split", []handLogMeta{det(1, 0), det(2, 5000)}, 1000, 2},
		{"a gap wider than the distance merges", []handLogMeta{det(1, 0), det(2, 5000)}, 10000, 1},
		{"one millisecond past the gap splits", []handLogMeta{det(1, 0), det(2, 1001)}, 1000, 2},
		{"a loss separates two holds of the same pose",
			[]handLogMeta{det(1, 0), det(2, 100), blank(3, 200), det(4, 300), det(5, 400)}, 1000, 2},
		{"a trailing loss does not create a session", []handLogMeta{det(1, 0), blank(2, 100)}, 1000, 1},
		{"a backwards jump is an agent restart, not a session",
			[]handLogMeta{det(1, 9000), det(2, 9100), det(3, 50), det(4, 150)}, 1000, 2},
		{"a non-positive gap falls back to the default", []handLogMeta{det(1, 0), det(2, 1500)}, 0, 2},
		// The two criteria are independent: no gap is wide enough to merge
		// across a row that says the hand left the frame.
		{"no gap merges across a loss",
			[]handLogMeta{det(1, 0), blank(2, 100), det(3, 200)}, 100000, 2},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := len(splitHandSessions(tc.rows, tc.gap)); got != tc.want {
				t.Errorf("sessions = %d, want %d", got, tc.want)
			}
		})
	}
}

func TestHandLogPrunesOldestBeyondCap(t *testing.T) {
	const cap = 5
	r := newTestRepo(t, cap)
	for i := 0; i < 12; i++ {
		saveHandFrame(t, r, "camera01", 1000+int64(i), 1000+int64(i)*125, true, GestureFist)
	}
	list, err := r.HandSessions("camera01", 1000)
	if err != nil {
		t.Fatalf("HandSessions: %v", err)
	}
	if list.TotalRows != cap {
		t.Errorf("total_rows = %d, want the cap %d", list.TotalRows, cap)
	}
	_, rows, err := r.HandSessionRows("camera01", 1, 1000)
	if err != nil {
		t.Fatalf("HandSessionRows: %v", err)
	}
	if len(rows) != cap {
		t.Fatalf("exported %d rows, want %d", len(rows), cap)
	}
	// Oldest-first: the newest frame must be the one that survived.
	if rows[len(rows)-1].FrameID != 1011 {
		t.Errorf("newest kept frame = %d, want 1011", rows[len(rows)-1].FrameID)
	}
	if rows[0].FrameID != 1007 {
		t.Errorf("oldest kept frame = %d, want 1007", rows[0].FrameID)
	}
}

func TestClearHandLogIsPerCamera(t *testing.T) {
	r := newTestRepo(t, 100)
	saveHandFrame(t, r, "camera01", 1, 1000, true, GestureFist)
	saveHandFrame(t, r, "camera01", 2, 1125, true, GestureFist)
	saveHandFrame(t, r, "camera02", 1, 1000, true, GestureOpenPalm)

	n, err := r.ClearHandLog("camera01")
	if err != nil {
		t.Fatalf("ClearHandLog: %v", err)
	}
	if n != 2 {
		t.Errorf("cleared = %d, want 2", n)
	}
	one, err := r.HandSessions("camera01", 1000)
	if err != nil {
		t.Fatalf("HandSessions(camera01): %v", err)
	}
	if one.TotalRows != 0 {
		t.Errorf("camera01 still has %d rows", one.TotalRows)
	}
	two, err := r.HandSessions("camera02", 1000)
	if err != nil {
		t.Fatalf("HandSessions(camera02): %v", err)
	}
	if two.TotalRows != 1 {
		t.Errorf("clearing one camera removed %d rows from another", 1-two.TotalRows)
	}
}

// ---------------------------------------------------------------------------
// Mode / token tables
// ---------------------------------------------------------------------------

// ai-hand has to be accepted here before the agent's aimode poller can ever be
// told to switch to it - the CLI flag alone is overridden by the poller within
// about a second (observed on the real rig).
func TestValidAIModeCoversHandAndRejectsUnknown(t *testing.T) {
	for _, m := range []string{AIModeOff, AIModeDetect, AIModePose, AIModeHand} {
		if !ValidAIMode(m) {
			t.Errorf("ValidAIMode(%q) = false, want true", m)
		}
	}
	// Near-misses must not slip through: the agent keeps its current model on an
	// unknown token rather than guessing, so a typo stored here is inert rather
	// than a nearest-match surprise.
	for _, m := range []string{"", "ai-bogus", "ai-y-hand", "AI-HAND", "ai-hand ", "hand"} {
		if ValidAIMode(m) {
			t.Errorf("ValidAIMode(%q) = true, want false", m)
		}
	}
}

func TestValidGestureIsClosedSet(t *testing.T) {
	for _, g := range []string{GestureUnknown, GestureOpenPalm, GestureFist} {
		if !ValidGesture(g) {
			t.Errorf("ValidGesture(%q) = false, want true", g)
		}
	}
	for _, g := range []string{"", "open_palm", "POINT", "FIST "} {
		if ValidGesture(g) {
			t.Errorf("ValidGesture(%q) = true, want false", g)
		}
	}
}
