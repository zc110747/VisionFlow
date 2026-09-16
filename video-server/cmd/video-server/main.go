// Command video-server is the single binary entrypoint for the central video
// server: it starts the database, MediaMTX, the camera monitor, the REST API and
// the embedded Web UI.
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"syscall"

	"video-server/internal/camera"
	"video-server/internal/config"
	"video-server/internal/database"
	"video-server/internal/logger"
	"video-server/internal/mediamtx"
	"video-server/internal/metadata"
	"video-server/internal/monitor"
	"video-server/internal/netiface"
	"video-server/internal/server"
)

const defaultConfigPath = "config/config.yaml"

func main() {
	// Flags first; a bare positional argument is still accepted as the config
	// path so existing scripts (scripts\run.bat <config>) keep working.
	cfgPath := defaultConfigPath
	bind := ""
	fs := flag.NewFlagSet("video-server", flag.ContinueOnError)
	fs.StringVar(&cfgPath, "config", defaultConfigPath, "path to the YAML config file")
	fs.StringVar(&bind, "bind", "", "override server.bind (e.g. 0.0.0.0 for the whole LAN, 127.0.0.1 for local only)")
	fs.Usage = func() {
		fmt.Fprintf(fs.Output(), "Usage: video-server [-config <file>] [-bind <addr>]\n\n")
		fs.PrintDefaults()
	}
	if err := fs.Parse(os.Args[1:]); err != nil {
		if err == flag.ErrHelp {
			os.Exit(0)
		}
		os.Exit(2)
	}
	// A positional argument only wins when -config was left at its default.
	if fs.NArg() > 0 && cfgPath == defaultConfigPath {
		cfgPath = fs.Arg(0)
	}

	cfg, err := config.Load(cfgPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "config error: %v\n", err)
		os.Exit(1)
	}
	if bind != "" {
		cfg.Server.Bind = bind
	}
	if netiface.IsWildcard(cfg.Server.Host) {
		// Re-resolve: the advertised host now follows an explicit -bind.
		cfg.Server.Host = "auto"
		cfg.RefreshPublicHost()
	}

	logger.SetLevel(cfg.Log.Level)
	if cfg.Log.File != "" {
		f, ferr := os.OpenFile(cfg.Log.File, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
		if ferr == nil {
			logger.SetOutput(io.MultiWriter(os.Stdout, f))
			defer f.Close()
		} else {
			logger.Warn("cannot open log file %s: %v", cfg.Log.File, ferr)
		}
	}

	logger.Info("video-server starting (config=%s)", cfgPath)

	db, err := database.Open(cfg.Database.Path)
	if err != nil {
		logger.Error("database open failed: %v", err)
		os.Exit(1)
	}
	repo := camera.NewRepository(db)
	// Metadata repo shares the same SQLite handle, so cameras and their AI
	// results can never drift apart across a backup/restore.
	meta := metadata.NewRepository(db, cfg.Metadata.RetentionRows, 0)
	mtx := mediamtx.New(cfg)
	mon := monitor.New(repo, mtx, cfg)
	srv := server.New(cfg, db, repo, meta, mtx, mon)

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	go func() {
		if err := srv.Start(ctx); err != nil {
			logger.Error("server error: %v", err)
			cancel()
		}
	}()

	logger.Info("video-server running; press Ctrl+C to stop")
	<-ctx.Done()

	logger.Info("shutting down...")
	srv.Shutdown(context.Background())
	logger.Info("video-server stopped")
}
