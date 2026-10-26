//go:build !libbmbot

package bmbot

import (
	"time"

	"github.com/yshurik/ynotbit/nickbot/internal/loop"
)

// Bot is not available in this build.
type Bot struct{}

// Open says that the library is not linked in.
func Open(Config) (*Bot, error) { return nil, ErrNotBuilt }

// ChanAddress says that the library is not linked in.
func ChanAddress(string) (string, error) { return "", ErrNotBuilt }

func (*Bot) Next(time.Duration) (loop.Event, bool, error)   { return loop.Event{}, false, ErrNotBuilt }
func (*Bot) Done(string) error                              { return ErrNotBuilt }
func (*Bot) SendChat(from, to, text string) (string, error) { return "", ErrNotBuilt }
func (*Bot) SendLetter(from, to, subject, body string) (string, error) {
	return "", ErrNotBuilt
}
func (*Bot) Status() (Status, error) { return Status{}, ErrNotBuilt }
func (*Bot) Close()                  {}
