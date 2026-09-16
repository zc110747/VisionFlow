package camera

import "time"

// Status represents the lifecycle state of a camera as tracked by the server.
type Status string

const (
	StatusOnline     Status = "online"
	StatusOffline    Status = "offline"
	StatusConnecting Status = "connecting"
	StatusError      Status = "error"
)

// Camera is the persisted representation plus a few computed/transport fields.
type Camera struct {
	ID         string     `json:"id"`
	Name       string     `json:"name"`
	StreamPath string     `json:"stream_path"`
	DeviceIP   string     `json:"device_ip,omitempty"`
	Status     Status     `json:"status"`
	Resolution string     `json:"resolution,omitempty"`
	FPS        int        `json:"fps"`
	Bitrate    int        `json:"bitrate"`
	CreatedAt  time.Time  `json:"created_at"`
	UpdatedAt  time.Time  `json:"updated_at"`
	LastSeen   *time.Time `json:"last_seen,omitempty"`

	// RTSPURL is filled in by the API layer from config; not stored in DB.
	RTSPURL string `json:"rtsp_url,omitempty"`
}

// Valid reports whether the status is one of the known values.
func (s Status) Valid() bool {
	switch s {
	case StatusOnline, StatusOffline, StatusConnecting, StatusError:
		return true
	default:
		return false
	}
}
