// Package metadata implements the SERVER side of the camera-agent AI metadata
// protocol. The wire format is defined by carmera-agent (see
// ../carmera-agent/README.md, section "Metadata 分支"); this package only
// consumes it.
//
// Two message kinds share one endpoint (POST /api/metadata) and are told apart
// by the "type" field:
//
//	"frame"  - one AI inference result: frame_id/timestamp + detected objects
//	"status" - periodic AI heartbeat (liveness + model/tracker identity)
//
// Hard rule mirrored from the agent side: frame_id and timestamp are produced
// by the CAPTURE side (camera frame counter / GStreamer PTS) and must be stored
// verbatim. The server never renumbers or "fixes" them.
package metadata

import (
	"encoding/json"
	"fmt"
	"time"

	"video-server/internal/logger"
)

// Message types carried by the "type" field.
const (
	TypeFrame  = "frame"
	TypeStatus = "status"
)

// AI modes driven by the web UI (see /api/cameras/{id}/aimode).
const (
	// AIModeOff means "stop drawing AI on the web UI". The agent keeps its
	// model loaded; the UI simply hides the overlay.
	AIModeOff = "ai-off"
	// AIModeDetect = person detection only (yolo11n).
	AIModeDetect = "ai-y"
	// AIModePose = person detection + 17 COCO keypoints (yolo11n-pose).
	AIModePose = "ai-y-pose"
	// AIModeHand = hand 3D landmarks (21 points, with wrist-relative depth) and
	// a gesture, from the MediaPipe palm -> landmark cascade. This mode does NOT
	// run person detection: the agent holds exactly one model at a time, so
	// switching to it releases the YOLO session (see carmera-agent README).
	AIModeHand = "ai-hand"
)

// DefaultAIMode is reported by GET /aimode before any choice is made.
const DefaultAIMode = AIModeDetect

// ValidAIMode reports whether s is one of the supported modes.
//
// This is the ONLY place the server decides which AI modes exist. The agent
// parses the same tokens with aimode_from_string() and deliberately ignores
// unknown ones (keeping its current model) rather than guessing a nearest
// match - so a mode added here but not there is inert, never harmful.
func ValidAIMode(s string) bool {
	switch s {
	case AIModeOff, AIModeDetect, AIModePose, AIModeHand:
		return true
	}
	return false
}

// Gesture tokens, mirroring carmera-agent's to_string(Gesture) verbatim.
//
// Kept in sync by hand because the two live in different languages; the agent
// source (src/ai/hand_analyzer.cpp) is the single definition point and names
// this rule in its own comment. Used only for REPORTING (session histograms and
// an explicit "is this a gesture Phase 1 knows" check) - never to reject or
// rewrite a value, so a later phase adding a gesture degrades to "unknown
// token" instead of a 400.
const (
	GestureUnknown  = "UNKNOWN"
	GestureOpenPalm = "OPEN_PALM"
	GestureFist     = "FIST"
)

// ValidGesture reports whether g is one of the Phase 1 gestures.
func ValidGesture(g string) bool {
	switch g {
	case GestureUnknown, GestureOpenPalm, GestureFist:
		return true
	}
	return false
}

// Probe is the first-pass decode target: only "type" is read so the handler can
// pick the right struct for the real decode. Frame and status messages share no
// other field, so decoding straight into one struct would silently drop data.
type Probe struct {
	Type string `json:"type"`
}

// InferType guesses the message kind from the payload shape. It exists purely
// for agent builds that predate the mandatory "type" field: those emit frames
// without any discriminator, so the only way to accept them is to look at which
// fields are present.
//
//   - "ai" (an object)          -> status heartbeat
//   - "objects" or "frame_id"   -> detection frame
//
// Returns "" when neither shape matches, which the caller turns into a 400 -
// guessing further would risk storing a frame as a heartbeat.
func InferType(body []byte) string {
	var shape struct {
		AI      json.RawMessage `json:"ai"`
		Objects json.RawMessage `json:"objects"`
		FrameID *int64          `json:"frame_id"`
	}
	if err := json.Unmarshal(body, &shape); err != nil {
		return ""
	}
	if len(shape.AI) > 0 {
		return TypeStatus
	}
	if len(shape.Objects) > 0 || shape.FrameID != nil {
		return TypeFrame
	}
	return ""
}

// Object is one detection inside a frame message. BBox is [x1,y1,x2,y2] in
// ORIGINAL video pixels (never normalised, never scaled).
type Object struct {
	Class      string  `json:"class"`
	Confidence float64 `json:"confidence"`
	TrackID    int     `json:"track_id"`
	BBox       [4]int  `json:"bbox"`
	// Keypoints is [x, y, conf] per COCO joint, present only for pose models.
	// x/y are ORIGINAL video pixels; conf is the model's (already sigmoided)
	// joint confidence in [0,1].
	Keypoints [][3]float64 `json:"keypoints,omitempty"`
}

// Motion3 is the per-axis motion classification of the hand this frame.
// Values are STATIONARY / LEFT / RIGHT (x), STATIONARY / FORWARD / BACKWARD (y),
// STATIONARY / UP / DOWN (z) - the agent's enum spellings, stored verbatim.
type Motion3 struct {
	X string `json:"x"`
	Y string `json:"y"`
	Z string `json:"z"`
}

// HandState is the "hand" block of a frame message (agent mode ai-hand only).
//
// Three axis systems meet here and must not be confused:
//
//	position / reference / delta  logical space, normalised to the frame:
//	                              X+ right, Y+ closer to the camera, Z+ up.
//	                              Y comes from the hand's apparent SIZE by
//	                              default, not from landmark depth.
//	landmarks                     21 x [x,y,z] in ORIGINAL video pixels. z is
//	                              the model's wrist-relative depth at the same
//	                              scale as x/y - NOT centimetres.
//
// Delta is meaningless until ReferenceInitialized is true: the agent only
// latches a reference once the hand has been stable, which is what stops a
// phantom jump when a hand enters the frame. Consumers MUST honour that flag.
type HandState struct {
	Detected             bool       `json:"detected"`
	Confidence           float64    `json:"confidence"`
	Handedness           string     `json:"handedness"`
	ReferenceInitialized bool       `json:"reference_initialized"`
	Position             [3]float64 `json:"position"`
	Reference            [3]float64 `json:"reference"`
	Delta                [3]float64 `json:"delta"`
	Motion               Motion3    `json:"motion"`
	Gesture              string     `json:"gesture"`
	// GestureCandidate is the pre-debounce value for the same frame; the two
	// differ while a pose is still being confirmed. Kept because a gesture that
	// flaps between two poses is only visible when both are recorded.
	GestureCandidate  string       `json:"gesture_candidate"`
	GestureConfidence float64      `json:"gesture_confidence"`
	Landmarks         [][3]float64 `json:"landmarks,omitempty"`

	// raw is the exact object this block arrived as, kept so that re-encoding
	// for storage returns the producer's bytes rather than a re-serialisation
	// of the fields this build happens to know about. Unexported: it is a
	// fidelity detail, not part of the model.
	raw json.RawMessage
}

// UnmarshalJSON decodes the hand block and remembers its source bytes.
//
// The reason for the extra field: this block's whole point is to be recorded
// and analysed offline (threshold recalibration, the axis experiment), and the
// recorder must not silently narrow it. Without this, a field added by a later
// agent phase - say a per-finger curl value - would be dropped by the server
// and be unrecoverable from the log, even though the agent sent it.
func (h *HandState) UnmarshalJSON(b []byte) error {
	// A distinct named type: it has the same fields but none of these methods,
	// so the decode below cannot recurse back into this function.
	type plain HandState
	var p plain
	if err := json.Unmarshal(b, &p); err != nil {
		return err
	}
	*h = HandState(p)
	// Copy: the decoder may hand out a window into a buffer it reuses.
	h.raw = append(json.RawMessage(nil), b...)
	return nil
}

// MarshalJSON re-emits the bytes the block arrived as, falling back to the
// struct's own encoding for a HandState that was built in code (tests, and any
// future producer inside the server).
//
// json.Marshal compacts whitespace but does not resequence keys, and the agent
// emits compact JSON already - so for a real payload this is byte-identical.
func (h HandState) MarshalJSON() ([]byte, error) {
	if len(h.raw) > 0 {
		return h.raw, nil
	}
	type plain HandState
	return json.Marshal(plain(h))
}

// FrameMessage is an AI inference result pushed by the agent (type="frame").
type FrameMessage struct {
	Version     int    `json:"version"`
	Type        string `json:"type"`
	CameraID    string `json:"camera_id"`
	FrameID     int64  `json:"frame_id"`
	Timestamp   int64  `json:"timestamp"`
	VideoWidth  int    `json:"video_width"`
	VideoHeight int    `json:"video_height"`
	// AIMode is the mode the agent ACTUALLY ran to produce this frame
	// ("ai-y" / "ai-y-pose" / "ai-hand"). The web drops frames whose mode no
	// longer matches the selected mode - this is what prevents stale boxes
	// during a switch.
	AIMode  string   `json:"ai_mode,omitempty"`
	Objects []Object `json:"objects"`
	// Hand is present ONLY on frames produced by ai-hand. A pointer (not a
	// zero-valued struct) for the same reason the agent emits the key
	// conditionally: a detect / pose frame must decode to exactly what it
	// decoded to before this field existed, with nothing to distinguish
	// "no hand mode" from "hand mode, nothing found".
	Hand *HandState `json:"hand,omitempty"`
}

// AIState is the AI sub-object of a heartbeat message.
type AIState struct {
	Enable        bool    `json:"enable"`
	Running       bool    `json:"running"`
	FPS           float64 `json:"fps"`
	Model         string  `json:"model"`
	Tracker       string  `json:"tracker"`
	LastFrameID   int64   `json:"last_frame_id"`
	LastTimestamp int64   `json:"last_timestamp"`
	Processed     int64   `json:"processed"`
}

// StatusMessage is the AI heartbeat pushed by the agent (type="status").
type StatusMessage struct {
	Version   int     `json:"version"`
	Type      string  `json:"type"`
	CameraID  string  `json:"camera_id"`
	WallClock int64   `json:"wall_clock"`
	AI        AIState `json:"ai"`
}

// b1 is the smallest allowed box edge. A degenerate box (x2==x1) cannot be
// drawn and cannot be IoU-matched, so it is widened by one pixel instead of
// being dropped - the agent already does the same clamp, this is just the
// server-side safety net for untrusted input.
const minBoxEdge = 1

// Normalize sanitises a frame message in place. It is deliberately defensive:
// the agent already clamps its boxes, but the server must not trust any
// producer it did not write.
//
// Returns the number of objects that were dropped as unusable.
func (f *FrameMessage) Normalize() (dropped int) {
	if f.VideoWidth < 0 {
		f.VideoWidth = 0
	}
	if f.VideoHeight < 0 {
		f.VideoHeight = 0
	}
	f.normalizeHand()
	if f.Objects == nil {
		f.Objects = []Object{}
		return 0
	}
	kept := f.Objects[:0]
	for _, o := range f.Objects {
		if !normalizeBox(&o.BBox, f.VideoWidth, f.VideoHeight) {
			dropped++
			continue
		}
		if o.Confidence < 0 {
			o.Confidence = 0
		}
		if o.Confidence > 1 {
			o.Confidence = 1
		}
		kept = append(kept, o)
	}
	f.Objects = kept
	return dropped
}

// normalizeHand clamps the hand block's confidences into [0,1] and drops a
// landmark array whose arity is not 21.
//
// Confidence is clamped for the same reason object confidence is - a consumer
// sizing a meter bar must not have to defend against 3.7 or NaN. The landmark
// count is checked because the array is indexed by MediaPipe's joint table
// (0 = wrist, 4 = thumb tip, ... 20 = pinky tip): an array of the wrong length
// is not "slightly off", it silently means different joints. Rather than store
// something that would be misread later, it is dropped and the rest of the
// frame is kept - the gesture verdict, which is what most consumers want, is
// computed on the agent side and survives.
//
// Enum tokens (gesture / handedness / motion) are deliberately NOT validated
// here. They are stored verbatim so a later agent phase can add a gesture
// without a server release; ValidGesture() is for reporting only.
func (f *FrameMessage) normalizeHand() {
	h := f.Hand
	if h == nil {
		return
	}
	h.Confidence = clamp01(h.Confidence)
	h.GestureConfidence = clamp01(h.GestureConfidence)
	if len(h.Landmarks) > 0 && len(h.Landmarks) != HandLandmarkCount {
		logger.Warn("metadata: hand block has %d landmarks (want %d); dropping the array",
			len(h.Landmarks), HandLandmarkCount)
		h.Landmarks = nil
	}
}

// HandLandmarkCount is MediaPipe's hand joint count. The array is positional,
// so this is a correctness constant, not a tunable.
const HandLandmarkCount = 21

func clamp01(v float64) float64 {
	if v < 0 || v != v { // negative, or NaN
		return 0
	}
	if v > 1 {
		return 1
	}
	return v
}

// normalizeBox clamps a bbox into [0,width]x[0,height] and guarantees
// 0 <= x1 < x2 <= width (same for y). Returns false when the box cannot be
// salvaged - e.g. a zero-sized video frame, where no valid box exists.
//
// std::clamp-style helpers are not used because lo>hi is undefined behaviour
// there; the ordering is fixed explicitly, mirroring the agent's clampi().
func normalizeBox(b *[4]int, width, height int) bool {
	if width <= 0 || height <= 0 {
		return false
	}
	x1, y1, x2, y2 := b[0], b[1], b[2], b[3]
	if x2 < x1 {
		x1, x2 = x2, x1
	}
	if y2 < y1 {
		y1, y2 = y2, y1
	}
	x1 = clampInt(x1, 0, width-1)
	y1 = clampInt(y1, 0, height-1)
	x2 = clampInt(x2, x1+minBoxEdge, width)
	y2 = clampInt(y2, y1+minBoxEdge, height)
	b[0], b[1], b[2], b[3] = x1, y1, x2, y2
	return true
}

func clampInt(v, lo, hi int) int {
	if lo > hi {
		lo, hi = hi, lo
	}
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

// Validate reports whether a status message carries enough identity to store.
func (s *StatusMessage) Validate() error {
	if s.CameraID == "" {
		return fmt.Errorf("camera_id is required")
	}
	return nil
}

// Validate reports whether a frame message carries enough identity to store.
func (f *FrameMessage) Validate() error {
	if f.CameraID == "" {
		return fmt.Errorf("camera_id is required")
	}
	if f.VideoWidth <= 0 || f.VideoHeight <= 0 {
		return fmt.Errorf("video_width/video_height must be positive (got %dx%d)", f.VideoWidth, f.VideoHeight)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Read models (what the API returns)
// ---------------------------------------------------------------------------

// ObjectView is the stored form of one detection.
type ObjectView struct {
	Class      string       `json:"class"`
	Confidence float64      `json:"confidence"`
	TrackID    int          `json:"track_id"`
	BBox       [4]int       `json:"bbox"`
	Keypoints  [][3]float64 `json:"keypoints,omitempty"`
}

// FrameView is the latest inference result for one camera.
type FrameView struct {
	FrameID     int64 `json:"frame_id"`
	Timestamp   int64 `json:"timestamp"`
	VideoWidth  int   `json:"video_width"`
	VideoHeight int   `json:"video_height"`
	// AIMode is the mode the agent ran for this frame. Empty for frames pushed
	// by agents that predate the field; the web treats a missing mode as
	// "does not match" and drops the frame.
	AIMode      string       `json:"ai_mode,omitempty"`
	ObjectCount int          `json:"object_count"`
	ReceivedAt  time.Time    `json:"received_at"`
	Objects     []ObjectView `json:"objects"`
	// Hand is the hand state for THIS frame, present only when the agent ran
	// ai-hand. It is read back from storage rather than recomputed, so the web
	// and the recorded log always agree.
	Hand *HandState `json:"hand,omitempty"`
}

// StatusView is the latest AI heartbeat for one camera.
type StatusView struct {
	Version       int       `json:"version"`
	Enable        bool      `json:"enable"`
	Running       bool      `json:"running"`
	FPS           float64   `json:"fps"`
	Model         string    `json:"model,omitempty"`
	Tracker       string    `json:"tracker,omitempty"`
	LastFrameID   int64     `json:"last_frame_id"`
	LastTimestamp int64     `json:"last_timestamp"`
	Processed     int64     `json:"processed"`
	WallClock     int64     `json:"wall_clock"`
	ReceivedAt    time.Time `json:"received_at"`
}

// Snapshot is everything the UI needs for one camera in a single call.
// Frame / Status are nil until the corresponding message kind has arrived -
// the agent can be running with AI disabled (status only) or with metadata
// enabled but AI off (status only), so neither may be assumed present.
type Snapshot struct {
	CameraID string      `json:"camera_id"`
	Frame    *FrameView  `json:"frame,omitempty"`
	Status   *StatusView `json:"status,omitempty"`
}

// ---------------------------------------------------------------------------
// Hand recorder read models
// ---------------------------------------------------------------------------

// HandLogRow is one recorded ai-hand frame, as exported.
type HandLogRow struct {
	// ID is the recorder's own append-only key, and the ONLY field here that is
	// monotonic for the life of the log. frame_id and timestamp are the agent's
	// clock, and it restarts from zero on every agent launch: a real 33-minute
	// capture contained (frame_id=77702, timestamp=2591322) immediately followed
	// by (frame_id=1, timestamp=700). An offline reader that sorts or de-dupes
	// on either of those silently interleaves two runs, so the export carries
	// the key that cannot do that.
	ID         int64     `json:"id"`
	FrameID    int64     `json:"frame_id"`
	Timestamp  int64     `json:"timestamp"`
	ReceivedAt time.Time `json:"received_at"`
	Detected   bool      `json:"detected"`
	Gesture    string    `json:"gesture,omitempty"`
	// Hand is the agent's block verbatim, so an exported file can be analysed
	// with the same code that reads the live protocol.
	Hand *HandState `json:"hand,omitempty"`
}

// HandSession is one contiguous run of recorded frames.
//
// The boundaries are produced by the server, not the agent: the recorder is
// append-only and always on, so "a session" is an analysis decision (see
// SplitHandSessions). Exporting the split parameters alongside the data is
// what makes the file reproducible.
type HandSession struct {
	Index          int            `json:"index"`
	StartFrameID   int64          `json:"start_frame_id"`
	EndFrameID     int64          `json:"end_frame_id"`
	StartTimestamp int64          `json:"start_timestamp"`
	EndTimestamp   int64          `json:"end_timestamp"`
	FrameCount     int            `json:"frame_count"`
	DetectedCount  int            `json:"detected_count"`
	Gestures       map[string]int `json:"gestures"`
}

// HandSessionList is the response of the sessions endpoint. It carries the
// split parameters back so a client can tell why it saw N sessions without
// having to know the server's default.
type HandSessionList struct {
	CameraID  string        `json:"camera_id"`
	GapMS     int           `json:"gap_ms"`
	TotalRows int           `json:"total_rows"`
	Sessions  []HandSession `json:"sessions"`
}

// HandSessionFile is the exported hand_session_*.json body.
type HandSessionFile struct {
	Version    int          `json:"version"`
	CameraID   string       `json:"camera_id"`
	ExportedAt time.Time    `json:"exported_at"`
	Session    HandSession  `json:"session"`
	GapMS      int          `json:"gap_ms"`
	Frames     []HandLogRow `json:"frames"`
}
