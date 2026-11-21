// Package bmbotgo is the root of the Go module github.com/yshurik/ynotbit/bmbot/go: the plumbing that
// bots written in Go share. The packages are bmbot (cgo over libbmbot, build tag libbmbot), loop (the
// events of the library, the limits, the replies, and "done" only after the reply) and limit (token
// buckets in bounded memory). The module needs nothing but the standard library.
package bmbotgo
