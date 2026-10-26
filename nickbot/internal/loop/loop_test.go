package loop

import (
	"context"
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/yshurik/ynotbit/nickbot/internal/limit"
	"github.com/yshurik/ynotbit/nickbot/internal/service"
	"github.com/yshurik/ynotbit/nickbot/internal/store"
)

const (
	botAddr  = "BM-87echoAkzstiHuPGSN8PH88m4WPm4attre8"
	addrA    = "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN"
	addrB    = "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6"
	addrAnon = "BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r"
)

type clock struct{ t time.Time }

func (c *clock) now() time.Time          { return c.t }
func (c *clock) advance(d time.Duration) { c.t = c.t.Add(d) }

type sent struct {
	kind, from, to, subject, text string
}

// fakeBot is the library, with its events in a list.
type fakeBot struct {
	events     []Event
	sent       []sent
	done       []string
	calls      []string // "send" and "done:<id>", in order
	sendErr    error
	doneErr    error
	nextErr    error
	nextCalled int
}

func (b *fakeBot) Next(timeout time.Duration) (Event, bool, error) {
	b.nextCalled++
	if b.nextErr != nil {
		return Event{}, false, b.nextErr
	}
	if len(b.events) == 0 {
		return Event{}, false, nil
	}
	e := b.events[0]
	b.events = b.events[1:]
	return e, true, nil
}

func (b *fakeBot) Done(id string) error {
	b.calls = append(b.calls, "done:"+id)
	if b.doneErr != nil {
		return b.doneErr
	}
	b.done = append(b.done, id)
	return nil
}

func (b *fakeBot) SendChat(from, to, text string) (string, error) {
	b.calls = append(b.calls, "send")
	if b.sendErr != nil {
		return "", b.sendErr
	}
	b.sent = append(b.sent, sent{kind: "chat", from: from, to: to, text: text})
	return "chat-1", nil
}

func (b *fakeBot) SendLetter(from, to, subject, body string) (string, error) {
	b.calls = append(b.calls, "send")
	if b.sendErr != nil {
		return "", b.sendErr
	}
	b.sent = append(b.sent, sent{kind: "letter", from: from, to: to, subject: subject, text: body})
	return "letter-1", nil
}

type permanentError struct{}

func (permanentError) Error() string   { return "bad address" }
func (permanentError) Permanent() bool { return true }

type rig struct {
	bot   *fakeBot
	loop  *Loop
	svc   *service.Service
	store *store.Store
	clock *clock
	logs  []string
}

func newRig(t *testing.T, mutate ...func(*Config)) *rig {
	t.Helper()
	c := &clock{t: time.Unix(1_800_000_000, 0)}
	st, err := store.Open(filepath.Join(t.TempDir(), "n.sqlite"), store.WithClock(c.now))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	svc := service.New(st, service.Options{
		Anonymous:   addrAnon,
		Now:         c.now,
		ChanAddress: func(p string) (string, error) { return "BM-chan-of-" + p, nil },
	})
	r := &rig{bot: &fakeBot{}, svc: svc, store: st, clock: c}
	cfg := Config{
		Now: c.now,
		Log: func(format string, args ...any) { r.logs = append(r.logs, fmt.Sprintf(format, args...)) },
	}
	for _, m := range mutate {
		m(&cfg)
	}
	r.loop = New(r.bot, svc, cfg)
	return r
}

func (r *rig) chat(id, from, text string) {
	r.bot.events = append(r.bot.events, Event{ID: id, Kind: "chat", To: botAddr, From: from, Text: text})
}

func (r *rig) letter(id, from, subject, body string) {
	r.bot.events = append(r.bot.events, Event{ID: id, Kind: "letter", To: botAddr, From: from, Subject: subject, Text: body})
}

// drain handles every event that is waiting.
func (r *rig) drain(t *testing.T) {
	t.Helper()
	for len(r.bot.events) > 0 {
		if _, err := r.loop.Step(0); err != nil {
			t.Fatal(err)
		}
	}
}

func TestAChatIsAnsweredFromTheAddressItCameToAndDone(t *testing.T) {
	r := newRig(t)
	r.chat("c1", addrA, "/register @ben")
	r.drain(t)
	if len(r.bot.sent) != 1 {
		t.Fatalf("%d replies", len(r.bot.sent))
	}
	got := r.bot.sent[0]
	if got.kind != "chat" || got.from != botAddr || got.to != addrA || !strings.HasPrefix(got.text, "Registered @ben") || !strings.Contains(got.text, "\nnickbot: {") {
		t.Errorf("reply = %+v", got)
	}
	if fmt.Sprint(r.bot.calls) != "[send done:c1]" {
		t.Errorf("calls = %v: the event is done only after its answer is queued", r.bot.calls)
	}
}

func TestALetterIsAnsweredWithALetter(t *testing.T) {
	r := newRig(t)
	r.letter("l1", addrA, "hello", "/help")
	r.drain(t)
	got := r.bot.sent[0]
	if got.kind != "letter" || got.subject != "Re: hello" || got.to != addrA || got.from != botAddr || strings.Contains(got.text, "nickbot: {") {
		t.Errorf("reply = %+v", got)
	}
}

func TestAnEventTheLibraryHandsOutTwiceIsDoneTwiceAndNotDoneTwice(t *testing.T) {
	r := newRig(t)
	r.chat("c1", addrA, "/register @ben")
	r.bot.doneErr = errors.New("crash before done")
	r.drain(t)
	// The program restarts; the library hands the event out again.
	r.bot.doneErr = nil
	r.chat("c1", addrA, "/register @ben")
	r.drain(t)
	if len(r.bot.sent) != 2 || r.bot.sent[0].text != r.bot.sent[1].text {
		t.Fatalf("the answer to a repeated event must be the first one again: %+v", r.bot.sent)
	}
	rows, _ := r.store.History(addrA, 10)
	if len(rows) != 1 {
		t.Errorf("%d history rows for one event", len(rows))
	}
	if len(r.bot.done) != 1 || r.bot.done[0] != "c1" {
		t.Errorf("done = %v", r.bot.done)
	}
}

func TestASenderOverTheRateGetsOneNoticeAndThenSilence(t *testing.T) {
	r := newRig(t, func(c *Config) { c.PerSender = limit.Config{Burst: 3, Every: time.Hour} })
	for i := 0; i < 10; i++ {
		r.chat(fmt.Sprintf("c%d", i), addrA, "/help")
	}
	r.drain(t)
	if len(r.bot.sent) != 4 {
		t.Fatalf("%d replies: three answers and one notice", len(r.bot.sent))
	}
	notice := r.bot.sent[3]
	if !strings.Contains(notice.text, `"error":"rate_limited"`) || !strings.Contains(notice.text, "Slow down") {
		t.Errorf("the fourth reply is %q", notice.text)
	}
	if len(r.bot.done) != 10 {
		t.Errorf("%d events done; every one is read, answered or not", len(r.bot.done))
	}
	// A minute later the sender gets another notice if it is still over.
	r.clock.advance(time.Minute + time.Second)
	for i := 0; i < 3; i++ {
		r.chat(fmt.Sprintf("d%d", i), addrA, "/help")
	}
	r.drain(t)
	if len(r.bot.sent) != 5 || !strings.Contains(r.bot.sent[4].text, "rate_limited") {
		t.Errorf("%d replies after the minute", len(r.bot.sent))
	}
}

func TestOneSendersExcessDoesNotCostAnotherItsAnswers(t *testing.T) {
	r := newRig(t, func(c *Config) { c.PerSender = limit.Config{Burst: 1, Every: time.Hour} })
	r.chat("a1", addrA, "/help")
	r.chat("a2", addrA, "/help")
	r.chat("b1", addrB, "/help")
	r.drain(t)
	to := map[string]int{}
	for _, s := range r.bot.sent {
		to[s.to]++
	}
	if to[addrA] != 2 || to[addrB] != 1 { // A: one answer and one notice
		t.Errorf("replies by address: %v", to)
	}
}

func TestTheSharedAddressHasABucketOfItsOwn(t *testing.T) {
	r := newRig(t, func(c *Config) {
		c.PerSender = limit.Config{Burst: 1, Every: time.Hour}
		c.Anonymous = limit.Config{Burst: 5, Every: time.Hour}
	})
	r.chat("a1", addrA, "/help")
	r.chat("a2", addrA, "/help")
	for i := 0; i < 8; i++ {
		r.chat(fmt.Sprintf("n%d", i), addrAnon, "/search nothing")
	}
	r.drain(t)
	var answers, notices int
	for _, s := range r.bot.sent {
		if s.to == addrAnon {
			if strings.Contains(s.text, "rate_limited") {
				notices++
			} else {
				answers++
			}
		}
	}
	if answers != 5 || notices != 1 {
		t.Errorf("the shared address got %d answers and %d notices", answers, notices)
	}
}

func TestAnUnknownCommandIsAnsweredTwiceAndThenIgnored(t *testing.T) {
	r := newRig(t)
	for i := 0; i < 6; i++ {
		r.chat(fmt.Sprintf("c%d", i), addrA, "hello")
	}
	r.drain(t)
	if len(r.bot.sent) != 2 {
		t.Fatalf("%d replies to chatter; two bots that answer each other's words must run out", len(r.bot.sent))
	}
	if len(r.bot.done) != 6 {
		t.Errorf("%d done", len(r.bot.done))
	}
	// Real commands are not held back by it.
	r.chat("real", addrA, "/help")
	r.drain(t)
	if len(r.bot.sent) != 3 {
		t.Errorf("a command after the chatter got no answer")
	}
}

func TestTheGlobalBudgetStopsAllAnswers(t *testing.T) {
	r := newRig(t, func(c *Config) { c.ReplyBudget = limit.Config{Burst: 4, Every: time.Hour} })
	for i := 0; i < 10; i++ {
		r.chat(fmt.Sprintf("c%d", i), fmt.Sprintf("BM-sender%d", i), "/help")
	}
	r.drain(t)
	if len(r.bot.sent) != 4 {
		t.Errorf("%d replies, want the budget of 4", len(r.bot.sent))
	}
	if len(r.bot.done) != 10 {
		t.Errorf("%d done: the ones over budget are read and dropped", len(r.bot.done))
	}
	if r.loop.Stats().Dropped != 6 {
		t.Errorf("stats = %+v", r.loop.Stats())
	}
}

func TestNotesAreLoggedAndHaveNothingToDo(t *testing.T) {
	r := newRig(t)
	r.bot.events = append(r.bot.events, Event{Kind: "note", Text: "the node was restarted"})
	r.drain(t)
	if len(r.bot.done) != 0 || len(r.bot.sent) != 0 {
		t.Errorf("a note was answered or done: %+v %v", r.bot.sent, r.bot.done)
	}
	found := false
	for _, l := range r.logs {
		found = found || strings.Contains(l, "the node was restarted")
	}
	if !found {
		t.Errorf("logs = %q", r.logs)
	}
}

func TestAPermanentSendErrorIsDoneButATransientOneIsNot(t *testing.T) {
	r := newRig(t)
	r.chat("c1", addrA, "/help")
	r.bot.sendErr = permanentError{}
	r.drain(t)
	if len(r.bot.done) != 1 {
		t.Errorf("a reply that can never be sent leaves the event for ever: done = %v", r.bot.done)
	}
	r.bot.done = nil
	r.chat("c2", addrA, "/help")
	r.bot.sendErr = errors.New("the node is down")
	r.drain(t)
	if len(r.bot.done) != 0 {
		t.Errorf("a reply that could not be queued for now was thrown away: done = %v", r.bot.done)
	}
	if r.loop.Stats().Errors != 2 {
		t.Errorf("stats = %+v", r.loop.Stats())
	}
}

func TestADoneThatFailsIsLoggedNotFatal(t *testing.T) {
	r := newRig(t)
	r.bot.doneErr = errors.New("closed")
	r.chat("c1", addrA, "/help")
	r.chat("c2", addrA, "/help")
	r.drain(t)
	if len(r.bot.sent) != 2 {
		t.Errorf("%d replies; one failure must not stop the next", len(r.bot.sent))
	}
}

func TestStepReturnsTheLibrarysError(t *testing.T) {
	r := newRig(t)
	r.bot.nextErr = errors.New("the bot is closed")
	if _, err := r.loop.Step(0); err == nil {
		t.Error("an error from the library was swallowed")
	}
}

func TestRunStopsWhenTheContextEnds(t *testing.T) {
	r := newRig(t)
	ctx, cancel := context.WithCancel(context.Background())
	r.chat("c1", addrA, "/help")
	done := make(chan error, 1)
	go func() { done <- r.loop.Run(ctx) }()
	deadline := time.Now().Add(5 * time.Second)
	for r.loop.Stats().Answered == 0 && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Errorf("Run = %v after a cancel", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Run did not stop")
	}
	if r.loop.Stats().Answered != 1 {
		t.Errorf("stats = %+v", r.loop.Stats())
	}
}

func TestRunReturnsAnErrorOfTheLibrary(t *testing.T) {
	r := newRig(t)
	r.bot.nextErr = errors.New("the bot is closed")
	if err := r.loop.Run(context.Background()); err == nil {
		t.Error("Run returned nil though the library failed")
	}
}

func TestStats(t *testing.T) {
	r := newRig(t, func(c *Config) { c.PerSender = limit.Config{Burst: 2, Every: time.Hour} })
	r.chat("c1", addrA, "/help")
	r.letter("l1", addrA, "x", "/help")
	r.chat("c2", addrA, "/help")
	r.chat("c3", addrA, "/help")
	r.drain(t)
	st := r.loop.Stats()
	if st.Received != 4 || st.Chats != 3 || st.Letters != 1 || st.Answered != 2 || st.Limited != 2 || st.Notices != 1 {
		t.Errorf("stats = %+v", st)
	}
}
