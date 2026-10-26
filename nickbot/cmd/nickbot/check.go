package main

import (
	"encoding/json"
	"fmt"
	"os"
	"time"
)

// statusFile is what nickbot writes about itself every ten seconds, and what --check reads.
type statusFile struct {
	Time        int64  `json:"time"`
	Uptime      int64  `json:"uptime"`
	Address     string `json:"address"`
	Anonymous   string `json:"anonymous"`
	Peers       int    `json:"peers"`
	NodeRunning bool   `json:"node_running"`
	Names       int    `json:"names"`
	Received    int64  `json:"received"`
	Chats       int64  `json:"chats"`
	Letters     int64  `json:"letters"`
	Answered    int64  `json:"answered"`
	Limited     int64  `json:"limited"`
	Notices     int64  `json:"notices"`
	Dropped     int64  `json:"dropped"`
	Errors      int64  `json:"errors"`
}

// checkStatus is the container's health check: the status file is fresh and names at least that
// many peers.
func checkStatus(path string, maxAge time.Duration, minPeers int, now time.Time) error {
	raw, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("cannot read the status file: %w", err)
	}
	var st statusFile
	if err := json.Unmarshal(raw, &st); err != nil {
		return fmt.Errorf("cannot read the status file: %w", err)
	}
	if age := now.Sub(time.Unix(st.Time, 0)); st.Time == 0 || age > maxAge {
		return fmt.Errorf("the status file is too old (%s)", age.Round(time.Second))
	}
	if st.Peers < minPeers {
		return fmt.Errorf("%d peers, want at least %d", st.Peers, minPeers)
	}
	return nil
}

// writeStatus replaces the status file in one step, so that a reader never sees half of it.
func writeStatus(path string, st statusFile) error {
	raw, err := json.Marshal(st)
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, append(raw, '\n'), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}
