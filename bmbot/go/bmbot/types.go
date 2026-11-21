// Package bmbot is Go's side of libbmbot, the Bitmessage bot library with a C ABI (bmbot/bmbot.h).
// The calls are in bmbot.go, which needs cgo and the library and so is built only with the tag
// libbmbot; without it Open says so, and the rest of nickbot builds and tests with no C at all.
package bmbot

import (
	"errors"
	"fmt"
)

// Node is the "node" object of the configuration.
type Node struct {
	Bin             string `json:"bin"` // "auto", a path, or "" for no node here (always sent: the default is "auto")
	Peer            string `json:"peer,omitempty"`
	Proxy           string `json:"proxy,omitempty"`
	Public          bool   `json:"public"` // join the public network, else link to Peer alone
	Listen          bool   `json:"listen,omitempty"`
	Port            int    `json:"port,omitempty"`
	MaxOutgoing     int    `json:"max_outgoing,omitempty"`
	MaxIncoming     int    `json:"max_incoming,omitempty"`
	WatchdogMinutes int    `json:"watchdog_minutes,omitempty"`
	RetentionMB     int    `json:"retention_mb,omitempty"`
	RetentionDays   int    `json:"retention_days,omitempty"`
}

// Config is what bmbot_open takes (see bmbot.h).
type Config struct {
	DataDir     string `json:"data_dir"`
	PhraseFile  string `json:"phrase_file,omitempty"`
	KeysFile    string `json:"keys_file,omitempty"` // the two private keys of an address no phrase made
	Phrase      string `json:"phrase,omitempty"`    // public: for tests and demos
	Label       string `json:"label,omitempty"`
	HangSeconds int    `json:"hang_seconds,omitempty"`
	StepMs      int    `json:"step_ms,omitempty"`
	Node        Node   `json:"node"`
}

// NodeStatus is the part of bmbot_status that nickbot looks at.
type NodeStatus struct {
	Managed          bool `json:"managed"`
	Running          bool `json:"running"`
	Peers            int  `json:"peers"`
	Objects          int  `json:"objects"`
	Pending          int  `json:"pending"`
	StatusAge        int  `json:"status_age"`
	Restarts         int  `json:"restarts"`
	WatchdogRestarts int  `json:"watchdog_restarts"`
}

// Status is what bmbot_status says.
type Status struct {
	Time     int64      `json:"time"`
	Uptime   int64      `json:"uptime"`
	Address  string     `json:"address"`
	Network  string     `json:"network"`
	Working  bool       `json:"working"`
	UpToDate bool       `json:"up_to_date"`
	Node     NodeStatus `json:"node"`
}

// Error codes of bmbot.h.
const (
	CodeArgument = -1
	CodeState    = -2
	CodeAuth     = -3
	CodeIO       = -4
	CodeNetwork  = -5
	CodeInternal = -6
)

// Error is a failure the library reported, with its code and its English reason.
type Error struct {
	Code int
	Text string
}

func (e *Error) Error() string { return fmt.Sprintf("bmbot: %s (code %d)", e.Text, e.Code) }

// Permanent says that trying again with the same arguments cannot work.
func (e *Error) Permanent() bool { return e.Code == CodeArgument }

// ErrNotBuilt is what Open returns in a build without the libbmbot tag.
var ErrNotBuilt = errors.New("nickbot was built without libbmbot (use -tags libbmbot)")
