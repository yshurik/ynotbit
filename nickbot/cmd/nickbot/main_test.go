package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/yshurik/ynotbit/nickbot/internal/nick"
	"github.com/yshurik/ynotbit/nickbot/internal/store"
)

func TestSnapshotMakesOneCopyADayAndKeepsTheNewest(t *testing.T) {
	db := filepath.Join(t.TempDir(), "nickbot.sqlite")
	st, err := store.Open(db)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	n, _ := nick.Parse("@ben")
	if _, err := st.Reg().Register(n, seedBot, store.RegisterOptions{}); err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(t.TempDir(), "backup")
	day := time.Date(2026, 10, 1, 12, 0, 0, 0, time.UTC)
	for i := 0; i < 10; i++ {
		now := day.AddDate(0, 0, i)
		if err := snapshot(st, dir, 7, now); err != nil {
			t.Fatal(err)
		}
		if err := snapshot(st, dir, 7, now.Add(time.Hour)); err != nil { // the same day again
			t.Fatalf("a second snapshot in a day: %v", err)
		}
	}
	entries, _ := os.ReadDir(dir)
	var names []string
	for _, e := range entries {
		names = append(names, e.Name())
	}
	if len(names) != 7 || names[0] != "nickbot-20261004.sqlite" || names[6] != "nickbot-20261010.sqlite" {
		t.Fatalf("snapshots: %v", names)
	}
	copyDB, err := store.Open(filepath.Join(dir, names[6]))
	if err != nil {
		t.Fatal(err)
	}
	defer copyDB.Close()
	if _, ok, _ := copyDB.Reg().ByNick(n); !ok {
		t.Error("the snapshot lacks the name")
	}
}

func TestSnapshotLeavesOtherFilesAlone(t *testing.T) {
	st, _ := store.Open(filepath.Join(t.TempDir(), "n.sqlite"))
	defer st.Close()
	dir := t.TempDir()
	other := filepath.Join(dir, "notes.txt")
	os.WriteFile(other, []byte("keep"), 0o600)
	for i := 0; i < 12; i++ {
		if err := snapshot(st, dir, 3, time.Date(2026, 10, 1+i, 0, 0, 0, 0, time.UTC)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := os.Stat(other); err != nil {
		t.Error("a file that is not a snapshot was removed")
	}
}

func TestTheStatusFileIsWrittenWholeAndChecksOut(t *testing.T) {
	path := filepath.Join(t.TempDir(), "status.json")
	if err := writeStatus(path, statusFile{Time: time.Now().Unix(), Peers: 4, Address: seedBot}); err != nil {
		t.Fatal(err)
	}
	if err := checkStatus(path, time.Minute, 1, time.Now()); err != nil {
		t.Error(err)
	}
	if _, err := os.Stat(path + ".tmp"); err == nil {
		t.Error("the temporary file was left behind")
	}
}

func TestFlagsDefaults(t *testing.T) {
	var stderr bytes.Buffer
	o, err := parseFlags([]string{"--data-dir", "/x"}, &stderr)
	if err != nil {
		t.Fatal(err)
	}
	if o.statusFile != "/x/status.json" || !o.public || o.anonPhrase != "anonymous" || o.changesPerDay != 5 || o.retentionMB != 512 {
		t.Errorf("options = %+v", o)
	}
	if _, err := parseFlags([]string{"--no-such-flag"}, &stderr); err == nil {
		t.Error("an unknown flag was accepted")
	}
}

func TestAKeysFileStandsInForThePhraseFile(t *testing.T) {
	var stderr bytes.Buffer
	o, err := parseFlags([]string{"--data-dir", "/x"}, &stderr)
	if err != nil || o.phraseFile != "/run/secrets/phrase" || o.keysFile != "" {
		t.Fatalf("defaults: %+v, %v", o, err)
	}
	// An address that no phrase made (a vanity search found it) comes from its two keys; the
	// phrase file's default then names nothing.
	o, err = parseFlags([]string{"--data-dir", "/x", "--keys-file", "/run/secrets/keys"}, &stderr)
	if err != nil || o.keysFile != "/run/secrets/keys" || o.phraseFile != "" {
		t.Errorf("with a keys file: %+v, %v", o, err)
	}
	// Asked for both, it says so rather than choosing.
	stderr.Reset()
	if _, err := parseFlags([]string{"--phrase-file", "/p", "--keys-file", "/k"}, &stderr); err == nil {
		t.Error("a phrase file and a keys file were both accepted")
	}
}

func TestRunCheckAndAdminExitCodes(t *testing.T) {
	var out, errOut bytes.Buffer
	good := filepath.Join(t.TempDir(), "s.json")
	writeStatus(good, statusFile{Time: time.Now().Unix(), Peers: 2})
	if code := run([]string{"--check", good}, &out, &errOut); code != 0 {
		t.Errorf("check of a healthy file = %d (%s)", code, errOut.String())
	}
	errOut.Reset()
	if code := run([]string{"--check", filepath.Join(t.TempDir(), "none.json")}, &out, &errOut); code != 1 || !strings.Contains(errOut.String(), "nickbot:") {
		t.Errorf("check of a missing file = %d (%s)", code, errOut.String())
	}
	if code := run([]string{"--check", good, "--peers", "99"}, &out, &errOut); code != 1 {
		t.Errorf("check with too few peers = %d", code)
	}
	if code := run([]string{"admin", "stats"}, &out, &errOut); code != 1 {
		t.Errorf("admin without a database = %d", code)
	}
	if code := run([]string{"--bogus"}, &out, &errOut); code != 2 {
		t.Errorf("a bad flag = %d", code)
	}
	db, st := adminRig(t)
	st.Close()
	out.Reset()
	if code := run([]string{"admin", "-db", db, "stats"}, &out, &errOut); code != 0 || !strings.Contains(out.String(), fmt.Sprintf("names: %d", 3)) {
		t.Errorf("admin stats = %d: %s", code, out.String())
	}
}
