package service

import (
	"encoding/json"
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/yshurik/ynotbit/nickbot/internal/bmaddr"
	"github.com/yshurik/ynotbit/nickbot/internal/store"
)

const (
	addrA    = "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN"
	addrB    = "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6"
	addrC    = "BM-2cUqyPSNiBCvTg51hztE4uWrxET3386Ezo"
	addrBot  = "BM-87echoAkzstiHuPGSN8PH88m4WPm4attre8"
	addrAnon = "BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r" // stands in for the shared lookup address
	addrRoom = "BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr" // the address the fake chan "room" makes
)

type clock struct{ t time.Time }

func (c *clock) now() time.Time          { return c.t }
func (c *clock) advance(d time.Duration) { c.t = c.t.Add(d) }

type fixture struct {
	svc   *Service
	store *store.Store
	clock *clock
	n     int
}

func newFixture(t *testing.T, mutate ...func(*Options)) *fixture {
	t.Helper()
	c := &clock{t: time.Unix(1_800_000_000, 0)}
	st, err := store.Open(filepath.Join(t.TempDir(), "n.sqlite"), store.WithClock(c.now))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	o := Options{
		Anonymous: addrAnon,
		Now:       c.now,
		ChanAddress: func(phrase string) (string, error) {
			if phrase == "room" {
				return addrRoom, nil
			}
			return "BM-chan-of-" + phrase, nil // an address nobody sends from
		},
	}
	for _, m := range mutate {
		m(&o)
	}
	return &fixture{svc: New(st, o), store: st, clock: c}
}

// chat sends a chat from the address and returns the reply.
func (f *fixture) chat(from, text string) Reply {
	f.n++
	return f.svc.Handle(Request{EventID: fmt.Sprintf("e%d", f.n), Transport: Chat, From: from, Text: text})
}

func (f *fixture) letter(from, subject, body string) Reply {
	f.n++
	return f.svc.Handle(Request{EventID: fmt.Sprintf("e%d", f.n), Transport: Letter, From: from, Subject: subject, Text: body})
}

// trailer returns the fields of the line a chat reply ends with.
func trailer(t *testing.T, r Reply) map[string]any {
	t.Helper()
	lines := strings.Split(r.Text, "\n")
	last := lines[len(lines)-1]
	if !strings.HasPrefix(last, "nickbot: ") {
		t.Fatalf("the last line is not a trailer: %q", last)
	}
	var m map[string]any
	if err := json.Unmarshal([]byte(strings.TrimPrefix(last, "nickbot: ")), &m); err != nil {
		t.Fatalf("the trailer is not JSON: %v: %q", err, last)
	}
	if m["v"] != float64(1) {
		t.Errorf("v = %v, want 1", m["v"])
	}
	return m
}

func firstLine(r Reply) string { return strings.SplitN(r.Text, "\n", 2)[0] }

func TestHelpListsTheCommands(t *testing.T) {
	f := newFixture(t)
	for _, text := range []string{"/help", "/HELP", "  /Help  "} {
		r := f.chat(addrA, text)
		for _, want := range []string{"/register", "/unregister", "/address", "/nickname", "/search", "/whoami"} {
			if !strings.Contains(r.Text, want) {
				t.Errorf("%q: the help does not mention %s:\n%s", text, want, r.Text)
			}
		}
		m := trailer(t, r)
		if m["cmd"] != "help" || m["ok"] != true {
			t.Errorf("trailer = %v", m)
		}
	}
}

func TestRegister(t *testing.T) {
	f := newFixture(t)
	r := f.chat(addrA, "/register @Ben")
	if r.Code != "" || r.Cmd != "register" {
		t.Errorf("reply = %+v", r)
	}
	if got := firstLine(r); got != "Registered @Ben for "+addrA+"." {
		t.Errorf("first line = %q", got)
	}
	m := trailer(t, r)
	if m["cmd"] != "register" || m["ok"] != true || m["nick"] != "@Ben" || m["address"] != addrA || m["already"] != nil {
		t.Errorf("trailer = %v", m)
	}
	e, ok, _ := f.store.Reg().ByNick(mustNick(t, "@ben"))
	if !ok || e.Address != addrA {
		t.Errorf("the registry has %+v, %v", e, ok)
	}
}

func TestRegisterAgainIsAlreadyYours(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @ben")
	r := f.chat(addrA, "/register @ben")
	if r.Code != "" || firstLine(r) != "@ben is already yours." {
		t.Errorf("reply = %q (code %q)", r.Text, r.Code)
	}
	if m := trailer(t, r); m["ok"] != true || m["already"] != true {
		t.Errorf("trailer = %v", m)
	}
}

func TestRegisterChangesTheName(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @ben")
	r := f.chat(addrA, "/register @benny")
	if got := firstLine(r); got != "Registered @benny for "+addrA+" (it replaces @ben)." {
		t.Errorf("first line = %q", got)
	}
	if m := trailer(t, r); m["replaced"] != "@ben" || m["nick"] != "@benny" {
		t.Errorf("trailer = %v", m)
	}
}

func TestTheSameEventIsAnsweredTheSameAndDoneOnce(t *testing.T) {
	f := newFixture(t)
	req := Request{EventID: "same", Transport: Chat, From: addrA, Text: "/register @ben"}
	first := f.svc.Handle(req)
	second := f.svc.Handle(req)
	if first.Text != second.Text || first.Code != "" || second.Code != "" {
		t.Errorf("replies differ:\n%q\n%q", first.Text, second.Text)
	}
	if !strings.HasPrefix(second.Text, "Registered @ben") {
		t.Errorf("the replay says %q: the user would be told the first answer again, not 'already yours'", firstLine(second))
	}
	rows, _ := f.store.History(addrA, 10)
	if len(rows) != 1 {
		t.Errorf("%d history rows for one event", len(rows))
	}
}

func TestRegisterRefusals(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @ben")
	for _, tc := range []struct {
		from, text, code, first string
	}{
		{addrB, "/register @ben", "taken", "@ben is taken."},
		{addrB, "/register @BEN", "taken", "@BEN is taken."},
		{addrB, "/register ben", "syntax", ""},
		{addrB, "/register @b", "syntax", ""},
		{addrB, "/register @1ben", "syntax", ""},
		{addrB, "/register", "usage", "Usage: /register @nick"},
		{addrB, "/register @ben @bob", "usage", "Usage: /register @nick"},
		{addrAnon, "/register @fresh", "anonymous", ""},
	} {
		r := f.chat(tc.from, tc.text)
		if r.Code != tc.code {
			t.Errorf("%q: code %q, want %q (%q)", tc.text, r.Code, tc.code, r.Text)
			continue
		}
		if tc.first != "" && firstLine(r) != tc.first {
			t.Errorf("%q: first line %q, want %q", tc.text, firstLine(r), tc.first)
		}
		m := trailer(t, r)
		if m["ok"] != false || m["error"] != tc.code {
			t.Errorf("%q: trailer = %v", tc.text, m)
		}
	}
	if list, _ := f.store.Reg().ByAddress(addrB); len(list) != 0 {
		t.Errorf("a refused address got names: %+v", list)
	}
}

func TestReservedNames(t *testing.T) {
	f := newFixture(t)
	if err := f.store.Seed([]store.SeedEntry{{Nick: mustNick(t, "@admin")}, {Nick: mustNick(t, "/echobot"), Address: addrBot}}); err != nil {
		t.Fatal(err)
	}
	if r := f.chat(addrA, "/register @admin"); r.Code != "reserved" {
		t.Errorf("code %q", r.Code)
	}
	if r := f.chat(addrA, "/register /echobot"); r.Code != "taken" {
		t.Errorf("code %q", r.Code)
	}
	if r := f.chat(addrBot, "/unregister /echobot"); r.Code != "static" {
		t.Errorf("code %q", r.Code)
	}
}

func TestAChanNameNeedsTheChansOwnAddress(t *testing.T) {
	f := newFixture(t)
	r := f.chat(addrRoom, "/register #room")
	if r.Code != "" || trailer(t, r)["nick"] != "#room" {
		t.Fatalf("the chan registering its own name: %+v", r)
	}
	if e, _, _ := f.store.Reg().ByNick(mustNick(t, "#room")); !e.Static {
		t.Error("a chan name is not permanent")
	}
	if r := f.chat(addrB, "/register #room"); r.Code != "not_chan" {
		t.Errorf("another address registering #room: code %q", r.Code)
	}
	if r := f.chat(addrRoom, "/register #other"); r.Code != "not_chan" {
		t.Errorf("the chan registering a name that is not its passphrase: code %q", r.Code)
	}
	if r := f.chat(addrRoom, "/register #room"); r.Code != "" || trailer(t, r)["already"] != true {
		t.Errorf("the chan again: %+v", r)
	}
	if r := f.chat(addrRoom, "/unregister #room"); r.Code != "static" {
		t.Errorf("unregistering a chan name: code %q", r.Code)
	}
	// The passphrase is the name as written: another case is another chan.
	if r := f.chat(addrRoom, "/register #Room"); r.Code != "not_chan" {
		t.Errorf("#Room from the address of #room: code %q", r.Code)
	}
}

func TestAChanDerivationFailureIsInternal(t *testing.T) {
	f := newFixture(t, func(o *Options) {
		o.ChanAddress = func(string) (string, error) { return "", errors.New("library gone") }
	})
	if r := f.chat(addrA, "/register #room"); r.Code != "internal" {
		t.Errorf("code %q", r.Code)
	}
}

func TestTheAnonymousAddressOnlyLooksThingsUp(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @ben")
	for _, text := range []string{"/register @fresh", "/unregister @ben", "/whoami"} {
		if r := f.chat(addrAnon, text); r.Code != "anonymous" {
			t.Errorf("%q from the shared address: code %q", text, r.Code)
		}
	}
	for _, text := range []string{"/address @ben", "/nickname " + addrA, "/search ben1", "/search benn", "/help"} {
		if r := f.chat(addrAnon, text); r.Code == "anonymous" || r.Code == "internal" {
			t.Errorf("%q from the shared address was refused: %q", text, r.Code)
		}
	}
	if e, _, _ := f.store.Reg().ByNick(mustNick(t, "@ben")); e.Address != addrA {
		t.Error("the shared address changed a name")
	}
}

func TestUnregister(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @ben")
	if r := f.chat(addrB, "/unregister @ben"); r.Code != "not_yours" {
		t.Errorf("by another address: code %q", r.Code)
	}
	if r := f.chat(addrA, "/unregister @nobody"); r.Code != "not_found" {
		t.Errorf("a name nobody has: code %q", r.Code)
	}
	r := f.chat(addrA, "/unregister @Ben")
	if r.Code != "" || firstLine(r) != "Removed @ben." {
		t.Errorf("reply = %q (code %q)", r.Text, r.Code)
	}
	if m := trailer(t, r); m["ok"] != true || m["cmd"] != "unregister" || m["nick"] != "@ben" {
		t.Errorf("trailer = %v", m)
	}
	if r := f.chat(addrB, "/register @ben"); r.Code != "taken" {
		t.Errorf("the name is held for its old owner: code %q", r.Code)
	}
	f.clock.advance(store.Quarantine + time.Second)
	if r := f.chat(addrB, "/register @ben"); r.Code != "" {
		t.Errorf("after the hold: code %q", r.Code)
	}
}

func TestAddress(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @Ben")
	r := f.chat(addrB, "/address @ben")
	if firstLine(r) != "@Ben is "+addrA+"." {
		t.Errorf("first line = %q", firstLine(r))
	}
	m := trailer(t, r)
	if m["ok"] != true || m["found"] != true || m["nick"] != "@Ben" || m["address"] != addrA || m["query"] != "@ben" {
		t.Errorf("trailer = %v", m)
	}
	r = f.chat(addrB, "/address @nobody")
	if firstLine(r) != "@nobody is not registered." || r.Code != "" {
		t.Errorf("not found: %q (code %q)", firstLine(r), r.Code)
	}
	if m := trailer(t, r); m["ok"] != true || m["found"] != false || m["query"] != "@nobody" || m["address"] != nil {
		t.Errorf("trailer = %v", m)
	}
	for _, text := range []string{"/address", "/address @a @b"} {
		if r := f.chat(addrB, text); r.Code != "usage" {
			t.Errorf("%q: code %q", text, r.Code)
		}
	}
	if r := f.chat(addrB, "/address ben"); r.Code != "syntax" {
		t.Errorf("no sigil: code %q", r.Code)
	}
}

func TestNickname(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @ben")
	f.chat(addrA, "/register /benbot")
	r := f.chat(addrB, "/nickname "+addrA)
	if firstLine(r) != addrA+" is @ben /benbot." {
		t.Errorf("first line = %q", firstLine(r))
	}
	m := trailer(t, r)
	results := m["results"].([]any)
	if len(results) != 1 {
		t.Fatalf("results = %v", results)
	}
	one := results[0].(map[string]any)
	nicks := one["nicks"].([]any)
	if one["address"] != addrA || len(nicks) != 2 || nicks[0] != "@ben" || nicks[1] != "/benbot" {
		t.Errorf("result = %v", one)
	}
	r = f.chat(addrB, "/nickname "+addrC)
	if firstLine(r) != addrC+" has no nickname." {
		t.Errorf("first line = %q", firstLine(r))
	}
	// The answer for a nobody is a list that is empty, not a missing one.
	one = trailer(t, r)["results"].([]any)[0].(map[string]any)
	if nicks, ok := one["nicks"].([]any); !ok || len(nicks) != 0 {
		t.Errorf("nicks = %v", one["nicks"])
	}
}

// addresses makes n different valid addresses.
func addresses(n int) []string {
	var list []string
	for i := 0; i < n; i++ {
		a := bmaddr.Address{Version: 4, Stream: 1}
		for j := range a.Ripe {
			a.Ripe[j] = byte(1 + i*7 + j)
		}
		list = append(list, a.String())
	}
	return list
}

func TestNicknameBatch(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @alpha")
	f.chat(addrB, "/register @bravo")
	r := f.chat(addrC, fmt.Sprintf("/nickname %s %s %s %s", addrA, addrB, addrC, addrA))
	results := trailer(t, r)["results"].([]any)
	if len(results) != 3 {
		t.Fatalf("%d results; a repeated address is asked once: %v", len(results), results)
	}
	if lines := strings.Split(r.Text, "\n"); len(lines) != 4 { // three answers and the trailer
		t.Errorf("%d lines: %q", len(lines), r.Text)
	}
	first := results[0].(map[string]any)
	if first["address"] != addrA || first["nicks"].([]any)[0] != "@alpha" {
		t.Errorf("the answers keep the order asked: %v", results)
	}
	eight := addresses(8)
	if r := f.chat(addrC, "/nickname "+strings.Join(eight, " ")); r.Code != "" || len(trailer(t, r)["results"].([]any)) != 8 {
		t.Errorf("eight addresses: code %q", r.Code)
	}
	if r := f.chat(addrC, "/nickname "+strings.Join(addresses(9), " ")); r.Code != "usage" {
		t.Errorf("nine addresses: code %q", r.Code)
	}
	// Nine asked but only eight different: that is eight.
	again := append(append([]string{}, eight...), eight[0])
	if r := f.chat(addrC, "/nickname "+strings.Join(again, " ")); r.Code != "" {
		t.Errorf("eight different addresses, one of them twice: code %q", r.Code)
	}
}

func TestNicknameSyntax(t *testing.T) {
	f := newFixture(t)
	for _, tc := range []struct{ text, code string }{
		{"/nickname", "usage"},
		{"/nickname BM-nope", "syntax"},
		{"/nickname " + addrA + " BM-nope", "syntax"},
		{"/nickname " + addrA[:len(addrA)-1] + "x", "syntax"},
		{"/nickname " + strings.ToLower(addrA), "syntax"},
		{"/nickname " + addrA[3:], "syntax"},
	} {
		if r := f.chat(addrB, tc.text); r.Code != tc.code {
			t.Errorf("%q: code %q, want %q", tc.text, r.Code, tc.code)
		}
	}
}

func TestSearch(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @benjamin")
	f.chat(addrB, "/register @benedict")
	f.chat(addrC, "/register /benbot")
	r := f.chat(addrAnon, "/search BENE")
	if firstLine(r) != `1 name for "bene":` {
		t.Errorf("first line = %q", firstLine(r))
	}
	m := trailer(t, r)
	list := m["results"].([]any)
	if len(list) != 1 || list[0].(map[string]any)["nick"] != "@benedict" || list[0].(map[string]any)["address"] != addrB {
		t.Errorf("results = %v", list)
	}
	if m["more"] != false || m["query"] != "bene" {
		t.Errorf("trailer = %v", m)
	}
	r = f.chat(addrAnon, "/search ben")
	if r.Code != "too_short" {
		t.Errorf("3 characters: code %q", r.Code)
	}
	r = f.chat(addrAnon, "/search @ben")
	if r.Code != "too_short" {
		t.Errorf("@ben: the sigil is not counted: code %q", r.Code)
	}
	r = f.chat(addrAnon, "/search @benj")
	if len(trailer(t, r)["results"].([]any)) != 1 {
		t.Errorf("@benj: %q", r.Text)
	}
	r = f.chat(addrAnon, "/search nothing")
	if firstLine(r) != `No names for "nothing".` {
		t.Errorf("first line = %q", firstLine(r))
	}
	if list := trailer(t, r)["results"].([]any); len(list) != 0 {
		t.Errorf("results = %v", list)
	}
}

func TestSearchAnswersAtMostTenAndSaysThereAreMore(t *testing.T) {
	f := newFixture(t)
	for i := 0; i < 12; i++ {
		if err := f.store.Seed([]store.SeedEntry{{Nick: mustNick(t, fmt.Sprintf("@many%02d", i)), Address: fmt.Sprintf("BM-many%02d", i)}}); err != nil {
			t.Fatal(err)
		}
	}
	r := f.chat(addrB, "/search many")
	m := trailer(t, r)
	if len(m["results"].([]any)) != 10 || m["more"] != true {
		t.Errorf("results %d, more %v", len(m["results"].([]any)), m["more"])
	}
	if firstLine(r) != `10 names for "many" (there are more):` {
		t.Errorf("first line = %q", firstLine(r))
	}
}

func TestSearchRefusesWhatIsNotATerm(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @victim")
	for _, text := range []string{
		"/search '", "/search %%%%", "/search a_b%", "/search ';DROP", "/search ben bob", "/search", "/search \\",
		"/search " + strings.Repeat("a", 33), "/search béna", "/search @@bena",
	} {
		r := f.chat(addrB, text)
		if r.Code != "syntax" && r.Code != "usage" && r.Code != "too_short" {
			t.Errorf("%q: code %q (%q)", text, r.Code, firstLine(r))
		}
	}
	if e, ok, _ := f.store.Reg().ByNick(mustNick(t, "@victim")); !ok || e.Address != addrA {
		t.Error("the registry changed")
	}
}

func TestWhoami(t *testing.T) {
	f := newFixture(t)
	r := f.chat(addrA, "/whoami")
	if firstLine(r) != "You have no nickname." {
		t.Errorf("first line = %q", firstLine(r))
	}
	// No names is a list with nothing in it, not a missing list.
	if nicks, ok := trailer(t, r)["nicks"].([]any); !ok || len(nicks) != 0 {
		t.Errorf("nicks = %v", trailer(t, r)["nicks"])
	}
	f.chat(addrA, "/register @ben")
	f.chat(addrA, "/register /benbot")
	r = f.chat(addrA, "/whoami")
	if firstLine(r) != "You are @ben /benbot." {
		t.Errorf("first line = %q", firstLine(r))
	}
	m := trailer(t, r)
	if m["address"] != addrA || len(m["nicks"].([]any)) != 2 {
		t.Errorf("trailer = %v", m)
	}
}

func TestUnknownThings(t *testing.T) {
	f := newFixture(t)
	for _, text := range []string{"hello", "/", "/nope", "/register_all @ben", "", "   ", "register @ben"} {
		r := f.chat(addrA, text)
		if r.Code != "unknown_command" {
			t.Errorf("%q: code %q", text, r.Code)
		}
		if !strings.Contains(r.Text, "/help") {
			t.Errorf("%q: the answer does not point to /help: %q", text, r.Text)
		}
	}
}

func TestWhitespaceAroundACommandIsIgnored(t *testing.T) {
	for _, text := range []string{"\n/register @ben", "  /register   @ben  ", "\t/register\t@ben\n", "\r\n\r\n/register @ben\r\nthanks!"} {
		g := newFixture(t)
		if r := g.chat(addrA, text); r.Code != "" || r.Cmd != "register" {
			t.Errorf("%q: %+v", text, r)
		}
	}
}

func TestLettersGetLettersWithoutATrailer(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @ben")
	r := f.letter(addrB, "who is ben?", "/address @ben")
	if r.Subject != "Re: who is ben?" {
		t.Errorf("subject = %q", r.Subject)
	}
	if strings.Contains(r.Text, "nickbot:") || strings.Contains(r.Text, "{") {
		t.Errorf("a letter reply has a trailer: %q", r.Text)
	}
	if firstLine(r) != "@ben is "+addrA+"." {
		t.Errorf("first line = %q", firstLine(r))
	}
	// The command may be in the subject, and the body's first line wins when there is one.
	if r := f.letter(addrB, "/address @ben", ""); firstLine(r) != "@ben is "+addrA+"." {
		t.Errorf("command in the subject: %q", r.Text)
	}
	if r := f.letter(addrB, "/help", "\n\n  /address @ben\nthanks"); firstLine(r) != "@ben is "+addrA+"." {
		t.Errorf("command after blank lines: %q", r.Text)
	}
	if r := f.letter(addrB, "hi", "just saying hello"); r.Code != "unknown_command" || !strings.Contains(r.Text, "/help") {
		t.Errorf("not a command: %+v", r)
	}
	if r := f.letter(addrB, "", "/help"); r.Subject != "Re: nickbot" {
		t.Errorf("no subject: %q", r.Subject)
	}
}

func TestTheReplySubjectIsOneShortLine(t *testing.T) {
	f := newFixture(t)
	r := f.letter(addrB, "line one\r\nline two\x00"+strings.Repeat("x", 500), "/help")
	if strings.ContainsAny(r.Subject, "\r\n\x00") || len(r.Subject) > 120 {
		t.Errorf("subject = %q", r.Subject)
	}
	if !strings.HasPrefix(r.Subject, "Re: line one") {
		t.Errorf("subject = %q", r.Subject)
	}
}

func TestQuotas(t *testing.T) {
	f := newFixture(t, func(o *Options) { o.ChangesPerDay = 3 })
	for _, nickname := range []string{"@aaa", "@bbb", "@ccc"} {
		if r := f.chat(addrA, "/register "+nickname); r.Code != "" {
			t.Fatalf("%s: code %q", nickname, r.Code)
		}
	}
	if r := f.chat(addrA, "/register @ddd"); r.Code != "rate_limited" {
		t.Errorf("a fourth change in a day: code %q", r.Code)
	}
	// Saying what is already so costs nothing.
	if r := f.chat(addrA, "/register @ccc"); r.Code != "" {
		t.Errorf("registering the name you have: code %q", r.Code)
	}
	// Other addresses have their own quota.
	if r := f.chat(addrB, "/register @ddd"); r.Code != "" {
		t.Errorf("another address: code %q", r.Code)
	}
	f.clock.advance(24*time.Hour + time.Second)
	if r := f.chat(addrA, "/register @ddd"); r.Code != "taken" { // taken by addrB above, but not rate limited
		t.Errorf("after a day: code %q", r.Code)
	}
	if r := f.chat(addrA, "/register @eee"); r.Code != "" {
		t.Errorf("after a day: code %q", r.Code)
	}
}

func TestGlobalBudgetAndCapacity(t *testing.T) {
	f := newFixture(t, func(o *Options) { o.NewPerHour = 2 })
	f.chat(addrA, "/register @aaa")
	f.chat(addrB, "/register @bbb")
	if r := f.chat(addrC, "/register @ccc"); r.Code != "busy" {
		t.Errorf("a third new name within the hour: code %q", r.Code)
	}
	f.clock.advance(time.Hour + time.Second)
	if r := f.chat(addrC, "/register @ccc"); r.Code != "" {
		t.Errorf("the next hour: code %q", r.Code)
	}
	g := newFixture(t, func(o *Options) { o.MaxNicks = 1 })
	g.chat(addrA, "/register @aaa")
	if r := g.chat(addrB, "/register @bbb"); r.Code != "busy" {
		t.Errorf("a full registry: code %q", r.Code)
	}
	if r := g.chat(addrA, "/register @aaa"); r.Code != "" {
		t.Errorf("a full registry still knows its own: code %q", r.Code)
	}
	if r := g.chat(addrA, "/register @zzz"); r.Code != "" {
		t.Errorf("a change in a full registry adds no name: code %q", r.Code)
	}
}

func TestNothingUnvalidatedIsEchoed(t *testing.T) {
	f := newFixture(t)
	hostile := []string{"<script>alert(1)</script>", "[click](http://evil.example)", "http://evil.example/x", "\u202egnp.exe", "`code`"}
	for _, h := range hostile {
		for _, cmd := range []string{"register", "unregister", "address", "nickname", "search", "whoami", "nonsense"} {
			for _, text := range []string{"/" + cmd + " " + h, "/" + cmd + " @" + h, "/" + cmd + " BM-" + h} {
				r := f.chat(addrA, text)
				if strings.Contains(r.Text, h) || strings.Contains(r.Text, "evil") || strings.Contains(r.Text, "script") {
					t.Errorf("%q echoed hostile text: %q", text, r.Text)
				}
			}
		}
	}
	if r := f.chat(addrA, "/"+strings.Repeat("x", 5000)); strings.Contains(r.Text, "xxxxxxxx") {
		t.Errorf("a long command word was echoed")
	}
}

func TestEveryReplyEndsWithOneLineOfJSON(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @ben")
	for _, text := range []string{
		"/help", "/register @ben", "/register nope", "/unregister @ben", "/address @ben", "/address @none", "/nickname " + addrA,
		"/search benn", "/whoami", "hello", "/register #room", "/search", "/nickname",
	} {
		r := f.chat(addrB, text)
		lines := strings.Split(r.Text, "\n")
		last := lines[len(lines)-1]
		if !strings.HasPrefix(last, "nickbot: {") || !json.Valid([]byte(strings.TrimPrefix(last, "nickbot: "))) {
			t.Errorf("%q: bad trailer %q", text, last)
		}
		for _, line := range lines[:len(lines)-1] {
			if strings.HasPrefix(line, "nickbot:") {
				t.Errorf("%q: a trailer before the end: %q", text, line)
			}
		}
		if len(r.Text) > 8000 {
			t.Errorf("%q: the reply is %d bytes; a chat holds 8192", text, len(r.Text))
		}
	}
}

func TestTheTrailerFormatIsFixed(t *testing.T) {
	f := newFixture(t)
	f.chat(addrA, "/register @ben")
	r := f.chat(addrB, "/address @ben")
	want := `nickbot: {"v":1,"cmd":"address","ok":true,"query":"@ben","found":true,"nick":"@ben","address":"` + addrA + `"}`
	lines := strings.Split(r.Text, "\n")
	if got := lines[len(lines)-1]; got != want {
		t.Errorf("trailer\n got  %s\n want %s", got, want)
	}
	r = f.chat(addrB, "/address @none")
	want = `nickbot: {"v":1,"cmd":"address","ok":true,"query":"@none","found":false}`
	lines = strings.Split(r.Text, "\n")
	if got := lines[len(lines)-1]; got != want {
		t.Errorf("trailer\n got  %s\n want %s", got, want)
	}
	r = f.chat(addrB, "/register @ben")
	want = `nickbot: {"v":1,"cmd":"register","ok":false,"error":"taken","query":"@ben"}`
	lines = strings.Split(r.Text, "\n")
	if got := lines[len(lines)-1]; got != want {
		t.Errorf("trailer\n got  %s\n want %s", got, want)
	}
}

func TestADatabaseFailureIsAnInternalReplyNotAPanic(t *testing.T) {
	f := newFixture(t)
	f.store.Close()
	for _, text := range []string{"/register @ben", "/address @ben", "/search benn", "/nickname " + addrA, "/whoami", "/unregister @ben"} {
		r := f.chat(addrA, text)
		if r.Code != "internal" {
			t.Errorf("%q: code %q", text, r.Code)
		}
	}
}

func TestHugeInputIsHandled(t *testing.T) {
	f := newFixture(t)
	for _, text := range []string{
		"/register " + strings.Repeat("@a", 100000),
		"/nickname " + strings.Repeat(addrA+" ", 5000),
		"/search " + strings.Repeat("a", 1<<20),
		strings.Repeat("/", 1<<20),
	} {
		r := f.chat(addrA, text)
		if r.Code != "usage" || len(r.Text) > 2000 {
			t.Errorf("a %d byte command: code %q, %d bytes of reply; too long is refused unread", len(text), r.Code, len(r.Text))
		}
	}
}

func TestIsAnonymous(t *testing.T) {
	f := newFixture(t)
	if !f.svc.IsAnonymous(addrAnon) || f.svc.IsAnonymous(addrA) {
		t.Error("IsAnonymous is wrong")
	}
	g := newFixture(t, func(o *Options) { o.Anonymous = "" })
	if g.svc.IsAnonymous("") || g.svc.IsAnonymous(addrAnon) {
		t.Error("with no shared address nobody is anonymous")
	}
}
