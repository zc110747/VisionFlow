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
)

// DefaultAIMode is reported by GET /aimode before any choice is made.
const DefaultAIMode = AIModeDetect

// ValidAIMode reports whether s is one of the three supported modes.
func ValidAIMode(s string) bool {
	switch s {
	case AIModeOff, AIModeDetect, AIModePose:
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
	// ("ai-y" / "ai-y-pose"). The web drops frames whose mode no longer matches
	// the selected mode - this is what prevents stale boxes during a switch.
	AIMode  string   `json:"ai_mode,omitempty"`
	Objects []Object `json:"objects"`
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
