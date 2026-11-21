package service_test

import (
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/yshurik/ynotbit/nickbot/internal/limit"
	"github.com/yshurik/ynotbit/nickbot/internal/loop"
	"github.com/yshurik/ynotbit/nickbot/internal/service"
	"github.com/yshurik/ynotbit/nickbot/internal/store"
)

// The loop is shared by every bot and tested with a fake service. These tests run it with nickbot's
// own service and registry, which is how nickbot runs.

const (
	botAddr  = "BM-bot"
	addrA    = "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN"
	addrAnon = "BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r"
)

type sentMessage struct{ kind, from, to, subject, text string }

type fakeBot struct {
	events  []loop.Event
	sent    []sentMessage
	done    []string
	doneErr error
}

func (b *fakeBot) Next(time.Duration) (loop.Event, bool, error) {
	if len(b.events) == 0 {
		return loop.Event{}, false, nil
	}
	e := b.events[0]
	b.events = b.events[1:]
	return e, true, nil
}

func (b *fakeBot) Done(id string) error {
	if b.doneErr != nil {
		return b.doneErr
	}
	b.done = append(b.done, id)
	return nil
}

func (b *fakeBot) SendChat(from, to, text string) (string, error) {
	b.sent = append(b.sent, sentMessage{kind: "chat", from: from, to: to, text: text})
	return "chat-1", nil
}

func (b *fakeBot) SendLetter(from, to, subject, body string) (string, error) {
	b.sent = append(b.sent, sentMessage{kind: "letter", from: from, to: to, subject: subject, text: body})
	return "letter-1", nil
}

type nickRig struct {
	bot   *fakeBot
	loop  *loop.Loop
	store *store.Store
}

func newNickRig(t *testing.T, mutate ...func(*loop.Config)) *nickRig {
	t.Helper()
	now := func() time.Time { return time.Unix(1_800_000_000, 0) }
	st, err := store.Open(filepath.Join(t.TempDir(), "n.sqlite"), store.WithClock(now))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	svc := service.New(st, service.Options{
		Anonymous:   addrAnon,
		Now:         now,
		ChanAddress: func(p string) (string, error) { return "BM-chan-of-" + p, nil },
	})
	r := &nickRig{bot: &fakeBot{}, store: st}
	cfg := loop.Config{Now: now, RateNotice: service.RateLimited}
	for _, m := range mutate {
		m(&cfg)
	}
	r.loop = loop.New(r.bot, svc, cfg)
	return r
}

func (r *nickRig) chat(id, text string) {
	r.bot.events = append(r.bot.events, loop.Event{ID: id, Kind: "chat", To: botAddr, From: addrA, Text: text})
}

func (r *nickRig) drain(t *testing.T) {
	t.Helper()
	for len(r.bot.events) > 0 {
		if _, err := r.loop.Step(0); err != nil {
			t.Fatal(err)
		}
	}
}

func TestTheLoopAnswersAChatWithNickbotsWordsAndTrailer(t *testing.T) {
	r := newNickRig(t)
	r.chat("c1", "/register @ben")
	r.drain(t)
	if len(r.bot.sent) != 1 {
		t.Fatalf("%d replies", len(r.bot.sent))
	}
	got := r.bot.sent[0]
	if got.kind != "chat" || got.from != botAddr || got.to != addrA || !strings.HasPrefix(got.text, "Registered @ben") || !strings.Contains(got.text, "\nnickbot: {") {
		t.Errorf("reply = %+v", got)
	}
}

func TestTheLoopAnswersALetterWithAPlainLetter(t *testing.T) {
	r := newNickRig(t)
	r.bot.events = append(r.bot.events, loop.Event{ID: "l1", Kind: "letter", To: botAddr, From: addrA, Subject: "hello", Text: "/help"})
	r.drain(t)
	got := r.bot.sent[0]
	if got.kind != "letter" || got.subject != "Re: hello" || got.to != addrA || got.from != botAddr || strings.Contains(got.text, "nickbot: {") {
		t.Errorf("reply = %+v", got)
	}
}

func TestAnEventTheLibraryHandsOutTwiceChangesTheRegistryOnce(t *testing.T) {
	r := newNickRig(t)
	r.chat("c1", "/register @ben")
	r.bot.doneErr = errors.New("crash before done")
	r.drain(t)
	// The program restarts; the library hands the event out again.
	r.bot.doneErr = nil
	r.chat("c1", "/register @ben")
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

// nickbot's "slow down" has its error code in its trailer, which a program reads.
func TestAnOverRateSenderIsToldSoInNickbotsOwnVoice(t *testing.T) {
	r := newNickRig(t, func(c *loop.Config) { c.PerSender = limit.Config{Burst: 3, Every: time.Hour} })
	for i := 0; i < 10; i++ {
		r.chat(fmt.Sprintf("c%d", i), "/help")
	}
	r.drain(t)
	if len(r.bot.sent) != 4 {
		t.Fatalf("%d replies: three answers and one notice", len(r.bot.sent))
	}
	notice := r.bot.sent[3]
	if !strings.Contains(notice.text, `"error":"rate_limited"`) || !strings.Contains(notice.text, "Slow down") {
		t.Errorf("the fourth reply is %q", notice.text)
	}
}
