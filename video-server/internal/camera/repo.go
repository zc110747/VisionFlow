package camera

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"

	"video-server/internal/dbutil"
)

// Repository provides CRUD and camera-discovery operations over the cameras table.
type Repository struct {
	db *sql.DB
}

func NewRepository(db *sql.DB) *Repository {
	return &Repository{db: db}
}

const timeFmt = time.RFC3339

func (r *Repository) scanRow(rows *sql.Row) (Camera, error) {
	var (
		c                 Camera
		deviceIP, res, ls sql.NullString
		created, updated  string
	)
	err := rows.Scan(
		&c.ID, &c.Name, &c.StreamPath, &deviceIP, &c.Status,
		&res, &c.FPS, &c.Bitrate, &created, &updated, &ls,
	)
	if err != nil {
		return c, err
	}
	c.DeviceIP = deviceIP.String
	c.Resolution = res.String
	if cs, e := time.Parse(timeFmt, created); e == nil {
		c.CreatedAt = cs
	}
	if us, e := time.Parse(timeFmt, updated); e == nil {
		c.UpdatedAt = us
	}
	if ls.Valid {
		if ts, e := time.Parse(timeFmt, ls.String); e == nil {
			c.LastSeen = &ts
		}
	}
	return c, nil
}

func (r *Repository) scanRows(rows *sql.Rows) ([]Camera, error) {
	defer rows.Close()
	var out []Camera
	for rows.Next() {
		var (
			c                 Camera
			deviceIP, res, ls sql.NullString
			created, updated  string
		)
		if err := rows.Scan(
			&c.ID, &c.Name, &c.StreamPath, &deviceIP, &c.Status,
			&res, &c.FPS, &c.Bitrate, &created, &updated, &ls,
		); err != nil {
			return nil, err
		}
		c.DeviceIP = deviceIP.String
		c.Resolution = res.String
		if cs, e := time.Parse(timeFmt, created); e == nil {
			c.CreatedAt = cs
		}
		if us, e := time.Parse(timeFmt, updated); e == nil {
			c.UpdatedAt = us
		}
		if ls.Valid {
			if ts, e := time.Parse(timeFmt, ls.String); e == nil {
				c.LastSeen = &ts
			}
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

// List returns all cameras ordered by name.
func (r *Repository) List() ([]Camera, error) {
	rows, err := r.db.Query(`
		SELECT id,name,stream_path,device_ip,status,resolution,fps,bitrate,created_at,updated_at,last_seen
		FROM cameras ORDER BY name ASC`)
	if err != nil {
		return nil, err
	}
	return r.scanRows(rows)
}

// Get returns the camera with the given id.
func (r *Repository) Get(id string) (Camera, error) {
	var c Camera
	err := dbutil.RetryOnBusy(3, func() error {
		row := r.db.QueryRow(`
			SELECT id,name,stream_path,device_ip,status,resolution,fps,bitrate,created_at,updated_at,last_seen
			FROM cameras WHERE id = ?`, id)
		var e error
		c, e = r.scanRow(row)
		return e
	})
	if errors.Is(err, sql.ErrNoRows) {
		return c, fmt.Errorf("camera %q not found", id)
	}
	return c, err
}

// GetByStreamPath returns the camera matching a stream path.
func (r *Repository) GetByStreamPath(path string) (Camera, error) {
	var c Camera
	err := dbutil.RetryOnBusy(3, func() error {
		row := r.db.QueryRow(`
			SELECT id,name,stream_path,device_ip,status,resolution,fps,bitrate,created_at,updated_at,last_seen
			FROM cameras WHERE stream_path = ?`, path)
		var e error
		c, e = r.scanRow(row)
		return e
	})
	if errors.Is(err, sql.ErrNoRows) {
		return c, fmt.Errorf("camera with stream_path %q not found", path)
	}
	return c, err
}

// Create inserts a new camera. id and stream_path are required.
func (r *Repository) Create(c Camera) error {
	if c.ID == "" {
		return fmt.Errorf("camera id is required")
	}
	if c.StreamPath == "" {
		c.StreamPath = c.ID
	}
	if c.Name == "" {
		c.Name = c.ID
	}
	if !c.Status.Valid() {
		c.Status = StatusOffline
	}
	now := time.Now().UTC().Format(timeFmt)
	_, err := r.db.Exec(`
		INSERT INTO cameras (id,name,stream_path,device_ip,status,resolution,fps,bitrate,created_at,updated_at,last_seen)
		VALUES (?,?,?,?,?,?,?,?,?,?,?)`,
		c.ID, c.Name, c.StreamPath, nullStr(c.DeviceIP), c.Status, nullStr(c.Resolution),
		c.FPS, c.Bitrate, now, now, nullStr(optTime(c.LastSeen)))
	return err
}

// Update patches the mutable fields of an existing camera (name, device_ip,
// status, resolution, fps, bitrate). id and stream_path are not changed.
func (r *Repository) Update(c Camera) error {
	now := time.Now().UTC().Format(timeFmt)
	res, err := r.db.Exec(`
		UPDATE cameras SET name=?, device_ip=?, status=?, resolution=?, fps=?, bitrate=?, updated_at=?
		WHERE id=?`,
		c.Name, nullStr(c.DeviceIP), c.Status, nullStr(c.Resolution),
		c.FPS, c.Bitrate, now, c.ID)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return fmt.Errorf("camera %q not found", c.ID)
	}
	return nil
}

// Delete removes a camera by id.
func (r *Repository) Delete(id string) error {
	res, err := r.db.Exec(`DELETE FROM cameras WHERE id = ?`, id)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return fmt.Errorf("camera %q not found", id)
	}
	return nil
}

// UpsertByStreamPath ensures a camera exists for the given RTSP stream path.
// If it already exists its status/last_seen are refreshed; otherwise it is
// auto-registered with name=path and status=online. This is the heart of the
// "camera auto-registration" feature.
//
// resolution is the "WxH" the publisher negotiated, as reported by MediaMTX.
// fps/bitrate are filled in by the monitor (ffprobe / bytesReceived delta).
// Empty/zero values are intentionally NOT overwritten: a scan that runs before
// the codec properties or stream probe are ready must not wipe good data.
func (r *Repository) UpsertByStreamPath(path string, status Status, lastSeen time.Time, resolution string, fps int, bitrate int) (Camera, error) {
	existing, err := r.GetByStreamPath(path)
	if err == nil {
		existing.Status = status
		ls := lastSeen.UTC().Format(timeFmt)
		now := time.Now().UTC().Format(timeFmt)

		// Build the SET clause dynamically: only known, non-empty values are
		// written, so a single scan can refresh any subset of the metrics.
		setCols := []string{"status=?", "last_seen=?", "updated_at=?"}
		args := []any{status, ls, now}
		if resolution != "" {
			setCols = append(setCols, "resolution=?")
			args = append(args, resolution)
			existing.Resolution = resolution
		}
		if fps > 0 {
			setCols = append(setCols, "fps=?")
			args = append(args, fps)
			existing.FPS = fps
		}
		if bitrate > 0 {
			setCols = append(setCols, "bitrate=?")
			args = append(args, bitrate)
			existing.Bitrate = bitrate
		}
		args = append(args, existing.ID)
		if uerr := dbutil.RetryOnBusy(3, func() error {
			_, e := r.db.Exec(
				`UPDATE cameras SET `+strings.Join(setCols, ", ")+` WHERE id=?`, args...)
			return e
		}); uerr != nil {
			return existing, uerr
		}
		existing.LastSeen = &lastSeen
		existing.UpdatedAt = time.Now().UTC()
		return existing, nil
	}

	c := Camera{
		ID:         path,
		Name:       path,
		StreamPath: path,
		Status:     status,
		Resolution: resolution,
		FPS:        fps,
		Bitrate:    bitrate,
		LastSeen:   &lastSeen,
	}
	if cerr := r.Create(c); cerr != nil {
		return c, cerr
	}
	return r.Get(path)
}

// MarkOfflineIfNotSeen flips cameras that have not been seen since threshold to
// offline. This implements the ONLINE -> OFFLINE timeout transition.
func (r *Repository) MarkOfflineIfNotSeen(threshold time.Time) error {
	return dbutil.RetryOnBusy(3, func() error {
		_, err := r.db.Exec(`
			UPDATE cameras SET status='offline', updated_at=?
			WHERE (last_seen IS NULL OR last_seen < ?) AND status != 'offline'`,
			time.Now().UTC().Format(timeFmt), threshold.UTC().Format(timeFmt))
		return err
	})
}

func nullStr(s string) any {
	if s == "" {
		return nil
	}
	return s
}

func optTime(t *time.Time) string {
	if t == nil {
		return ""
	}
	return t.UTC().Format(timeFmt)
}

// normalizeStatus coerces an arbitrary string to a valid Status, defaulting to
// offline. Used by the API to accept user-supplied status values.
func NormalizeStatus(s string) Status {
	s = strings.ToLower(strings.TrimSpace(s))
	switch Status(s) {
	case StatusOnline, StatusOffline, StatusConnecting, StatusError:
		return Status(s)
	default:
		return StatusOffline
	}
}
