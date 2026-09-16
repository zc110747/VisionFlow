package api

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"video-server/internal/config"
)

// hlsFailTransport returns a transport error for the first n calls, then a 200
// with the given body. Exercises hlsProxy's transient retry path without a real
// MediaMTX.
type hlsFailTransport struct {
	fails int
	calls int
	body  string
}

func (t *hlsFailTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	t.calls++
	if t.calls <= t.fails {
		return nil, io.EOF // simulate connection refused / reset
	}
	return &http.Response{
		StatusCode: http.StatusOK,
		Body:       io.NopCloser(strings.NewReader(t.body)),
		Header:     make(http.Header),
	}, nil
}

func TestHLSProxyRetriesTransientFailure(t *testing.T) {
	tr := &hlsFailTransport{fails: 2, body: "#EXTM3U"}
	orig := hlsClient
	hlsClient = &http.Client{Transport: tr}
	defer func() { hlsClient = orig }()

	cfg := &config.Config{}
	cfg.MediaMTX.Bind = "127.0.0.1"
	cfg.MediaMTX.HLSPort = 8888
	h := &Handler{cfg: cfg}

	req := httptest.NewRequest(http.MethodGet, "/hls/cam01/index.m3u8", nil)
	req.SetPathValue("stream", "cam01")
	req.SetPathValue("file", "index.m3u8")
	rec := httptest.NewRecorder()

	h.hlsProxy(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d (body=%s)", rec.Code, rec.Body.String())
	}
	if rec.Body.String() != "#EXTM3U" {
		t.Fatalf("got body %q, want #EXTM3U", rec.Body.String())
	}
	if tr.calls != 3 {
		t.Fatalf("expected 3 upstream calls (2 fails + 1 success), got %d", tr.calls)
	}
}

func TestHLSProxyGivesUpAfterRetries(t *testing.T) {
	tr := &hlsFailTransport{fails: 100, body: "x"}
	orig := hlsClient
	hlsClient = &http.Client{Transport: tr}
	defer func() { hlsClient = orig }()

	cfg := &config.Config{}
	cfg.MediaMTX.Bind = "127.0.0.1"
	cfg.MediaMTX.HLSPort = 8888
	h := &Handler{cfg: cfg}

	req := httptest.NewRequest(http.MethodGet, "/hls/cam01/index.m3u8", nil)
	req.SetPathValue("stream", "cam01")
	req.SetPathValue("file", "index.m3u8")
	rec := httptest.NewRecorder()

	h.hlsProxy(rec, req)

	if rec.Code != http.StatusBadGateway {
		t.Fatalf("expected 502 after exhausting retries, got %d", rec.Code)
	}
	if tr.calls != 4 {
		t.Fatalf("expected exactly 4 attempts, got %d", tr.calls)
	}
}
