package store

import (
	"database/sql"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/yshurik/ynotbit/nickbot/internal/nick"
)

const (
	addrA = "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN"
	addrB = "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6"
	addrC = "BM-2cUqyPSNiBCvTg51hztE4uWrxET3386Ezo"
	addrD = "BM-87echoAkzstiHuPGSN8PH88m4WPm4attre8"
)

type clock struct{ t time.Time }

func (c *clock) now() time.Time          { return c.t }
func (c *clock) advance(d time.Duration) { c.t = c.t.Add(d) }

func open(t *testing.T) (*Store, *clock) {
	t.Helper()
	c := &clock{t: time.Unix(1_800_000_000, 0)}
	s, err := Open(filepath.Join(t.TempDir(), "nickbot.sqlite"), WithClock(c.now))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s, c
}

func must(t *testing.T, text string) nick.Nick {
	t.Helper()
	n, err := nick.Parse(text)
	if err != nil {
		t.Fatalf("Parse(%q): %v", text, err)
	}
	return n
}

func register(t *testing.T, s *Store, text, address string) RegisterResult {
	t.Helper()
	r, err := s.Reg().Register(must(t, text), address, RegisterOptions{})
	if err != nil {
		t.Fatalf("Register(%s, %s): %v", text, address, err)
	}
	return r
}

func lookup(t *testing.T, s *Store, text string) (Entry, bool) {
	t.Helper()
	e, ok, err := s.Reg().ByNick(must(t, text))
	if err != nil {
		t.Fatal(err)
	}
	return e, ok
}

func TestOpenMakesTheSchemaAndReopens(t *testing.T) {
	path := filepath.Join(t.TempDir(), "n.sqlite")
	s, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if v, err := s.userVersion(); err != nil || v != len(migrations) {
		t.Fatalf("user_version = %d, %v; want %d", v, err, len(migrations))
	}
	if _, err := s.Reg().Register(must(t, "@ben"), addrA, RegisterOptions{}); err != nil {
		t.Fatal(err)
	}
	s.Close()
	s, err = Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if _, ok := lookup(t, s, "@ben"); !ok {
		t.Error("the name did not survive a reopen")
	}
}

func TestOpenRefusesADatabaseFromANewerVersion(t *testing.T) {
	path := filepath.Join(t.TempDir(), "n.sqlite")
	s, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.db.Exec(fmt.Sprintf("PRAGMA user_version = %d", len(migrations)+1)); err != nil {
		t.Fatal(err)
	}
	s.Close()
	if _, err := Open(path); err == nil {
		t.Fatal("a database from a newer nickbot was opened")
	}
}

// databaseOfTheFirstSchema makes a file the way the first version of the schema did, and runs the
// statements on it: what the service from before the change left behind.
func databaseOfTheFirstSchema(t *testing.T, statements ...string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "n.sqlite")
	db, err := sql.Open("sqlite", "file:"+path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	for _, statement := range append([]string{migrations[0], "PRAGMA user_version = 1"}, statements...) {
		if _, err := db.Exec(statement); err != nil {
			t.Fatalf("%s: %v", statement, err)
		}
	}
	return path
}

func firstSchemaName(nickname, sigil, address string) string {
	return fmt.Sprintf("INSERT INTO nicks(nick_key, nick, sigil, name_lc, address, static, created, updated) "+
		"VALUES('%[1]s', '%[1]s', '%[2]s', '%[3]s', '%[4]s', 0, 1, 1)", nickname, sigil, nickname[1:], address)
}

func TestADatabaseOfTheFirstSchemaKeepsItsNamesAndLosesItsHolds(t *testing.T) {
	path := databaseOfTheFirstSchema(t,
		firstSchemaName("@ben", "@", addrA),
		fmt.Sprintf("INSERT INTO released(nick_key, address, until) VALUES('@old', '%s', 4000000000)", addrB))
	s, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if e, ok := lookup(t, s, "@ben"); !ok || e.Address != addrA {
		t.Errorf("the name did not come through: %+v, %v", e, ok)
	}
	// B gave up @old, and the old rules kept it free for B for a month: now it is free for anyone.
	register(t, s, "@old", addrC)
	var tables int
	s.db.QueryRow("SELECT COUNT(*) FROM sqlite_master WHERE name='released'").Scan(&tables)
	if tables != 0 {
		t.Error("the table of holds is still there")
	}
}

func TestADatabaseWithTwoNamesForOneAddressIsRefusedAndLeftAlone(t *testing.T) {
	path := databaseOfTheFirstSchema(t, firstSchemaName("@ben", "@", addrA), firstSchemaName("/benbot", "/", addrA))
	_, err := Open(path)
	if err == nil {
		t.Fatal("a database with two names for an address was opened")
	}
	if !strings.Contains(err.Error(), "migration 2") {
		t.Errorf("the error does not say which step failed: %v", err)
	}
	// Nothing was changed, so the service from before can still read it.
	db, err := sql.Open("sqlite", "file:"+path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	var version, names, tables int
	db.QueryRow("PRAGMA user_version").Scan(&version)
	db.QueryRow("SELECT COUNT(*) FROM nicks").Scan(&names)
	db.QueryRow("SELECT COUNT(*) FROM sqlite_master WHERE name='released'").Scan(&tables)
	if version != 1 || names != 2 || tables != 1 {
		t.Errorf("version %d, %d names, %d hold tables; want 1, 2, 1", version, names, tables)
	}
}

func TestRegisterGivesTheNameToTheAddress(t *testing.T) {
	s, c := open(t)
	r := register(t, s, "@Ben", addrA)
	if r.Already || r.Replaced != "" || r.Entry.Nick != "@Ben" || r.Entry.Address != addrA || r.Entry.Static {
		t.Errorf("result = %+v", r)
	}
	if !r.Entry.Created.Equal(c.now()) {
		t.Errorf("created %v, now %v", r.Entry.Created, c.now())
	}
	e, ok := lookup(t, s, "@ben")
	if !ok || e.Address != addrA || e.Nick != "@Ben" {
		t.Errorf("ByNick(@ben) = %+v, %v; the spelling registered is kept", e, ok)
	}
	list, err := s.Reg().ByAddress(addrA)
	if err != nil || len(list) != 1 || list[0].Nick != "@Ben" {
		t.Errorf("ByAddress = %+v, %v", list, err)
	}
	if _, ok := lookup(t, s, "@nobody"); ok {
		t.Error("found a name nobody has")
	}
	if list, _ := s.Reg().ByAddress(addrB); len(list) != 0 {
		t.Errorf("another address has names: %+v", list)
	}
}

func TestRegisteringYourOwnNameAgainChangesNothing(t *testing.T) {
	s, c := open(t)
	first := register(t, s, "@ben", addrA)
	c.advance(time.Hour)
	again := register(t, s, "@BEN", addrA)
	if !again.Already {
		t.Error("the second registration says it is new")
	}
	if !again.Entry.Created.Equal(first.Entry.Created) || again.Entry.Nick != "@ben" {
		t.Errorf("the entry changed: %+v -> %+v", first.Entry, again.Entry)
	}
	n, err := s.Reg().CountChanges(addrA, c.now().Add(-24*time.Hour))
	if err != nil || n != 1 {
		t.Errorf("changes = %d, %v; only the first counts", n, err)
	}
}

func TestANameOfAnotherAddressIsTaken(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@ben", addrA)
	for _, text := range []string{"@ben", "@BEN", "@Ben"} {
		if _, err := s.Reg().Register(must(t, text), addrB, RegisterOptions{}); !errors.Is(err, ErrTaken) {
			t.Errorf("Register(%s) by another address = %v, want ErrTaken", text, err)
		}
	}
	if e, _ := lookup(t, s, "@ben"); e.Address != addrA {
		t.Errorf("the owner changed to %s", e.Address)
	}
}

func TestAnAddressHasOneName(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@ben", addrA)
	r := register(t, s, "@benny", addrA)
	if r.Replaced != "@ben" {
		t.Errorf("Replaced = %q, want @ben", r.Replaced)
	}
	if _, ok := lookup(t, s, "@ben"); ok {
		t.Error("the old name is still there")
	}
	// A name of another sigil takes the place too: the address has one name, not one of each.
	r = register(t, s, "/benbot", addrA)
	if r.Replaced != "@benny" {
		t.Errorf("Replaced = %q, want @benny", r.Replaced)
	}
	if _, ok := lookup(t, s, "@benny"); ok {
		t.Error("the person's name stayed beside the bot's")
	}
	list, _ := s.Reg().ByAddress(addrA)
	if len(list) != 1 || list[0].Nick != "/benbot" {
		t.Errorf("names = %+v", list)
	}
}

func TestAPersonAndABotOfOneNameAreTwoNames(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@ben", addrA)
	register(t, s, "/ben", addrB)
	if e, _ := lookup(t, s, "@ben"); e.Address != addrA {
		t.Errorf("@ben moved to %s", e.Address)
	}
	if e, _ := lookup(t, s, "/ben"); e.Address != addrB {
		t.Errorf("/ben = %s", e.Address)
	}
}

func TestTheDatabaseItselfRefusesASecondNameForAnAddress(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@ben", addrA)
	_, err := s.db.Exec("INSERT INTO nicks(nick_key, nick, sigil, name_lc, address, static, created, updated) "+
		"VALUES('/benbot', '/benbot', '/', 'benbot', ?, 0, 1, 1)", addrA)
	if err == nil {
		t.Error("a second name went in for one address")
	}
}

func TestChanNamesKeepTheirCase(t *testing.T) {
	s, _ := open(t)
	if _, err := s.Reg().Register(must(t, "#Room"), addrA, RegisterOptions{Static: true}); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Reg().Register(must(t, "#room"), addrB, RegisterOptions{Static: true}); err != nil {
		t.Errorf("#room is another chan than #Room: %v", err)
	}
	if e, _ := lookup(t, s, "#Room"); e.Address != addrA {
		t.Errorf("#Room = %s", e.Address)
	}
	if e, _ := lookup(t, s, "#room"); e.Address != addrB {
		t.Errorf("#room = %s", e.Address)
	}
}

func TestAChangedNameIsFreeAtOnce(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@ben", addrA)
	register(t, s, "@benny", addrA)
	// Nobody keeps @ben for A: another address takes it now ...
	register(t, s, "@ben", addrB)
	// ... and A has no claim on it any more.
	if _, err := s.Reg().Register(must(t, "@ben"), addrA, RegisterOptions{}); !errors.Is(err, ErrTaken) {
		t.Errorf("the old owner took back a name another address has: %v", err)
	}
	if e, _ := lookup(t, s, "@ben"); e.Address != addrB {
		t.Errorf("@ben = %s", e.Address)
	}
	if e, _ := lookup(t, s, "@benny"); e.Address != addrA {
		t.Errorf("@benny = %s", e.Address)
	}
}

func TestUnregister(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@ben", addrA)
	if err := s.Reg().Unregister(must(t, "@ben"), addrB); !errors.Is(err, ErrNotYours) {
		t.Errorf("Unregister by another address = %v, want ErrNotYours", err)
	}
	if _, ok := lookup(t, s, "@ben"); !ok {
		t.Error("another address removed the name")
	}
	if err := s.Reg().Unregister(must(t, "@nobody"), addrA); !errors.Is(err, ErrNotFound) {
		t.Errorf("Unregister of a name nobody has = %v, want ErrNotFound", err)
	}
	if err := s.Reg().Unregister(must(t, "@BEN"), addrA); err != nil {
		t.Fatal(err)
	}
	if _, ok := lookup(t, s, "@ben"); ok {
		t.Error("the name is still there")
	}
	// Free at once, like the name of a change.
	register(t, s, "@ben", addrB)
}

func TestStaticNamesNeverChange(t *testing.T) {
	s, _ := open(t)
	r := s.Reg()
	if _, err := r.Register(must(t, "#room"), addrA, RegisterOptions{Static: true}); err != nil {
		t.Fatal(err)
	}
	if e, _ := lookup(t, s, "#room"); !e.Static {
		t.Error("not marked static")
	}
	if err := r.Unregister(must(t, "#room"), addrA); !errors.Is(err, ErrStatic) {
		t.Errorf("Unregister of a static name = %v, want ErrStatic", err)
	}
	if _, err := r.Register(must(t, "#other"), addrA, RegisterOptions{Static: true}); !errors.Is(err, ErrStatic) {
		t.Errorf("a second chan name for one address = %v, want ErrStatic", err)
	}
	if _, err := r.Register(must(t, "@person"), addrA, RegisterOptions{}); !errors.Is(err, ErrStatic) {
		t.Errorf("a person's name for the address of a chan = %v, want ErrStatic", err)
	}
	if _, err := r.Register(must(t, "#room"), addrB, RegisterOptions{Static: true}); !errors.Is(err, ErrTaken) {
		t.Errorf("another address took a chan name: %v", err)
	}
	again, err := r.Register(must(t, "#room"), addrA, RegisterOptions{Static: true})
	if err != nil || !again.Already {
		t.Errorf("the chan registering its own name again = %+v, %v", again, err)
	}
}

func TestSeed(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@taken", addrB)
	err := s.Seed([]SeedEntry{
		{Nick: must(t, "/echobot"), Address: addrD},
		{Nick: must(t, "@admin")}, // reserved, no address
		{Nick: must(t, "@taken"), Address: addrA},
	})
	if err != nil {
		t.Fatal(err)
	}
	if e, ok := lookup(t, s, "/echobot"); !ok || e.Address != addrD || !e.Static {
		t.Errorf("/echobot = %+v, %v", e, ok)
	}
	if _, ok := lookup(t, s, "@admin"); ok {
		t.Error("a reserved name was registered")
	}
	if _, err := s.Reg().Register(must(t, "@Admin"), addrC, RegisterOptions{}); !errors.Is(err, ErrReserved) {
		t.Errorf("Register of a reserved name = %v, want ErrReserved", err)
	}
	if e, _ := lookup(t, s, "@taken"); e.Address != addrA || !e.Static {
		t.Errorf("the operator's seed did not win: %+v", e)
	}
	if _, err := s.Reg().Register(must(t, "@taken"), addrC, RegisterOptions{}); !errors.Is(err, ErrTaken) {
		t.Errorf("Register of a seeded name = %v", err)
	}
	if list, _ := s.Reg().ByAddress(addrB); len(list) != 0 {
		t.Errorf("the address that lost its name still has %+v", list)
	}
	// Seeding again changes nothing and does not fail.
	if err := s.Seed([]SeedEntry{{Nick: must(t, "/echobot"), Address: addrD}, {Nick: must(t, "@admin")}}); err != nil {
		t.Errorf("a second Seed: %v", err)
	}
	if err := s.Reg().Unregister(must(t, "/echobot"), addrD); !errors.Is(err, ErrStatic) {
		t.Errorf("a seeded name was unregistered: %v", err)
	}
}

func TestSeedGivesAnAddressOneName(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@ben", addrA)
	if err := s.Seed([]SeedEntry{{Nick: must(t, "/echobot"), Address: addrA}}); err != nil {
		t.Fatal(err)
	}
	if _, ok := lookup(t, s, "@ben"); ok {
		t.Error("the address kept its old name beside the seeded one")
	}
	if list, _ := s.Reg().ByAddress(addrA); len(list) != 1 || list[0].Nick != "/echobot" {
		t.Errorf("names = %+v", list)
	}
}

func searchNames(t *testing.T, s *Store, term string, limit int) ([]string, bool) {
	t.Helper()
	parsed, err := nick.ParseTerm(term)
	if err != nil {
		t.Fatalf("ParseTerm(%q): %v", term, err)
	}
	list, more, err := s.Reg().Search(parsed, limit)
	if err != nil {
		t.Fatal(err)
	}
	var names []string
	for _, e := range list {
		names = append(names, e.Nick)
	}
	return names, more
}

func TestSearchFindsPartsOfNamesWhateverTheCase(t *testing.T) {
	s, _ := open(t)
	for i, text := range []string{"@benjamin", "@ben_x", "/benbot", "@abenaki", "@xyzzy", "#Benches"} {
		address := fmt.Sprintf("BM-test%02d", i)
		if _, err := s.Reg().Register(must(t, text), address, RegisterOptions{Static: text[0] == '#'}); err != nil {
			t.Fatal(err)
		}
	}
	for _, tc := range []struct {
		term string
		want []string
	}{
		{"benj", []string{"@benjamin"}},
		{"ben_", []string{"@ben_x"}}, // _ is a letter of a name, not a wildcard
		{"enak", []string{"@abenaki"}},
		{"BENC", []string{"#Benches"}}, // the case of the term does not matter, nor the case of a chan
		{"/benb", []string{"/benbot"}},
		{"@benb", nil}, // the sigil narrows the search
		{"#benc", []string{"#Benches"}},
		{"nomatch", nil},
	} {
		got, more := searchNames(t, s, tc.term, 10)
		if more || fmt.Sprint(got) != fmt.Sprint(tc.want) {
			t.Errorf("%s = %v (more %v), want %v", tc.term, got, more, tc.want)
		}
	}
}

func TestSearchPutsNamesThatBeginWithTheTermFirst(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@xbenx", "BM-1")
	register(t, s, "@benzz", "BM-2")
	register(t, s, "@abenz", "BM-3")
	register(t, s, "@bennn", "BM-4")
	list, _, err := s.Reg().Search(nick.Term{Text: "ben"}, 10)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, e := range list {
		got = append(got, e.Nick)
	}
	want := []string{"@bennn", "@benzz", "@abenz", "@xbenx"}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Errorf("ben = %v, want %v: names that begin with the term first, then by name", got, want)
	}
}

func TestSearchCutsAtTheLimitAndSaysThereIsMore(t *testing.T) {
	s, _ := open(t)
	for i := 0; i < 15; i++ {
		register(t, s, fmt.Sprintf("@zzcat%02d", i), fmt.Sprintf("BM-cat%02d", i))
	}
	names, more := searchNames(t, s, "zzca", 10)
	if !more || len(names) != 10 || names[0] != "@zzcat00" || names[9] != "@zzcat09" {
		t.Errorf("zzca = %v (more %v)", names, more)
	}
	names, more = searchNames(t, s, "zzca", 15)
	if more || len(names) != 15 {
		t.Errorf("with room for all: %d names, more %v", len(names), more)
	}
	names, more = searchNames(t, s, "zzcat14", 1)
	if more || len(names) != 1 {
		t.Errorf("zzcat14 = %v (more %v)", names, more)
	}
}

// Whatever reaches Search is data. ParseTerm keeps these out earlier, and this is the second line of
// defence: the driver is given them as parameters.
func TestSearchTreatsHostileTermsAsData(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@victim", addrA)
	register(t, s, "@a_b", addrB)
	register(t, s, "@axb", addrC)
	hostile := []string{
		"'", "\"", "''", "' OR '1'='1", "'; DROP TABLE nicks; --", "%", "%%%%", "____", "a%", "%a", "\\",
		"\\%", "\x00", "a\x00b", "\n", "/*", "*/", "--", "victim' --", "UNION SELECT address FROM nicks",
		"é", "ｖｉｃｔｉｍ", "\u202e", "[a-z]", "(", ")", ";", "?", "$1", "@1", ":x", "x'||'y",
	}
	for _, h := range hostile {
		list, _, err := s.Reg().Search(nick.Term{Text: h}, 10)
		if err != nil {
			t.Errorf("Search(%q): %v", h, err)
			continue
		}
		if len(list) != 0 {
			t.Errorf("Search(%q) found %d names", h, len(list))
		}
	}
	// _ is not a wildcard: it is found where it is, and "a_b" does not match "axb".
	for _, text := range []string{"_", "a_b"} {
		list, _, _ := s.Reg().Search(nick.Term{Text: text}, 10)
		if len(list) != 1 || list[0].Nick != "@a_b" {
			t.Errorf("%s = %+v, want only @a_b", text, list)
		}
	}
	var count int
	if err := s.db.QueryRow("SELECT COUNT(*) FROM nicks").Scan(&count); err != nil || count != 3 {
		t.Errorf("the table has %d rows, %v", count, err)
	}
}

func TestDoRunsOnceAndReplaysTheReply(t *testing.T) {
	s, _ := open(t)
	runs := 0
	fn := func(r *Reg) (string, error) {
		runs++
		if _, err := r.Register(must(t, "@ben"), addrA, RegisterOptions{}); err != nil {
			return "", err
		}
		return "registered @ben", nil
	}
	reply, replayed, err := s.Do("event-1", fn)
	if err != nil || replayed || reply != "registered @ben" {
		t.Fatalf("first Do = %q, %v, %v", reply, replayed, err)
	}
	reply, replayed, err = s.Do("event-1", fn)
	if err != nil || !replayed || reply != "registered @ben" || runs != 1 {
		t.Fatalf("second Do = %q, replayed %v, %v, runs %d; the reply comes back and nothing runs", reply, replayed, err, runs)
	}
	// Another event is a new action: and the name is already the sender's.
	r, _, err := s.Do("event-2", func(r *Reg) (string, error) {
		res, err := r.Register(must(t, "@ben"), addrA, RegisterOptions{})
		if err != nil {
			return "", err
		}
		return fmt.Sprint(res.Already), nil
	})
	if err != nil || r != "true" {
		t.Errorf("event-2 = %q, %v", r, err)
	}
}

func TestDoRollsBackWhatFailed(t *testing.T) {
	s, _ := open(t)
	boom := errors.New("boom")
	_, _, err := s.Do("event-1", func(r *Reg) (string, error) {
		if _, err := r.Register(must(t, "@ben"), addrA, RegisterOptions{}); err != nil {
			return "", err
		}
		return "", boom
	})
	if !errors.Is(err, boom) {
		t.Fatalf("Do = %v", err)
	}
	if _, ok := lookup(t, s, "@ben"); ok {
		t.Error("a failed action left its name behind")
	}
	// The failed event is not recorded as handled: it may run again.
	runs := 0
	_, replayed, err := s.Do("event-1", func(r *Reg) (string, error) { runs++; return "ok", nil })
	if err != nil || replayed || runs != 1 {
		t.Errorf("second Do: replayed %v, %v, runs %d", replayed, err, runs)
	}
}

func TestHistoryAndCounts(t *testing.T) {
	s, c := open(t)
	register(t, s, "@ben", addrA)
	c.advance(time.Hour)
	register(t, s, "@benny", addrA)
	c.advance(time.Hour)
	if err := s.Reg().Unregister(must(t, "@benny"), addrA); err != nil {
		t.Fatal(err)
	}
	register(t, s, "@zed", addrB)
	all, err := s.Reg().CountAllChanges(c.now().Add(-24 * time.Hour))
	if err != nil || all != 3 {
		t.Errorf("all changes = %d, %v; two registrations and a change count, an unregister does not", all, err)
	}
	mine, _ := s.Reg().CountChanges(addrA, c.now().Add(-24*time.Hour))
	if mine != 2 {
		t.Errorf("A's changes = %d, want 2", mine)
	}
	recent, _ := s.Reg().CountChanges(addrA, c.now().Add(-90*time.Minute))
	if recent != 1 {
		t.Errorf("A's changes in the last 90 minutes = %d, want 1", recent)
	}
	rows, err := s.History(addrA, 10)
	if err != nil || len(rows) != 3 {
		t.Fatalf("history = %+v, %v", rows, err)
	}
	if rows[0].Action != "unregister" || rows[1].Action != "change" || rows[2].Action != "register" {
		t.Errorf("history order/actions: %+v", rows)
	}
}

func TestPrune(t *testing.T) {
	s, c := open(t)
	if _, _, err := s.Do("old", func(r *Reg) (string, error) { return "x", nil }); err != nil {
		t.Fatal(err)
	}
	c.advance(HandledKeep + time.Hour)
	if _, _, err := s.Do("new", func(r *Reg) (string, error) { return "y", nil }); err != nil {
		t.Fatal(err)
	}
	if err := s.Prune(); err != nil {
		t.Fatal(err)
	}
	var n int
	s.db.QueryRow("SELECT COUNT(*) FROM handled").Scan(&n)
	if n != 1 {
		t.Errorf("%d handled events left, want only the new one", n)
	}
}

func TestBackupIsASecondDatabase(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@ben", addrA)
	dir := t.TempDir()
	target := filepath.Join(dir, "it's a \"backup\".sqlite") // quoting must not matter
	if err := s.Backup(target); err != nil {
		t.Fatal(err)
	}
	copyDB, err := Open(target)
	if err != nil {
		t.Fatal(err)
	}
	defer copyDB.Close()
	if e, ok := lookup(t, copyDB, "@ben"); !ok || e.Address != addrA {
		t.Errorf("the backup has %+v, %v", e, ok)
	}
	if err := s.Backup(target); err == nil {
		t.Error("a backup overwrote an earlier file")
	}
	if _, err := os.Stat(target); err != nil {
		t.Error(err)
	}
}

func TestStatsExportAndAdminRemoval(t *testing.T) {
	s, _ := open(t)
	register(t, s, "@ben", addrA)
	register(t, s, "/benbot", addrB)
	if _, err := s.Reg().Register(must(t, "#room"), addrD, RegisterOptions{Static: true}); err != nil {
		t.Fatal(err)
	}
	st, err := s.Stats()
	if err != nil || st.Total != 3 || st.People != 1 || st.Bots != 1 || st.Chans != 1 || st.Addresses != 3 {
		t.Errorf("stats = %+v, %v", st, err)
	}
	all, err := s.Export()
	if err != nil || len(all) != 3 || all[0].Nick != "#room" {
		t.Errorf("export = %+v, %v; sorted by key", all, err)
	}
	// An operator removes any name, static ones too, and nobody is held to it.
	if err := s.Remove(must(t, "#room")); err != nil {
		t.Fatal(err)
	}
	if err := s.Remove(must(t, "#room")); !errors.Is(err, ErrNotFound) {
		t.Errorf("Remove of a missing name = %v", err)
	}
	if _, err := s.Reg().Register(must(t, "#room"), addrC, RegisterOptions{Static: true}); err != nil {
		t.Errorf("a removed name is free at once: %v", err)
	}
	if err := s.Reserve(must(t, "@ben")); err != nil {
		t.Fatal(err)
	}
	if e, ok := lookup(t, s, "@ben"); ok {
		t.Errorf("reserving a registered name should remove it first, still %+v", e)
	}
}

func TestOneWinnerWhenManyAskAtOnce(t *testing.T) {
	s, _ := open(t)
	var wins, taken int32
	var wg sync.WaitGroup
	for i := 0; i < 24; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, _, err := s.Do(fmt.Sprintf("e%d", i), func(r *Reg) (string, error) {
				_, err := r.Register(must(t, "@race"), fmt.Sprintf("BM-racer%02d", i), RegisterOptions{})
				return "", err
			})
			switch {
			case err == nil:
				atomic.AddInt32(&wins, 1)
			case errors.Is(err, ErrTaken):
				atomic.AddInt32(&taken, 1)
			default:
				t.Errorf("racer %d: %v", i, err)
			}
		}(i)
	}
	wg.Wait()
	if wins != 1 || taken != 23 {
		t.Errorf("wins %d, taken %d; exactly one racer owns the name", wins, taken)
	}
}

func TestTotal(t *testing.T) {
	s, _ := open(t)
	if n, err := s.Reg().Total(); err != nil || n != 0 {
		t.Fatalf("empty: %d, %v", n, err)
	}
	register(t, s, "@ben", addrA)
	register(t, s, "/benbot", addrC)
	register(t, s, "@zed", addrB)
	if n, err := s.Reg().Total(); err != nil || n != 3 {
		t.Errorf("Total = %d, %v; want 3", n, err)
	}
}
