// Package loop is nickbot's heart: it takes each chat and letter the library hands out, keeps
// senders to their rates, has the service answer, sends the answer, and only then says the event
// is done.
package loop

import (
	"context"
	"errors"
	"sync/atomic"
	"time"

	"github.com/yshurik/ynotbit/nickbot/internal/limit"
	"github.com/yshurik/ynotbit/nickbot/internal/service"
)

// Event is what the library hands out (bmbot.h): a chat, a letter, or a note about a problem.
type Event struct {
	ID       string
	Kind     string // "chat", "letter" or "note"
	To       string // our address
	From     string
	Subject  string
	Text     string
	Seqno    int64
	Received int64
}

// Bot is the part of the library the loop uses.
type Bot interface {
	// Next waits up to the timeout for an event; ok is false when none came.
	Next(timeout time.Duration) (ev Event, ok bool, err error)
	Done(id string) error
	SendChat(from, to, text string) (id string, err error)
	SendLetter(from, to, subject, body string) (id string, err error)
}

// Handler is the service.
type Handler interface {
	Handle(service.Request) service.Reply
	IsAnonymous(address string) bool
}

// Config sets the limits; zero values take the defaults.
type Config struct {
	PerSender   limit.Config // commands from one address: 10 at once, then one every 6 seconds
	Anonymous   limit.Config // from the shared address, which is everybody: 60, then two a second
	Notice      limit.Config // "slow down" to one sender: once a minute
	Unknown     limit.Config // answers to what is no command, per sender: 2, then one every 10 minutes
	ReplyBudget limit.Config // all answers together: 600, then one every 6 seconds
	MaxSenders  int          // how many senders' buckets are remembered
	Timeout     time.Duration
	Now         func() time.Time
	Log         func(format string, args ...any)
}

// Stats counts what the loop has done.
type Stats struct {
	Received, Chats, Letters int64
	Answered                 int64 // replies queued
	Limited                  int64 // events refused for the sender's rate
	Notices                  int64 // "slow down" replies sent
	Dropped                  int64 // events dropped for the budget or because they were chatter
	Errors                   int64 // replies that could not be queued
}

type permanent interface{ Permanent() bool }

// Loop runs a Bot.
type Loop struct {
	bot     Bot
	h       Handler
	cfg     Config
	sender  *limit.Limiter
	anon    *limit.Limiter
	notice  *limit.Limiter
	unknown *limit.Limiter
	budget  *limit.Limiter

	received, chats, letters, answered, limited, notices, dropped, errs atomic.Int64
}

// New makes a loop.
func New(bot Bot, h Handler, cfg Config) *Loop {
	if cfg.PerSender == (limit.Config{}) {
		cfg.PerSender = limit.Config{Burst: 10, Every: 6 * time.Second}
	}
	if cfg.Anonymous == (limit.Config{}) {
		cfg.Anonymous = limit.Config{Burst: 60, Every: 500 * time.Millisecond}
	}
	if cfg.Notice == (limit.Config{}) {
		cfg.Notice = limit.Config{Burst: 1, Every: time.Minute}
	}
	if cfg.Unknown == (limit.Config{}) {
		cfg.Unknown = limit.Config{Burst: 2, Every: 10 * time.Minute}
	}
	if cfg.ReplyBudget == (limit.Config{}) {
		cfg.ReplyBudget = limit.Config{Burst: 600, Every: 6 * time.Second}
	}
	if cfg.MaxSenders == 0 {
		cfg.MaxSenders = 10000
	}
	if cfg.Timeout == 0 {
		cfg.Timeout = time.Second
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	if cfg.Log == nil {
		cfg.Log = func(string, ...any) {}
	}
	return &Loop{
		bot: bot, h: h, cfg: cfg,
		sender:  limit.New(cfg.PerSender, cfg.MaxSenders, cfg.Now),
		anon:    limit.New(cfg.Anonymous, 1, cfg.Now),
		notice:  limit.New(cfg.Notice, cfg.MaxSenders, cfg.Now),
		unknown: limit.New(cfg.Unknown, cfg.MaxSenders, cfg.Now),
		budget:  limit.New(cfg.ReplyBudget, 1, cfg.Now),
	}
}

// Stats says what has been done so far; it may be called from any goroutine.
func (l *Loop) Stats() Stats {
	return Stats{
		Received: l.received.Load(), Chats: l.chats.Load(), Letters: l.letters.Load(),
		Answered: l.answered.Load(), Limited: l.limited.Load(), Notices: l.notices.Load(),
		Dropped: l.dropped.Load(), Errors: l.errs.Load(),
	}
}

// Run handles events until the context ends (nil) or the library fails (its error).
func (l *Loop) Run(ctx context.Context) error {
	for ctx.Err() == nil {
		if _, err := l.Step(l.cfg.Timeout); err != nil {
			return err
		}
	}
	return nil
}

// Step waits up to the timeout for one event and handles it; handled says whether there was one.
func (l *Loop) Step(timeout time.Duration) (handled bool, err error) {
	ev, ok, err := l.bot.Next(timeout)
	if err != nil {
		return false, err
	}
	if !ok {
		return false, nil
	}
	if ev.Kind == "note" {
		l.cfg.Log("note: %s", ev.Text)
		return true, nil
	}
	l.handle(ev)
	return true, nil
}

func (l *Loop) handle(ev Event) {
	l.received.Add(1)
	transport := service.Chat
	if ev.Kind == "letter" {
		transport = service.Letter
		l.letters.Add(1)
	} else {
		l.chats.Add(1)
	}
	req := service.Request{EventID: ev.ID, Transport: transport, From: ev.From, Subject: ev.Subject, Text: ev.Text}
	switch {
	case ev.From == "" || ev.From == ev.To:
		l.finish(ev) // nothing to answer
		return
	case !l.allowed(ev.From):
		l.limited.Add(1)
		if l.notice.Allow(ev.From) {
			l.notices.Add(1)
			l.send(ev, service.RateLimited(req), false)
			return
		}
		l.finish(ev)
		return
	case !l.budget.Allow("*"):
		l.dropped.Add(1)
		l.finish(ev)
		return
	}
	reply := l.h.Handle(req)
	if reply.Code == "unknown_command" && !l.unknown.Allow(ev.From) {
		l.dropped.Add(1)
		l.finish(ev)
		return
	}
	l.send(ev, reply, true)
}

func (l *Loop) allowed(from string) bool {
	if l.h.IsAnonymous(from) {
		return l.anon.Allow("*")
	}
	return l.sender.Allow(from)
}

// send queues the reply and, if that worked, finishes the event. A reply that can never be sent (the
// library says the address or text is wrong) finishes it too: it would come back at every start.
func (l *Loop) send(ev Event, r service.Reply, counted bool) {
	var err error
	if ev.Kind == "letter" {
		_, err = l.bot.SendLetter(ev.To, ev.From, r.Subject, r.Text)
	} else {
		_, err = l.bot.SendChat(ev.To, ev.From, r.Text)
	}
	if err != nil {
		l.errs.Add(1)
		l.cfg.Log("cannot queue the reply to event %s: %v", ev.ID, err)
		var p permanent
		if errors.As(err, &p) && p.Permanent() {
			l.finish(ev)
		}
		return
	}
	if counted {
		l.answered.Add(1)
	}
	l.finish(ev)
}

func (l *Loop) finish(ev Event) {
	if err := l.bot.Done(ev.ID); err != nil {
		l.cfg.Log("cannot mark event %s done: %v", ev.ID, err)
	}
}
