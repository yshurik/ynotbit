package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func writeFile(t *testing.T, content string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "status.json")
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestCheckStatus(t *testing.T) {
	now := time.Unix(1_800_000_000, 0)
	fresh := writeFile(t, `{"time":1799999990,"peers":3}`)
	stale := writeFile(t, `{"time":1799999000,"peers":3}`)
	lonely := writeFile(t, `{"time":1799999990,"peers":0}`)
	for _, tc := range []struct {
		name, path string
		maxAge     time.Duration
		peers      int
		want       string // a part of the error, or "" for healthy
	}{
		{"healthy", fresh, 90 * time.Second, 1, ""},
		{"no peers needed", lonely, 90 * time.Second, 0, ""},
		{"stale", stale, 90 * time.Second, 1, "old"},
		{"lonely", lonely, 90 * time.Second, 1, "peers"},
		{"missing", filepath.Join(t.TempDir(), "none.json"), 90 * time.Second, 1, "read"},
		{"not json", writeFile(t, "nonsense"), 90 * time.Second, 1, "read"},
		{"no time", writeFile(t, `{"peers":5}`), 90 * time.Second, 1, "old"},
	} {
		err := checkStatus(tc.path, tc.maxAge, tc.peers, now)
		switch {
		case tc.want == "" && err != nil:
			t.Errorf("%s: %v", tc.name, err)
		case tc.want != "" && (err == nil || !strings.Contains(err.Error(), tc.want)):
			t.Errorf("%s: error %v, want one with %q", tc.name, err, tc.want)
		}
	}
}
