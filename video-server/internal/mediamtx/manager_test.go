package mediamtx

import (
	"context"
	"errors"
	"io"
	"net/http"
	"strings"
	"testing"

	"video-server/internal/config"
)

// failNTimesTransport returns a transport error for the first n calls, then a
// 201 Created with the given body. It lets us exercise WebRTCOffer's transient
// retry path without a real MediaMTX process.
type failNTimesTransport struct {
	fails  int
	calls  int
	answer string
}

func (t *failNTimesTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	t.calls++
	if t.calls <= t.fails {
		return nil, errors.New("connection refused")
	}
	return &http.Response{
		StatusCode: http.StatusCreated,
		Body:       io.NopCloser(strings.NewReader(t.answer)),
		Header:     make(http.Header),
	}, nil
}

func newTestManager() *Manager {
	return &Manager{
		cfg:        &config.Config{},
		webrtcBase: "http://127.0.0.1:8889",
		httpClient: &http.Client{},
	}
}

func TestWebRTCOfferRetriesTransientFailure(t *testing.T) {
	tr := &failNTimesTransport{fails: 2, answer: "answer-sdp"}
	m := newTestManager()
	m.httpClient.Transport = tr

	ans, err := m.WebRTCOffer(context.Background(), "cam01", "offer-sdp")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if ans != "answer-sdp" {
		t.Fatalf("got answer %q, want answer-sdp", ans)
	}
	if tr.calls != 3 {
		t.Fatalf("expected 3 upstream calls (2 fails + 1 success), got %d", tr.calls)
	}
}

func TestWebRTCOfferGivesUpAfterRetries(t *testing.T) {
	tr := &failNTimesTransport{fails: 100, answer: "x"}
	m := newTestManager()
	m.httpClient.Transport = tr

	_, err := m.WebRTCOffer(context.Background(), "cam01", "offer")
	if err == nil {
		t.Fatal("expected error after exhausting retries")
	}
	if !strings.Contains(err.Error(), "after 4 tries") {
		t.Fatalf("expected 'after 4 tries' in error, got %v", err)
	}
	if tr.calls != 4 {
		t.Fatalf("expected exactly 4 attempts, got %d", tr.calls)
	}
}
