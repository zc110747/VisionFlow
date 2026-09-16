package dbutil

import (
	"errors"
	"testing"
)

func TestIsBusyMatchesKnownForms(t *testing.T) {
	cases := []struct {
		name string
		err  error
		want bool
	}{
		{"nil", nil, false},
		{"generic", errors.New("boom"), false},
		{"locked lowercase", errors.New("database is locked"), true},
		{"SQLITE_BUSY", errors.New("SQLITE_BUSY: database is locked"), true},
		{"code 5", errors.New("database is locked (5) (SQLITE_BUSY)"), true},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := IsBusy(c.err); got != c.want {
				t.Fatalf("IsBusy(%v) = %v, want %v", c.err, got, c.want)
			}
		})
	}
}

func TestRetryOnBusyRetriesOnlyOnBusy(t *testing.T) {
	// A non-busy error must be returned immediately without any retry.
	calls := 0
	err := RetryOnBusy(5, func() error {
		calls++
		return errors.New("permanent failure")
	})
	if calls != 1 {
		t.Fatalf("expected 1 call for non-busy error, got %d", calls)
	}
	if err == nil {
		t.Fatal("expected the error to be returned")
	}
}

func TestRetryOnBusySucceedsAfterTransientBusy(t *testing.T) {
	calls := 0
	err := RetryOnBusy(5, func() error {
		calls++
		if calls < 3 {
			return errors.New("database is locked (5) (SQLITE_BUSY)")
		}
		return nil
	})
	if err != nil {
		t.Fatalf("expected success, got %v", err)
	}
	if calls != 3 {
		t.Fatalf("expected exactly 3 calls (2 busy + 1 success), got %d", calls)
	}
}

func TestRetryOnBusyExhaustsAttempts(t *testing.T) {
	calls := 0
	err := RetryOnBusy(3, func() error {
		calls++
		return errors.New("database is locked")
	})
	if err == nil {
		t.Fatal("expected final busy error to be returned")
	}
	if calls != 3 {
		t.Fatalf("expected 3 attempts, got %d", calls)
	}
}
