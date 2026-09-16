// Package webstatic embeds the built Vue frontend (web/dist) into the Go binary
// so a single `video-server` executable serves the HTTP API and the Web UI.
//
// NOTE: web/dist is produced by `npm run build` in the web/ directory. The Go
// build will fail if dist/ does not exist yet.
package webstatic

import "embed"

//go:embed all:dist
var Dist embed.FS
