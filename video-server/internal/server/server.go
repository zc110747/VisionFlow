// Package server wires the MediaMTX manager, the monitor, the SQLite store and the
// HTTP server (REST API + embedded Web UI) into a single runnable unit.
package server

import (
	"context"
	"database/sql"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"strings"

	"video-server/internal/api"
	"video-server/internal/camera"
	"video-server/internal/config"
	"video-server/internal/logger"
	"video-server/internal/mediamtx"
	"video-server/internal/metadata"
	"video-server/internal/monitor"
	"video-server/internal/netiface"
	"video-server/web"
)

// Server is the top-level runtime container.
type Server struct {
	cfg        *config.Config
	db         *sql.DB
	repo       *camera.Repository
	meta       *metadata.Repository
	mtx        *mediamtx.Manager
	mon        *monitor.Monitor
	httpServer *http.Server
}

func New(cfg *config.Config, db *sql.DB, repo *camera.Repository, meta *metadata.Repository, mtx *mediamtx.Manager, mon *monitor.Monitor) *Server {
	return &Server{cfg: cfg, db: db, repo: repo, meta: meta, mtx: mtx, mon: mon}
}

// Start launches MediaMTX, the monitor and the HTTP server. It blocks until the
// server stops (error or shutdown).
func (s *Server) Start(ctx context.Context) error {
	if err := s.mtx.Start(ctx); err != nil {
		return fmt.Errorf("start mediamtx: %w", err)
	}
	go s.mon.Run(ctx)

	h := api.New(s.cfg, s.repo, s.mtx, s.db, s.meta)
	mux := http.NewServeMux()
	h.Register(mux)

	sub, err := fs.Sub(webstatic.Dist, "dist")
	if err != nil {
		logger.Warn("web UI not embedded (run 'npm run build' in web/ first): %v", err)
	} else {
		mux.Handle("/", spaHandler(sub))
	}

	// Listen explicitly (instead of ListenAndServe) so we can log the real
	// bound address and then print every URL a client can actually reach.
	ln, err := net.Listen("tcp", s.cfg.HTTPListenAddr())
	if err != nil {
		return fmt.Errorf("listen %s: %w", s.cfg.HTTPListenAddr(), err)
	}
	s.httpServer = &http.Server{Addr: ln.Addr().String(), Handler: mux}

	// Go reports a dual-stack wildcard bind as "[::]:port", which reads as if
	// the server were IPv6-only. Say what it actually means.
	listenDesc := s.cfg.HTTPListenAddr()
	if netiface.IsWildcard(s.cfg.Server.Bind) {
		listenDesc = fmt.Sprintf("%s (all local IPv4+IPv6 addresses)", s.cfg.HTTPListenAddr())
	}
	logger.Info("http server listening on %s  [socket=%s]", listenDesc, ln.Addr().String())
	printAccessBanner(s.cfg)

	if err := s.httpServer.Serve(ln); err != nil && err != http.ErrServerClosed {
		return err
	}
	return nil
}

// printAccessBanner logs every address the service can be reached on: the
// loopback URL for this machine plus one URL per LAN address, and the media
// endpoints cameras/players need. Without this the only visible URL is
// localhost, which is useless from another computer.
func printAccessBanner(cfg *config.Config) {
	sep := strings.Repeat("=", 74)
	line := strings.Repeat("-", 74)
	logger.Info("%s", sep)
	logger.Info(" video-server ready")
	logger.Info("%s", line)
	if netiface.IsWildcard(cfg.Server.Bind) {
		logger.Info(" HTTP listen   : %s  (same port on every local IP)", cfg.HTTPListenAddr())
	} else {
		logger.Info(" HTTP listen   : %s", cfg.HTTPListenAddr())
	}

	// Only list the addresses the listener can actually answer on. A
	// wildcard bind covers every local IP; a specific bind covers just that.
	bindIP := cfg.Server.Bind
	wildcard := netiface.IsWildcard(bindIP)
	loopbackOnly := netiface.IsLoopbackBind(bindIP)

	if loopbackOnly {
		logger.Info(" Web UI (local): http://127.0.0.1:%d/  (bind=%s: local only)", cfg.Server.HTTPPort, bindIP)
	} else {
		logger.Info(" Web UI (local): http://127.0.0.1:%d/", cfg.Server.HTTPPort)
	}

	if !loopbackOnly {
		// Filter first: a wildcard bind covers every local IP, a specific bind
		// only answers on that one, so the two cases list different URLs.
		var lan []netiface.Address
		for _, a := range netiface.LAN() {
			if wildcard || a.IP == bindIP {
				lan = append(lan, a)
			}
		}
		if len(lan) == 0 {
			logger.Warn(" no reachable IPv4 address detected - other machines cannot connect")
		}
		const maxShown = 8 // keep the banner readable on machines with many NICs
		for i, a := range lan {
			if i >= maxShown {
				logger.Info("                 ... and %d more (see GET /api/net/addresses)", len(lan)-maxShown)
				break
			}
			tag := ""
			if a.Virtual {
				tag = " [virtual NIC]"
			} else if a.LinkLocal {
				tag = " [link-local]"
			}
			logger.Info(" Web UI (LAN)  : http://%s:%d/   (%s)%s", a.IP, cfg.Server.HTTPPort, a.Interface, tag)
		}
	}

	logger.Info("%s", line)
	logger.Info(" RTSP push/pull: rtsp://%s:%d/<stream-path>", cfg.PublicHost(), cfg.RTSP.Port)
	logger.Info(" WebRTC play   : open the Web UI above (signaling proxied by the server, port %d)", cfg.WebRTC.Port)
	logger.Info(" HLS (optional): http://%s:%d/<stream-path>/index.m3u8", cfg.PublicHost(), cfg.MediaMTX.HLSPort)
	logger.Info(" MediaMTX API  : http://%s  (bind=%s, used by the monitor only)", cfg.APIListenAddr(), cfg.MediaMTX.APIBind)
	logger.Info("%s", line)
	logger.Info(" Media bind    : %s  -> RTSP %d / WebRTC %d / HLS %d",
		cfg.MediaMTX.Bind, cfg.RTSP.Port, cfg.WebRTC.Port, cfg.MediaMTX.HLSPort)
	if !loopbackOnly {
		logger.Info(" NOTE: if another machine cannot connect, allow the ports through the")
		logger.Info("       firewall first:  scripts\\firewall-add.bat %d %d %d %d",
			cfg.Server.HTTPPort, cfg.RTSP.Port, cfg.WebRTC.Port, cfg.MediaMTX.HLSPort)
	}
	logger.Info("%s", sep)
}

// Shutdown gracefully stops the HTTP server, MediaMTX and closes the database.
func (s *Server) Shutdown(ctx context.Context) {
	if s.httpServer != nil {
		_ = s.httpServer.Shutdown(ctx)
	}
	s.mtx.Stop()
	if s.db != nil {
		_ = s.db.Close()
	}
}

// spaHandler serves embedded static files and falls back to index.html for
// client-side routes (e.g. /camera/camera01) so the Vue router can handle them.
func spaHandler(fsys fs.FS) http.Handler {
	fileServer := http.FileServer(http.FS(fsys))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p := strings.TrimPrefix(r.URL.Path, "/")
		if p == "" {
			p = "index.html"
		}
		if _, err := fs.Stat(fsys, p); err != nil {
			data, e := fs.ReadFile(fsys, "index.html")
			if e != nil {
				http.NotFound(w, r)
				return
			}
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			_, _ = w.Write(data)
			return
		}
		fileServer.ServeHTTP(w, r)
	})
}
