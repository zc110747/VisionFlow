package config

import (
	"fmt"
	"net"
	"os"
	"strconv"

	"gopkg.in/yaml.v3"

	"video-server/internal/netiface"
)

// Config is the top-level Video Server configuration loaded from config/config.yaml.
type Config struct {
	Server   ServerConfig   `yaml:"server"`
	RTSP     RTSPConfig     `yaml:"rtsp"`
	WebRTC   WebRTCConfig   `yaml:"webrtc"`
	Database DatabaseConfig `yaml:"database"`
	MediaMTX MediaMTXConfig `yaml:"mediamtx"`
	Metadata MetadataConfig `yaml:"metadata"`
	Log      LogConfig      `yaml:"log"`
}

// MetadataConfig controls the AI-metadata ingest endpoint (POST /api/metadata)
// that camera-agents push their detection results and heartbeats to.
type MetadataConfig struct {
	// Enabled gates the endpoint. When false the route is registered but
	// answers 503, which makes the agent back off instead of hammering a
	// service the operator deliberately turned off.
	Enabled bool `yaml:"enabled"`
	// RetentionRows is how many detected-object rows are kept per camera.
	// The history table grows by one row per object per frame (about 5
	// frames/s x a handful of objects), so it must be bounded.
	// <= 0 disables pruning entirely, which WILL grow without bound.
	RetentionRows int `yaml:"retention_rows"`
	// MaxBodyBytes rejects oversized payloads before they are decoded, so a
	// broken or hostile agent cannot exhaust memory with one POST.
	MaxBodyBytes int64 `yaml:"max_body_bytes"`
	// RequireKnownCamera makes the endpoint reject messages for a camera that
	// the monitor has not registered yet. Default false on purpose: the
	// monitor polls MediaMTX every 3s, so metadata routinely arrives before
	// the camera row exists, and rejecting it would only make the agent
	// needlessly back off and drop frames.
	RequireKnownCamera bool `yaml:"require_known_camera"`
}

type ServerConfig struct {
	// Host is what goes into the URLs shown to clients (RTSP/WebRTC/HLS).
	// Leave it empty or set it to "auto" to advertise this machine's LAN
	// address, which is what makes the URLs work from another computer.
	Host     string `yaml:"host"`
	HTTPPort int    `yaml:"http_port"`
	// Bind is the listen address. Defaults to 0.0.0.0 so the same port is
	// served on every IPv4 address this machine has (LAN, Wi-Fi, VPN...).
	// Set it to 127.0.0.1 to keep the server local-only again.
	Bind string `yaml:"bind"`

	// publicHost is the resolved form of Host: wildcards are replaced by the
	// primary LAN address once, at load time, so URL building stays cheap.
	publicHost string
}

type RTSPConfig struct {
	Port int `yaml:"port"`
}

type WebRTCConfig struct {
	Port int `yaml:"port"`
}

type DatabaseConfig struct {
	Path string `yaml:"path"`
}

// MediaMTXConfig points to the MediaMTX binary and the generated config file.
// APIPort is the MediaMTX control API port (used for camera discovery).
// HLSPort is only used to build optional HLS URLs.
type MediaMTXConfig struct {
	Binary string `yaml:"binary"`
	Config string `yaml:"config"`
	// Bind is the listen address for the media ports (RTSP / WebRTC / HLS).
	// Defaults to 0.0.0.0 so cameras and players on the LAN can reach them.
	Bind string `yaml:"bind"`
	// APIBind is the listen address of the MediaMTX control API. It defaults
	// to 127.0.0.1 because only this server talks to it - there is no reason
	// to expose full stream control to the whole LAN.
	APIBind string `yaml:"api_bind"`
	APIPort int    `yaml:"api_port"`
	HLSPort int    `yaml:"hls_port"`
	// RTP/RTCP are the UDP ports MediaMTX receives interleaved/RTP media on.
	// They are configurable so two servers can run side by side (a second
	// instance would otherwise collide on the 8000/8001 defaults).
	RTPPort  int `yaml:"rtp_port"`
	RTCPPort int `yaml:"rtcp_port"`
	// ICEUDPPort / ICETCPPort are the WebRTC ICE listeners. TCP is enabled
	// alongside UDP on purpose: it is the only way WebRTC survives a network
	// that blocks UDP between wireless clients, which is the usual reason a
	// phone shows no picture while a desktop player is fine.
	ICEUDPPort int `yaml:"ice_udp_port"`
	ICETCPPort int `yaml:"ice_tcp_port"`
}

type LogConfig struct {
	Level string `yaml:"level"`
	File  string `yaml:"file"`
}

// Default returns a Config populated with sensible defaults. Any field present
// in the YAML file overrides the corresponding default.
func Default() *Config {
	return &Config{
		Server:   ServerConfig{Host: "auto", Bind: "0.0.0.0", HTTPPort: 8080},
		RTSP:     RTSPConfig{Port: 8554},
		WebRTC:   WebRTCConfig{Port: 8889},
		Database: DatabaseConfig{Path: "data/video.db"},
		MediaMTX: MediaMTXConfig{
			Binary: "./mediamtx/mediamtx", Config: "./config/mediamtx.yml",
			Bind: "0.0.0.0", APIBind: "127.0.0.1", APIPort: 9997, HLSPort: 8888,
			RTPPort: 8000, RTCPPort: 8001, ICEUDPPort: 8189, ICETCPPort: 8189,
		},
		Metadata: MetadataConfig{
			Enabled:            true,
			RetentionRows:      2000,
			MaxBodyBytes:       1 << 20, // 1 MiB
			RequireKnownCamera: false,
		},
		Log: LogConfig{Level: "info"},
	}
}

// Load reads the YAML file at path. If the file does not exist the defaults are
// returned (so the server can still boot). Missing keys keep their default value
// because defaults are applied before unmarshalling.
func Load(path string) (*Config, error) {
	cfg := Default()
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return cfg, nil
		}
		return nil, fmt.Errorf("read config %q: %w", path, err)
	}
	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config %q: %w", path, err)
	}
	applyDefaults(cfg)
	// Resolve the advertised host once: with host: auto (or 0.0.0.0) the LAN
	// address is baked in here, so every later URL build is a plain field read.
	cfg.RefreshPublicHost()
	return cfg, nil
}

func applyDefaults(cfg *Config) {
	if cfg.Server.HTTPPort == 0 {
		cfg.Server.HTTPPort = 8080
	}
	if cfg.Server.Bind == "" {
		// Bind to every IPv4 address so one port serves the whole LAN.
		cfg.Server.Bind = "0.0.0.0"
	}
	if cfg.Server.Host == "" {
		cfg.Server.Host = "auto"
	}
	if cfg.RTSP.Port == 0 {
		cfg.RTSP.Port = 8554
	}
	if cfg.WebRTC.Port == 0 {
		cfg.WebRTC.Port = 8889
	}
	if cfg.Database.Path == "" {
		cfg.Database.Path = "data/video.db"
	}
	if cfg.MediaMTX.Binary == "" {
		cfg.MediaMTX.Binary = "./mediamtx/mediamtx"
	}
	if cfg.MediaMTX.Config == "" {
		cfg.MediaMTX.Config = "./config/mediamtx.yml"
	}
	if cfg.MediaMTX.Bind == "" {
		cfg.MediaMTX.Bind = "0.0.0.0"
	}
	if cfg.MediaMTX.APIBind == "" {
		// Control API stays loopback-only: only this server needs it.
		cfg.MediaMTX.APIBind = "127.0.0.1"
	}
	if cfg.MediaMTX.APIPort == 0 {
		cfg.MediaMTX.APIPort = 9997
	}
	if cfg.MediaMTX.HLSPort == 0 {
		cfg.MediaMTX.HLSPort = 8888
	}
	if cfg.MediaMTX.RTPPort == 0 {
		cfg.MediaMTX.RTPPort = 8000
	}
	if cfg.MediaMTX.RTCPPort == 0 {
		cfg.MediaMTX.RTCPPort = 8001
	}
	if cfg.MediaMTX.ICEUDPPort == 0 {
		cfg.MediaMTX.ICEUDPPort = 8189
	}
	if cfg.MediaMTX.ICETCPPort == 0 {
		cfg.MediaMTX.ICETCPPort = 8189
	}
	if cfg.Log.Level == "" {
		cfg.Log.Level = "info"
	}
}

// PublicHost returns the host advertised to clients. It equals server.host for
// an explicit value, and the machine's primary LAN IPv4 when the config uses
// "auto"/"0.0.0.0" (resolved once at load time).
func (c *Config) PublicHost() string {
	if c.Server.publicHost != "" {
		return c.Server.publicHost
	}
	// Config built programmatically (tests) without going through Load.
	return netiface.ResolvePublicHost(c.Server.Host)
}

// RefreshPublicHost re-resolves the advertised host after Host/Bind were
// changed at runtime (e.g. by the -bind flag). A loopback bind downgrades the
// advertised host to 127.0.0.1, since a LAN address would be undialable.
func (c *Config) RefreshPublicHost() {
	host := c.Server.Host
	if netiface.IsWildcard(host) && netiface.IsLoopbackBind(c.Server.Bind) {
		host = "127.0.0.1"
	}
	c.Server.publicHost = netiface.ResolvePublicHost(host)
}

// HTTPListenAddr is the address the HTTP server binds to.
func (c *Config) HTTPListenAddr() string {
	return net.JoinHostPort(c.Server.Bind, strconv.Itoa(c.Server.HTTPPort))
}

// MediaListenAddr builds a MediaMTX listen address from its bind host + port.
func (c *Config) MediaListenAddr(port int) string {
	return net.JoinHostPort(c.MediaMTX.Bind, strconv.Itoa(port))
}

// APIListenAddr builds the MediaMTX control API listen address.
func (c *Config) APIListenAddr() string {
	return net.JoinHostPort(c.MediaMTX.APIBind, strconv.Itoa(c.MediaMTX.APIPort))
}

// HLSUpstreamAddr is the address THIS server dials to reach MediaMTX's HLS
// endpoint when proxying /hls/*. With the default wildcard bind that is
// loopback; if MediaMTX was pinned to a single interface we dial that one.
func (c *Config) HLSUpstreamAddr() string {
	host := "127.0.0.1"
	if !netiface.IsWildcard(c.MediaMTX.Bind) && !netiface.IsLoopbackBind(c.MediaMTX.Bind) {
		host = c.MediaMTX.Bind
	}
	return net.JoinHostPort(host, strconv.Itoa(c.MediaMTX.HLSPort))
}

// RTSPURL builds the public RTSP URL for a stream path, using the advertised
// (LAN) host so the URL works from another machine, not just from this box.
func (c *Config) RTSPURL(path string) string {
	return fmt.Sprintf("rtsp://%s:%d/%s", c.PublicHost(), c.RTSP.Port, path)
}

// APIAddr returns the MediaMTX control API address the server talks to (loopback).
func (c *Config) APIAddr() string {
	return fmt.Sprintf("127.0.0.1:%d", c.MediaMTX.APIPort)
}

// WebRTCAPIAddr returns the MediaMTX WebRTC signaling address (loopback).
func (c *Config) WebRTCAPIAddr() string {
	return fmt.Sprintf("127.0.0.1:%d", c.WebRTC.Port)
}
