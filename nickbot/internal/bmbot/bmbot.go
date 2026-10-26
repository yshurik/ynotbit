//go:build libbmbot

package bmbot

/*
#cgo CFLAGS: -I${SRCDIR}/../../../bmbot
#cgo LDFLAGS: -lbmbot
#include <stdlib.h>
#include "bmbot.h"
*/
import "C"

import (
	"encoding/json"
	"sync"
	"time"
	"unsafe"

	"github.com/yshurik/ynotbit/nickbot/internal/loop"
)

// Bot is an open library handle. Its methods may be used from any goroutine; Close waits for the
// calls in flight (a Next, at most its timeout) and then frees the handle.
type Bot struct {
	mu sync.RWMutex // read: a call is using h; write: Close is freeing it
	h  *C.bmbot
}

func fail(code C.int, message *C.char) error {
	text := "error"
	if message != nil {
		text = C.GoString(message)
		C.bmbot_free(unsafe.Pointer(message))
	}
	return &Error{Code: int(code), Text: text}
}

func cstr(s string) (*C.char, func()) {
	c := C.CString(s)
	return c, func() { C.free(unsafe.Pointer(c)) }
}

// take copies a string the library returned and frees it.
func take(p *C.char) string {
	if p == nil {
		return ""
	}
	s := C.GoString(p)
	C.bmbot_free(unsafe.Pointer(p))
	return s
}

// Open opens the bot: its vault and mailbox from the phrase, its node as a child process.
func Open(cfg Config) (*Bot, error) {
	js, err := json.Marshal(cfg)
	if err != nil {
		return nil, err
	}
	c, free := cstr(string(js))
	defer free()
	var h *C.bmbot
	var message *C.char
	if code := C.bmbot_open(c, &h, &message); code != C.BMBOT_OK {
		return nil, fail(code, message)
	}
	return &Bot{h: h}, nil
}

// ChanAddress is the address of the chan the phrase makes.
func ChanAddress(phrase string) (string, error) {
	c, free := cstr(phrase)
	defer free()
	var address, message *C.char
	if code := C.bmbot_chan_address(c, &address, &message); code != C.BMBOT_OK {
		return "", fail(code, message)
	}
	return take(address), nil
}

func (b *Bot) handle() (*C.bmbot, func(), error) {
	b.mu.RLock()
	if b.h == nil {
		b.mu.RUnlock()
		return nil, nil, &Error{Code: CodeState, Text: "the bot is closed"}
	}
	return b.h, b.mu.RUnlock, nil
}

type eventJSON struct {
	ID       string `json:"id"`
	Kind     string `json:"kind"`
	To       string `json:"to"`
	From     string `json:"from"`
	Subject  string `json:"subject"`
	Text     string `json:"text"`
	Seqno    int64  `json:"seqno"`
	Received int64  `json:"received"`
}

// Next waits up to the timeout for the next event.
func (b *Bot) Next(timeout time.Duration) (loop.Event, bool, error) {
	h, release, err := b.handle()
	if err != nil {
		return loop.Event{}, false, err
	}
	defer release()
	var out, message *C.char
	code := C.bmbot_next(h, C.int(timeout.Milliseconds()), &out, &message)
	switch {
	case code == C.BMBOT_TIMEOUT:
		return loop.Event{}, false, nil
	case code != C.BMBOT_OK:
		return loop.Event{}, false, fail(code, message)
	}
	var ev eventJSON
	if err := json.Unmarshal([]byte(take(out)), &ev); err != nil {
		return loop.Event{}, false, err
	}
	return loop.Event{ID: ev.ID, Kind: ev.Kind, To: ev.To, From: ev.From, Subject: ev.Subject,
		Text: ev.Text, Seqno: ev.Seqno, Received: ev.Received}, true, nil
}

// Done marks an event read.
func (b *Bot) Done(id string) error {
	h, release, err := b.handle()
	if err != nil {
		return err
	}
	defer release()
	c, free := cstr(id)
	defer free()
	var message *C.char
	if code := C.bmbot_done(h, c, &message); code != C.BMBOT_OK {
		return fail(code, message)
	}
	return nil
}

// SendChat queues a chat from one of the bot's addresses (empty: its own).
func (b *Bot) SendChat(from, to, text string) (string, error) {
	h, release, err := b.handle()
	if err != nil {
		return "", err
	}
	defer release()
	cf, ff := cstr(from)
	defer ff()
	ct, ft := cstr(to)
	defer ft()
	cx, fx := cstr(text)
	defer fx()
	var id, message *C.char
	if code := C.bmbot_send_chat(h, cf, ct, cx, &id, &message); code != C.BMBOT_OK {
		return "", fail(code, message)
	}
	return take(id), nil
}

// SendLetter queues a letter.
func (b *Bot) SendLetter(from, to, subject, body string) (string, error) {
	h, release, err := b.handle()
	if err != nil {
		return "", err
	}
	defer release()
	cf, ff := cstr(from)
	defer ff()
	ct, ft := cstr(to)
	defer ft()
	cs, fs := cstr(subject)
	defer fs()
	cb, fb := cstr(body)
	defer fb()
	var id, message *C.char
	if code := C.bmbot_send_letter(h, cf, ct, cs, cb, &id, &message); code != C.BMBOT_OK {
		return "", fail(code, message)
	}
	return take(id), nil
}

// Status is the bot's and its node's state.
func (b *Bot) Status() (Status, error) {
	h, release, err := b.handle()
	if err != nil {
		return Status{}, err
	}
	defer release()
	var out, message *C.char
	if code := C.bmbot_status(h, &out, &message); code != C.BMBOT_OK {
		return Status{}, fail(code, message)
	}
	var s Status
	err = json.Unmarshal([]byte(take(out)), &s)
	return s, err
}

// Close stops the node and frees the handle. It is safe to call twice.
func (b *Bot) Close() {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.h != nil {
		C.bmbot_close(b.h)
		b.h = nil
	}
}
