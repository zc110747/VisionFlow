// Package dbutil holds small database-agnostic helpers shared by the camera
// and metadata repositories. It exists so every data path can treat a
// transient SQLite SQLITE_BUSY ("database is locked") the same way: retry it
// instead of returning it to a caller that would misread it as a hard error
// (e.g. a 404 "camera not found" or a 500 "store failed").
package dbutil

import (
	"strings"
	"time"
)

// IsBusy reports whether err is a transient SQLite contention error that is
// safe to retry. We match on the well-known strings and the numeric code so
// the check keeps working across driver versions and locales.
func IsBusy(err error) bool {
	if err == nil {
		return false
	}
	msg := err.Error()
	return strings.Contains(msg, "database is locked") ||
		strings.Contains(msg, "SQLITE_BUSY") ||
		strings.Contains(msg, "(5)")
}

// RetryOnBusy runs fn, retrying only while it returns a busy/locked error.
// Non-busy errors (including a final busy error after all attempts) are
// returned immediately. attempts is clamped to at least 1. The backoff grows
// linearly (20ms, 40ms, 60ms, ...) which is plenty for SQLite's sub-millisecond
// writer windows while staying well under the 5s busy_timeout budget.
func RetryOnBusy(attempts int, fn func() error) error {
	if attempts < 1 {
		attempts = 1
	}
	var last error
	for i := 0; i < attempts; i++ {
		last = fn()
		if last == nil || !IsBusy(last) {
			return last
		}
		time.Sleep(time.Duration(i+1) * 20 * time.Millisecond)
	}
	return last
}
