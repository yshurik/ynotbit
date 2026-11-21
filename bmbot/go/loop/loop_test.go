package loop_test

import (
	"context"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"testing"
	"time"
	"unicode"
	"unicode/utf8"

	"github.com/yshurik/ynotbit/bmbot/go/limit"
	"github.com/yshurik/ynotbit/bmbot/go/loop"
)

const (
	botAddr  = "BM-bot"
	addrA    = "BM-sender-a"
	addrB    = "BM-sender-b"
	addrAnon = "BM-shared"
)

type clock struct{ t time.Time }

func (c *clock) now() time.Time          { return c.t }
func (c *clock) advance(d time.Duration) { c.t = c.t.Add(d) }

type sent struct {
	kind, from, to, subject, text string
}

// fakeBot is the library, with its events in a list.
type fakeBot struct {
	events     []loop.Event
	sent       []sent
	done       []string
	calls      []string // "send" and "done:<id>", in order
	sendErr    error
	doneErr    error
	nextErr    error
	nextCalled int
}

func (b *fakeBot) Next(timeout time.Duration) (loop.Event, bool, error) {
	b.nextCalled++
	if b.nextErr != nil {
		return loop.Event{}, false, b.nextErr
	}
	if len(b.events) == 0 {
		return loop.Event{}, false, nil
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

// fakeHandler is a service with one kind of command (a line that starts with /), a trailer of its
// own on chat answers, and an "unknown_command" code for any other text.
type fakeHandler struct {
	anonymous string
	calls     []loop.Request
}

func (h *fakeHandler) IsAnonymous(address string) bool { return address == h.anonymous }

func (h *fakeHandler) Handle(req loop.Request) loop.Reply {
	h.calls = append(h.calls, req)
	if !strings.HasPrefix(req.Text, "/") {
		return loop.Reply{Text: "That is no command.", Subject: "Re: " + req.Subject, Code: "unknown_command"}
	}
	r := loop.Reply{Text: "ok " + req.Text, Subject: "Re: " + req.Subject, Cmd: strings.TrimPrefix(req.Text, "/")}
	if req.Transport == loop.Chat {
		r.Text += "\nfake: {\"v\":1}"
	}
	return r
}

type rig struct {
	bot     *fakeBot
	handler *fakeHandler
	loop    *loop.Loop
	clock   *clock
	logs    []string
}

func newRig(t *testing.T, mutate ...func(*loop.Config)) *rig {
	t.Helper()
	c := &clock{t: time.Unix(1_800_000_000, 0)}
	r := &rig{bot: &fakeBot{}, handler: &fakeHandler{anonymous: addrAnon}, clock: c}
	cfg := loop.Config{
		Now: c.now,
		Log: func(format string, args ...any) { r.logs = append(r.logs, fmt.Sprintf(format, args...)) },
	}
	for _, m := range mutate {
		m(&cfg)
	}
	r.loop = loop.New(r.bot, r.handler, cfg)
	return r
}

func (r *rig) chat(id, from, text string) {
	r.bot.events = append(r.bot.events, loop.Event{ID: id, Kind: "chat", To: botAddr, From: from, Text: text})
}

func (r *rig) letter(id, from, subject, body string) {
	r.bot.events = append(r.bot.events, loop.Event{ID: id, Kind: "letter", To: botAddr, From: from, Subject: subject, Text: body})
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
	if got.kind != "chat" || got.from != botAddr || got.to != addrA || !strings.HasPrefix(got.text, "ok /register @ben") || !strings.Contains(got.text, "\nfake: {") {
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
	if got.kind != "letter" || got.subject != "Re: hello" || got.to != addrA || got.from != botAddr || strings.Contains(got.text, "fake: {") {
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
	if len(r.handler.calls) != 2 {
		t.Errorf("the handler saw %d requests; the loop does not keep track of events", len(r.handler.calls))
	}
	if len(r.bot.done) != 1 || r.bot.done[0] != "c1" {
		t.Errorf("done = %v", r.bot.done)
	}
}

func TestASenderOverTheRateGetsOneNoticeAndThenSilence(t *testing.T) {
	r := newRig(t, func(c *loop.Config) { c.PerSender = limit.Config{Burst: 3, Every: time.Hour} })
	for i := 0; i < 10; i++ {
		r.chat(fmt.Sprintf("c%d", i), addrA, "/help")
	}
	r.drain(t)
	if len(r.bot.sent) != 4 {
		t.Fatalf("%d replies: three answers and one notice", len(r.bot.sent))
	}
	if notice := r.bot.sent[3]; !strings.Contains(notice.text, "Slow down") {
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
	if len(r.bot.sent) != 5 || !strings.Contains(r.bot.sent[4].text, "Slow down") {
		t.Errorf("%d replies after the minute", len(r.bot.sent))
	}
}

func TestOneSendersExcessDoesNotCostAnotherItsAnswers(t *testing.T) {
	r := newRig(t, func(c *loop.Config) { c.PerSender = limit.Config{Burst: 1, Every: time.Hour} })
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
	r := newRig(t, func(c *loop.Config) {
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
			if strings.Contains(s.text, "Slow down") {
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
	r := newRig(t, func(c *loop.Config) { c.ReplyBudget = limit.Config{Burst: 4, Every: time.Hour} })
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
	r.bot.events = append(r.bot.events, loop.Event{Kind: "note", Text: "the node was restarted"})
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
	r := newRig(t, func(c *loop.Config) { c.PerSender = limit.Config{Burst: 2, Every: time.Hour} })
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

// A service with a voice of its own says "slow down" in it: the loop asks the config for the notice.
func TestAnInjectedRateNoticeIsWhatTheSenderIsGiven(t *testing.T) {
	r := newRig(t, func(c *loop.Config) {
		c.PerSender = limit.Config{Burst: 1, Every: time.Hour}
		c.RateNotice = func(req loop.Request) loop.Reply {
			return loop.Reply{Text: "custom notice to " + req.From, Code: "rate_limited"}
		}
	})
	r.chat("a1", addrA, "/help")
	r.chat("a2", addrA, "/help")
	r.drain(t)
	if len(r.bot.sent) != 2 || r.bot.sent[1].text != "custom notice to "+addrA {
		t.Fatalf("sent %+v", r.bot.sent)
	}
	if strings.Contains(r.bot.sent[1].text, "Slow down") {
		t.Error("the default notice was used")
	}
}

// With no notice of its own a loop still answers a sender over its rate, in words that belong to no
// bot, to a chat and to a letter. Silence is not an option.
func TestTheDefaultRateNoticeIsNeutralAndNeverSilent(t *testing.T) {
	r := newRig(t, func(c *loop.Config) { c.PerSender = limit.Config{Burst: 1, Every: time.Hour} })
	r.chat("a1", addrA, "/help")
	r.chat("a2", addrA, "/help")
	r.letter("b1", addrB, "hello", "/help")
	r.letter("b2", addrB, "hello", "/help")
	r.letter("c1", "BM-sender-c", "", "/help")
	r.letter("c2", "BM-sender-c", "", "/help")
	r.drain(t)
	if len(r.bot.sent) != 6 {
		t.Fatalf("%d replies, want 3 answers and 3 notices: %+v", len(r.bot.sent), r.bot.sent)
	}
	trailer := regexp.MustCompile(`(?m)^[a-z][a-z0-9_]{1,31}: \{`)
	byTo := map[string]sent{}
	for _, s := range r.bot.sent {
		byTo[s.to] = s // the last reply to an address is its notice
	}
	chat, letter, bare := byTo[addrA], byTo[addrB], byTo["BM-sender-c"]
	if chat.kind != "chat" || strings.TrimSpace(chat.text) == "" || trailer.MatchString(chat.text) {
		t.Errorf("the chat notice is %+v", chat)
	}
	if letter.kind != "letter" || letter.subject != "Re: hello" || strings.TrimSpace(letter.text) == "" {
		t.Errorf("the letter notice is %+v", letter)
	}
	if bare.subject != "Re: your letter" {
		t.Errorf("a letter with no subject got the subject %q", bare.subject)
	}
}

// A letter's subject is the sender's, and a sender may put anything in it: a line break, which the library refuses (the notice
// would never be sent and the event would be done with no answer), or a megabyte. The notice's subject is one short line.
func TestTheDefaultRateNoticeToALetterHasOneShortLineForASubject(t *testing.T) {
	r := newRig(t, func(c *loop.Config) { c.PerSender = limit.Config{Burst: 1, Every: time.Hour} })
	subjects := map[string]string{
		"BM-sender-lines":     "first line\r\nsecond line",
		"BM-sender-long":      strings.Repeat("long ", 20000),
		"BM-sender-invisible": "\r\n\u2028\u200b",
		"BM-sender-control":   "tab\there \x00 nul",
	}
	for to, subject := range subjects {
		r.letter(to+"-1", to, subject, "/help")
		r.letter(to+"-2", to, subject, "/help")
	}
	r.drain(t)
	byTo := map[string]sent{}
	for _, s := range r.bot.sent {
		byTo[s.to] = s // the last reply to an address is its notice
	}
	for to := range subjects {
		notice := byTo[to]
		if notice.kind != "letter" || !strings.HasPrefix(notice.subject, "Re: ") || strings.TrimSpace(notice.text) == "" {
			t.Errorf("%s: the notice is %+v", to, notice)
			continue
		}
		for _, c := range notice.subject {
			if unicode.IsControl(c) || unicode.Is(unicode.Cf, c) || c == '\u2028' || c == '\u2029' || c == utf8.RuneError {
				t.Errorf("%s: the subject %q holds the character %U", to, notice.subject, c)
				break
			}
		}
		if n := utf8.RuneCountInString(strings.TrimPrefix(notice.subject, "Re: ")); n == 0 || n > 100 {
			t.Errorf("%s: the subject has %d characters after \"Re: \", want 1 to 100", to, n)
		}
	}
	if got := byTo["BM-sender-invisible"].subject; got != "Re: your letter" {
		t.Errorf("a subject of nothing visible got %q, want %q", got, "Re: your letter")
	}
	if got := byTo["BM-sender-lines"].subject; !strings.Contains(got, "first line") {
		t.Errorf("the words of the subject are lost: %q", got)
	}
}
