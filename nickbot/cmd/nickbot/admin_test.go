package main

import (
	"bytes"
	"path/filepath"
	"strings"
	"testing"

	"github.com/yshurik/ynotbit/nickbot/internal/nick"
	"github.com/yshurik/ynotbit/nickbot/internal/store"
)

func adminRig(t *testing.T) (db string, st *store.Store) {
	t.Helper()
	db = filepath.Join(t.TempDir(), "nickbot.sqlite")
	st, err := store.Open(db)
	if err != nil {
		t.Fatal(err)
	}
	for _, r := range []struct{ nick, address string }{{"@ben", seedBot}, {"/benbot", seedBot}, {"@zed", "BM-other"}} {
		n, _ := nick.Parse(r.nick)
		if _, err := st.Reg().Register(n, r.address, store.RegisterOptions{}); err != nil {
			t.Fatal(err)
		}
	}
	return db, st
}

func runAdminOn(t *testing.T, db string, args ...string) (string, error) {
	t.Helper()
	var out bytes.Buffer
	err := runAdmin(append([]string{"-db", db}, args...), &out)
	return out.String(), err
}

func TestAdminStats(t *testing.T) {
	db, st := adminRig(t)
	st.Close()
	out, err := runAdminOn(t, db, "stats")
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"names: 3", "people: 2", "bots: 1", "chans: 0", "addresses: 2"} {
		if !strings.Contains(out, want) {
			t.Errorf("stats lack %q:\n%s", want, out)
		}
	}
}

func TestAdminExport(t *testing.T) {
	db, st := adminRig(t)
	st.Close()
	out, err := runAdminOn(t, db, "export")
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimSpace(out), "\n")
	if len(lines) != 3 || lines[0] != "/benbot\t"+seedBot+"\tno" || lines[1] != "@ben\t"+seedBot+"\tno" {
		t.Errorf("export:\n%s", out)
	}
}

func TestAdminRemoveReserveAndHistory(t *testing.T) {
	db, st := adminRig(t)
	st.Close()
	if _, err := runAdminOn(t, db, "remove", "@ben"); err != nil {
		t.Fatal(err)
	}
	if _, err := runAdminOn(t, db, "remove", "@ben"); err == nil {
		t.Error("removing a missing name had no error")
	}
	if _, err := runAdminOn(t, db, "reserve", "@zed"); err != nil {
		t.Fatal(err)
	}
	out, _ := runAdminOn(t, db, "export")
	if strings.Contains(out, "@zed") || strings.Contains(out, "@ben\t") {
		t.Errorf("the removed and the reserved names are still registered:\n%s", out)
	}
	out, err := runAdminOn(t, db, "history", seedBot)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"register", "remove", "@ben", "/benbot"} {
		if !strings.Contains(out, want) {
			t.Errorf("history lacks %q:\n%s", want, out)
		}
	}
}

func TestAdminRefusals(t *testing.T) {
	db, st := adminRig(t)
	st.Close()
	for name, args := range map[string][]string{
		"no command":      {},
		"unknown":         {"explode"},
		"remove no arg":   {"remove"},
		"remove bad nick": {"remove", "ben"},
		"reserve bad":     {"reserve", "@b"},
		"history no arg":  {"history"},
	} {
		if _, err := runAdminOn(t, db, args...); err == nil {
			t.Errorf("%s: no error", name)
		}
	}
	var out bytes.Buffer
	if err := runAdmin([]string{"stats"}, &out); err == nil {
		t.Error("no -db: no error")
	}
}
