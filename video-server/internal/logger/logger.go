// Package logger provides a tiny leveled logger (DEBUG/INFO/WARN/ERROR) that
// writes to an io.Writer (stdout by default, optionally a file via a MultiWriter).
package logger

import (
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"time"
)

type Level int

const (
	DEBUG Level = iota
	INFO
	WARN
	ERROR
)

func (l Level) String() string {
	switch l {
	case DEBUG:
		return "DEBUG"
	case INFO:
		return "INFO"
	case WARN:
		return "WARN"
	case ERROR:
		return "ERROR"
	default:
		return "?"
	}
}

var levelNames = map[string]Level{
	"debug": DEBUG,
	"info":  INFO,
	"warn":  WARN,
	"error": ERROR,
}

type Logger struct {
	mu  sync.Mutex
	out io.Writer
	lvl Level
}

// std is the package-level default logger used by the package functions.
var std = New("info", os.Stdout)

// New creates a logger. Unknown levels fall back to INFO. A nil writer defaults
// to os.Stdout.
func New(level string, out io.Writer) *Logger {
	l, ok := levelNames[strings.ToLower(strings.TrimSpace(level))]
	if !ok {
		l = INFO
	}
	if out == nil {
		out = os.Stdout
	}
	return &Logger{out: out, lvl: l}
}

// SetLevel updates the level of the default logger.
func SetLevel(level string) {
	l, ok := levelNames[strings.ToLower(strings.TrimSpace(level))]
	if !ok {
		l = INFO
	}
	std.mu.Lock()
	std.lvl = l
	std.mu.Unlock()
}

// SetOutput replaces the writer of the default logger (e.g. stdout + file).
func SetOutput(w io.Writer) {
	if w == nil {
		return
	}
	std.mu.Lock()
	std.out = w
	std.mu.Unlock()
}

func (l *Logger) logf(lvl Level, format string, args ...any) {
	if lvl < l.lvl {
		return
	}
	msg := fmt.Sprintf(format, args...)
	line := fmt.Sprintf("%s %-5s %s\n", time.Now().Format("2006-01-02 15:04:05"), lvl.String(), msg)
	l.mu.Lock()
	_, _ = l.out.Write([]byte(line))
	l.mu.Unlock()
}

func (l *Logger) Debug(format string, args ...any) { l.logf(DEBUG, format, args...) }
func (l *Logger) Info(format string, args ...any)  { l.logf(INFO, format, args...) }
func (l *Logger) Warn(format string, args ...any)  { l.logf(WARN, format, args...) }
func (l *Logger) Error(format string, args ...any) { l.logf(ERROR, format, args...) }

func Debug(format string, args ...any) { std.Debug(format, args...) }
func Info(format string, args ...any)  { std.Info(format, args...) }
func Warn(format string, args ...any)  { std.Warn(format, args...) }
func Error(format string, args ...any) { std.Error(format, args...) }
