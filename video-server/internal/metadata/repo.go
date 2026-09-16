package metadata

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"time"

	"video-server/internal/dbutil"
	"video-server/internal/logger"
)

// Repository persists AI metadata pushed by camera-agents.
//
// Storage is deliberately split by update pattern:
//
//	ai_status  one row per camera, overwritten on every heartbeat  (latest only)
//	ai_frame   one row per camera, overwritten on every result     (latest only)
//	ai_object  the latest frame's detected objects, one set per camera (latest only)
//
// All three tables hold ONLY the latest result per camera. The web reads
// GET /metadata once per poll and must never scan a growing history. ai_object
// is replaced (delete + insert) on every frame so a reused frame_id - the agent
// counter resets across restarts - can never return a stale prior run's objects
// (e.g. pose skeletons leaking into person-detect mode).
type Repository struct {
	db *sql.DB
}

// NewRepository builds a metadata repository. The retention/pruneEvery args are
// retained only for call-site compatibility; ai_object now holds just the latest
// frame's objects, so no history or pruning is kept.
func NewRepository(db *sql.DB, _ /*retention*/, _ /*pruneEvery*/ int) *Repository {
	return &Repository{db: db}
}

// Migrate creates the metadata tables. It is called from the shared schema
// migration so a fresh database is ready in one step.
func Migrate(db *sql.DB) error {
	stmts := []string{
		`CREATE TABLE IF NOT EXISTS ai_status (
			camera_id      TEXT PRIMARY KEY,
			version        INTEGER NOT NULL DEFAULT 0,
			ai_enable      INTEGER NOT NULL DEFAULT 0,
			ai_running     INTEGER NOT NULL DEFAULT 0,
			ai_fps         REAL    NOT NULL DEFAULT 0,
			model          TEXT,
			tracker        TEXT,
			last_frame_id  INTEGER NOT NULL DEFAULT 0,
			last_timestamp INTEGER NOT NULL DEFAULT 0,
			processed      INTEGER NOT NULL DEFAULT 0,
			wall_clock     INTEGER NOT NULL DEFAULT 0,
			received_at    TEXT NOT NULL,
			updated_at     TEXT NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS ai_frame (
			camera_id     TEXT PRIMARY KEY,
			frame_id      INTEGER NOT NULL,
			timestamp     INTEGER NOT NULL,
			video_width   INTEGER NOT NULL DEFAULT 0,
			video_height  INTEGER NOT NULL DEFAULT 0,
			ai_mode       TEXT,
			object_count  INTEGER NOT NULL DEFAULT 0,
			received_at   TEXT NOT NULL,
			updated_at    TEXT NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS ai_object (
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
			keypoints   TEXT,
			received_at TEXT    NOT NULL
		)`,
		// Desired AI mode per camera (web UI -> agent poller). Overwritten on
		// every POST /api/cameras/{id}/aimode.
		`CREATE TABLE IF NOT EXISTS ai_aimode (
			camera_id TEXT PRIMARY KEY,
			mode      TEXT NOT NULL
		)`,
		// Serves the latest-frame lookup.
		`CREATE INDEX IF NOT EXISTS idx_ai_object_camera_id ON ai_object(camera_id, id)`,
	}
	for _, s := range stmts {
		if _, err := db.Exec(s); err != nil {
			return fmt.Errorf("migrate metadata: %w", err)
		}
	}
	// Forward-compatible: an existing joint DB (or any upgraded deployment) may
	// predate the keypoints column that pose models introduced. CREATE TABLE IF
	// NOT EXISTS leaves the old table untouched, so every metadata INSERT would
	// fail with "table ai_object has no column named keypoints". Upgrade the
	// schema in place instead of forcing a manual DB wipe - the column is simply
	// added when absent.
	if err := addColumnIfMissing(db, "ai_object", "keypoints", "TEXT"); err != nil {
		return err
	}
	// Same forward-compat for the per-frame AI mode column introduced to let
	// the web drop transition frames: older joint DBs have ai_frame without it.
	if err := addColumnIfMissing(db, "ai_frame", "ai_mode", "TEXT"); err != nil {
		return err
	}
	return nil
}

// addColumnIfMissing makes a schema forward-compatible with databases created
// by older binaries. PRAGMA table_info reports the live columns; we ALTER only
// when the column is genuinely absent, so calling this on a fresh DB is a no-op.
func addColumnIfMissing(db *sql.DB, table, col, typ string) error {
	rows, err := db.Query("PRAGMA table_info(" + table + ")")
	if err != nil {
		return fmt.Errorf("pragma table_info(%s): %w", table, err)
	}
	defer rows.Close()
	found := false
	for rows.Next() {
		var cid int
		var name, ctype string
		var notNull int
		var dflt, pk interface{}
		if err := rows.Scan(&cid, &name, &ctype, &notNull, &dflt, &pk); err != nil {
			return fmt.Errorf("scan pragma %s: %w", table, err)
		}
		if name == col {
			found = true
			break
		}
	}
	if found {
		return nil
	}
	if _, err := db.Exec(fmt.Sprintf("ALTER TABLE %s ADD COLUMN %s %s", table, col, typ)); err != nil {
		return fmt.Errorf("alter %s add %s: %w", table, col, err)
	}
	return nil
}

const timeFmt = time.RFC3339Nano

// SaveFrame stores one inference result: it upserts the per-camera "latest
// frame" row and appends the detected objects to the bounded history.
// Transient SQLITE_BUSY (the agent posts many frames per second while the web
// reads concurrently) is retried so a momentary writer collision never becomes
// a 500 on the ingest endpoint.
func (r *Repository) SaveFrame(f *FrameMessage, received time.Time) error {
	return dbutil.RetryOnBusy(5, func() error {
		return r.saveFrameOnce(f, received)
	})
}

func (r *Repository) saveFrameOnce(f *FrameMessage, received time.Time) error {
	now := received.UTC().Format(timeFmt)
	recv := now

	tx, err := r.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	if _, err := tx.Exec(`
		INSERT INTO ai_frame (camera_id,frame_id,timestamp,video_width,video_height,ai_mode,object_count,received_at,updated_at)
		VALUES (?,?,?,?,?,?,?,?,?)
		ON CONFLICT(camera_id) DO UPDATE SET
			frame_id=excluded.frame_id, timestamp=excluded.timestamp,
			video_width=excluded.video_width, video_height=excluded.video_height,
			ai_mode=excluded.ai_mode,
			object_count=excluded.object_count,
			received_at=excluded.received_at, updated_at=excluded.updated_at`,
		f.CameraID, f.FrameID, f.Timestamp, f.VideoWidth, f.VideoHeight,
		f.AIMode, len(f.Objects), recv, now); err != nil {
		return fmt.Errorf("upsert ai_frame: %w", err)
	}

	// ai_object holds ONLY this frame's objects for the camera (mirroring the
	// "latest only" contract of ai_frame). Deleting first is what makes the
	// lookup in Latest() unambiguous: the agent's frame_id is NOT globally unique
	// (it resets across restarts), so a naive append + "WHERE frame_id = ?" join
	// would return stale objects from a previous run - e.g. pose skeletons from
	// when the mode was ai-y-pose composited onto a current ai-y frame. Replacing
	// the set on every frame prevents that leak entirely.
	if _, err := tx.Exec(`DELETE FROM ai_object WHERE camera_id = ?`, f.CameraID); err != nil {
		return fmt.Errorf("clear ai_object: %w", err)
	}

	ins, err := tx.Prepare(`
		INSERT INTO ai_object (camera_id,frame_id,class,confidence,track_id,x1,y1,x2,y2,keypoints,received_at)
		VALUES (?,?,?,?,?,?,?,?,?,?,?)`)
	if err != nil {
		return fmt.Errorf("prepare ai_object: %w", err)
	}
	defer ins.Close()
	for _, o := range f.Objects {
		var kpJSON []byte
		if len(o.Keypoints) > 0 {
			if b, merr := json.Marshal(o.Keypoints); merr == nil {
				kpJSON = b
			}
		}
		if _, err := ins.Exec(f.CameraID, f.FrameID, o.Class, o.Confidence, o.TrackID,
			o.BBox[0], o.BBox[1], o.BBox[2], o.BBox[3],
			nullBytes(kpJSON), recv); err != nil {
			return fmt.Errorf("insert ai_object: %w", err)
		}
	}

	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit frame: %w", err)
	}
	return nil
}

// SaveStatus stores one AI heartbeat (latest per camera). Retried on transient
// lock contention for the same reason as SaveFrame.
func (r *Repository) SaveStatus(s *StatusMessage, received time.Time) error {
	now := received.UTC().Format(timeFmt)
	return dbutil.RetryOnBusy(3, func() error {
		_, err := r.db.Exec(`
			INSERT INTO ai_status
				(camera_id,version,ai_enable,ai_running,ai_fps,model,tracker,
				 last_frame_id,last_timestamp,processed,wall_clock,received_at,updated_at)
			VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)
			ON CONFLICT(camera_id) DO UPDATE SET
				version=excluded.version, ai_enable=excluded.ai_enable,
				ai_running=excluded.ai_running, ai_fps=excluded.ai_fps,
				model=excluded.model, tracker=excluded.tracker,
				last_frame_id=excluded.last_frame_id,
				last_timestamp=excluded.last_timestamp,
				processed=excluded.processed, wall_clock=excluded.wall_clock,
				received_at=excluded.received_at, updated_at=excluded.updated_at`,
			s.CameraID, s.Version, boolInt(s.AI.Enable), boolInt(s.AI.Running), s.AI.FPS,
			nullStr(s.AI.Model), nullStr(s.AI.Tracker),
			s.AI.LastFrameID, s.AI.LastTimestamp, s.AI.Processed, s.WallClock, now, now)
		if err != nil {
			return fmt.Errorf("upsert ai_status: %w", err)
		}
		return nil
	})
}

// Latest returns the newest frame + status for one camera. Both may be nil when
// nothing has arrived yet; that is not an error so the endpoint can answer 200
// with an empty snapshot instead of making the UI handle 404s.
func (r *Repository) Latest(cameraID string) (Snapshot, error) {
	snap := Snapshot{CameraID: cameraID}

	var (
		frameID, ts    int64
		w, h, objCount int
		aiMode         string
		receivedAt     string
	)
	err := dbutil.RetryOnBusy(3, func() error {
		return r.db.QueryRow(`
			SELECT frame_id,timestamp,video_width,video_height,ai_mode,object_count,received_at
			FROM ai_frame WHERE camera_id = ?`, cameraID).
			Scan(&frameID, &ts, &w, &h, &aiMode, &objCount, &receivedAt)
	})
	switch {
	case err == nil:
		objs, oerr := r.objects(cameraID, frameID)
		if oerr != nil {
			return snap, oerr
		}
		snap.Frame = &FrameView{
			FrameID:     frameID,
			Timestamp:   ts,
			VideoWidth:  w,
			VideoHeight: h,
			AIMode:      aiMode,
			ObjectCount: objCount,
			ReceivedAt:  parseTime(receivedAt),
			Objects:     objs,
		}
	case err == sql.ErrNoRows:
		// no frames yet - fall through, status may still exist
	default:
		return snap, fmt.Errorf("read ai_frame: %w", err)
	}

	st, err := r.status(cameraID)
	switch {
	case err == nil:
		snap.Status = st
	case err == sql.ErrNoRows:
		// neither kind has arrived yet
	default:
		return snap, fmt.Errorf("read ai_status: %w", err)
	}
	return snap, nil
}

// List returns a snapshot for every camera that has ever sent metadata, so the
// UI can render one card per device without knowing their ids up front.
func (r *Repository) List() ([]Snapshot, error) {
	ids, err := r.CameraIDs()
	if err != nil {
		return nil, err
	}
	out := make([]Snapshot, 0, len(ids))
	for _, id := range ids {
		s, err := r.Latest(id)
		if err != nil {
			return nil, err
		}
		out = append(out, s)
	}
	return out, nil
}

// CameraIDs returns every camera id present in either metadata table, most
// recently updated first (bounded, so a stuck camera cannot starve the rest).
func (r *Repository) CameraIDs() ([]string, error) {
	var rows *sql.Rows
	if err := dbutil.RetryOnBusy(3, func() error {
		var e error
		rows, e = r.db.Query(`
			SELECT camera_id, MAX(updated_at) AS u FROM (
				SELECT camera_id, updated_at FROM ai_frame
				UNION ALL
				SELECT camera_id, updated_at FROM ai_status
			) GROUP BY camera_id ORDER BY u DESC`)
		return e
	}); err != nil {
		return nil, fmt.Errorf("list metadata cameras: %w", err)
	}
	defer rows.Close()
	var ids []string
	for rows.Next() {
		var (
			id string
			// MAX(updated_at) is only used for ordering; its value is not needed.
			lastUpdate sql.NullString
		)
		if err := rows.Scan(&id, &lastUpdate); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

func (r *Repository) status(cameraID string) (*StatusView, error) {
	var (
		s               StatusView
		enable, running int
		model, tracker  sql.NullString
		receivedAt      string
	)
	err := dbutil.RetryOnBusy(3, func() error {
		return r.db.QueryRow(`
			SELECT version,ai_enable,ai_running,ai_fps,model,tracker,
			       last_frame_id,last_timestamp,processed,wall_clock,received_at
			FROM ai_status WHERE camera_id = ?`, cameraID).
			Scan(&s.Version, &enable, &running, &s.FPS, &model, &tracker,
				&s.LastFrameID, &s.LastTimestamp, &s.Processed, &s.WallClock, &receivedAt)
	})
	if err != nil {
		return nil, err
	}
	s.Enable = enable != 0
	s.Running = running != 0
	s.Model = model.String
	s.Tracker = tracker.String
	s.ReceivedAt = parseTime(receivedAt)
	return &s, nil
}

func (r *Repository) objects(cameraID string, frameID int64) ([]ObjectView, error) {
	var rows *sql.Rows
	if err := dbutil.RetryOnBusy(3, func() error {
		var e error
		rows, e = r.db.Query(`
			SELECT class,confidence,track_id,x1,y1,x2,y2,keypoints
			FROM ai_object WHERE camera_id = ? AND frame_id = ? ORDER BY id ASC`,
			cameraID, frameID)
		return e
	}); err != nil {
		return nil, fmt.Errorf("read ai_object: %w", err)
	}
	defer rows.Close()
	out := []ObjectView{}
	for rows.Next() {
		var o ObjectView
		var kpJSON sql.NullString
		if err := rows.Scan(&o.Class, &o.Confidence, &o.TrackID,
			&o.BBox[0], &o.BBox[1], &o.BBox[2], &o.BBox[3], &kpJSON); err != nil {
			return nil, err
		}
		if kpJSON.Valid && kpJSON.String != "" {
			if uerr := json.Unmarshal([]byte(kpJSON.String), &o.Keypoints); uerr != nil {
				logger.Warn("metadata: bad keypoints json for %s frame %d: %v",
					cameraID, frameID, uerr)
			}
		}
		out = append(out, o)
	}
	return out, rows.Err()
}

// DeleteCamera removes every metadata row for one camera. Used when the
// camera itself is deleted, so the DB does not keep orphaned AI rows around.
func (r *Repository) DeleteCamera(cameraID string) error {
	if _, err := r.db.Exec(`DELETE FROM ai_status WHERE camera_id = ?`, cameraID); err != nil {
		return err
	}
	if _, err := r.db.Exec(`DELETE FROM ai_frame  WHERE camera_id = ?`, cameraID); err != nil {
		return err
	}
	if _, err := r.db.Exec(`DELETE FROM ai_object WHERE camera_id = ?`, cameraID); err != nil {
		return err
	}
	if _, err := r.db.Exec(`DELETE FROM ai_aimode WHERE camera_id = ?`, cameraID); err != nil {
		return err
	}
	return nil
}

// GetAIMode returns the desired AI mode for a camera. When no choice has been
// made yet it returns the default (AIModeDetect) and a nil error, so the agent
// poller and the UI always get a usable value.
func (r *Repository) GetAIMode(cameraID string) (string, error) {
	var mode string
	err := dbutil.RetryOnBusy(3, func() error {
		return r.db.QueryRow(`SELECT mode FROM ai_aimode WHERE camera_id = ?`, cameraID).
			Scan(&mode)
	})
	switch {
	case err == sql.ErrNoRows:
		return DefaultAIMode, nil
	case err != nil:
		return DefaultAIMode, fmt.Errorf("read ai_aimode: %w", err)
	}
	if !ValidAIMode(mode) {
		return DefaultAIMode, nil
	}
	return mode, nil
}

// SetAIMode records the desired AI mode. Invalid values are rejected so the
// DB can never hold a mode the agent poller does not understand.
func (r *Repository) SetAIMode(cameraID, mode string) error {
	if !ValidAIMode(mode) {
		return fmt.Errorf("invalid ai mode %q (want ai-off|ai-y|ai-y-pose)", mode)
	}
	return dbutil.RetryOnBusy(3, func() error {
		if _, err := r.db.Exec(`
			INSERT INTO ai_aimode (camera_id, mode) VALUES (?, ?)
			ON CONFLICT(camera_id) DO UPDATE SET mode = excluded.mode`,
			cameraID, mode); err != nil {
			return fmt.Errorf("upsert ai_aimode: %w", err)
		}
		return nil
	})
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func nullStr(s string) any {
	if s == "" {
		return nil
	}
	return s
}

// nullBytes returns nil for a nil/empty slice so the column stores NULL
// instead of "null"/"[]"; otherwise it stores the already-marshalled bytes.
func nullBytes(b []byte) any {
	if len(b) == 0 {
		return nil
	}
	return b
}

// parseTime is lenient on purpose: a timestamp written by an older build must
// not turn a metadata read into a 500.
func parseTime(s string) time.Time {
	if t, err := time.Parse(timeFmt, s); err == nil {
		return t
	}
	if t, err := time.Parse(time.RFC3339, s); err == nil {
		return t
	}
	return time.Time{}
}
