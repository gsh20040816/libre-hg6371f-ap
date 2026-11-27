//go:build !linux || !cgo

package main

// bcmTryExec is a no-op stub on non-Linux or non-CGO builds (e.g. macOS host tests).
func bcmTryExec(name string, args []string) (string, bool) {
	return "", false
}
