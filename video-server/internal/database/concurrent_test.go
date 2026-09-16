package database_test

import (
	"sync"
	"testing"
	"time"

	"video-server/internal/camera"
	"video-server/internal/database"
	"video-server/internal/dbutil"
	"video-server/internal/metadata"
)

// TestConcurrentReadWriteNoSQLITEBUSY reproduces the production incident:
// the agent hammers SaveFrame/SaveStatus while the web polls Latest/GetAIMode
// and the monitor upserts the cameras row. Before the fix the DB ran with the
// rollback journal and an unbounded connection pool, so every read collided
// with the constant writes and surfaced as "database is locked" -> a spurious
// 404 (GET /cameras/{id}) or 500 (GET /metadata). WAL + RetryOnBusy must keep
// every operation error-free under sustained contention.
func TestConcurrentReadWriteNoSQLITEBUSY(t *testing.T) {
	db, err := database.Open(t.TempDir() + "/joint.db")
	if err != nil {
		t.Fatalf("database.Open: %v", err)
	}
	defer db.Close()

	// The fix's precondition: Open must engage WAL so readers and the constant
	// writer no longer block each other. If this ever regresses the stress test
	// below becomes meaningless, so fail loudly here.
	var mode string
	if err := db.QueryRow("PRAGMA journal_mode").Scan(&mode); err != nil {
		t.Fatalf("query journal_mode: %v", err)
	}
	if mode != "wal" {
		t.Fatalf("expected WAL journal mode after Open, got %q (fix precondition missing)", mode)
	}

	meta := metadata.NewRepository(db, 0, 1)
	cam := camera.NewRepository(db)
	if err := cam.Create(camera.Camera{
		ID: "camera01", StreamPath: "camera01", Status: camera.StatusOnline,
	}); err != nil {
		t.Fatalf("create camera: %v", err)
	}
	if err := meta.SetAIMode("camera01", metadata.AIModePose); err != nil {
		t.Fatalf("set aimode: %v", err)
	}

	const (
		writers = 4
		readers = 8
		dur     = 2 * time.Second
	)
	start := time.Now()
	var wg sync.WaitGroup
	var mu sync.Mutex
	busy := 0
	fail := 0

	// Writers simulate the camera-agent: a write transaction per inference
	// result plus a heartbeat, at video-frame rate.
	for w := 0; w < writers; w++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			var fid int64
			for time.Since(start) < dur {
				fid++
				f := &metadata.FrameMessage{
					Version: 1, Type: metadata.TypeFrame, CameraID: "camera01",
					FrameID: fid, Timestamp: fid * 33, VideoWidth: 1280, VideoHeight: 720,
					Objects: []metadata.Object{
						{Class: "person", Confidence: 0.9, TrackID: 1, BBox: [4]int{1, 2, 3, 4}},
					},
				}
				if err := meta.SaveFrame(f, time.Now()); err != nil {
					record(&mu, &busy, &fail, err)
					return
				}
				s := &metadata.StatusMessage{
					Version: 1, Type: metadata.TypeStatus, CameraID: "camera01",
					AI: metadata.AIState{Enable: true, Running: true, FPS: 5, Model: "yolo11n-pose"},
				}
				if err := meta.SaveStatus(s, time.Now()); err != nil {
					record(&mu, &busy, &fail, err)
					return
				}
			}
		}(w)
	}

	// Readers simulate the web UI polling + the monitor upserting camera state.
	for r := 0; r < readers; r++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for time.Since(start) < dur {
				if _, err := meta.Latest("camera01"); err != nil {
					record(&mu, &busy, &fail, err)
					return
				}
				if _, err := meta.GetAIMode("camera01"); err != nil {
					record(&mu, &busy, &fail, err)
					return
				}
				if _, err := cam.Get("camera01"); err != nil {
					record(&mu, &busy, &fail, err)
					return
				}
				if _, err := cam.UpsertByStreamPath("camera01", camera.StatusOnline,
					time.Now(), "1280x720", 30, 4000); err != nil {
					record(&mu, &busy, &fail, err)
					return
				}
			}
		}()
	}

	wg.Wait()
	if busy > 0 {
		t.Errorf("observed %d SQLITE_BUSY errors under concurrency (must be 0 with WAL + RetryOnBusy)", busy)
	}
	if fail > 0 {
		t.Errorf("observed %d total DB errors under concurrency", fail)
	}
}

func record(mu *sync.Mutex, busy, fail *int, err error) {
	mu.Lock()
	defer mu.Unlock()
	*fail++
	if dbutil.IsBusy(err) {
		*busy++
	}
}
