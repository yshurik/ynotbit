// Package service is nickbot's commands: it reads a chat or a letter, does what it says to the
// registry, and writes the answer. It knows nothing of Bitmessage but the shape of an address and
// the one library call that turns a chan's passphrase into its address.
package service

import (
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/yshurik/ynotbit/nickbot/internal/bmaddr"
	"github.com/yshurik/ynotbit/nickbot/internal/nick"
	"github.com/yshurik/ynotbit/nickbot/internal/store"
)

const (
	searchLimit     = 10
	maxNicknameArgs = 8
)

// Options configure the service; zero values take the defaults.
type Options struct {
	// Anonymous is the address everybody's lookups come from. It may look things up and nothing else.
	Anonymous string
	// ChanAddress is the address a chan's passphrase makes (the bmbot library's).
	ChanAddress func(passphrase string) (string, error)
	Now         func() time.Time
	// ChangesPerDay is how many names an address may register or change in 24 hours (5).
	ChangesPerDay int
	// NewPerHour is how many registrations and changes the whole registry takes in an hour (200).
	NewPerHour int
	// MaxNicks is how many names the registry holds (1,000,000).
	MaxNicks int
	Log      func(format string, args ...any)
}

// Service answers requests.
type Service struct {
	store *store.Store
	o     Options
}

// New makes the service over the registry.
func New(st *store.Store, o Options) *Service {
	if o.Now == nil {
		o.Now = time.Now
	}
	if o.ChangesPerDay == 0 {
		o.ChangesPerDay = 5
	}
	if o.NewPerHour == 0 {
		o.NewPerHour = 200
	}
	if o.MaxNicks == 0 {
		o.MaxNicks = 1_000_000
	}
	if o.Log == nil {
		o.Log = func(string, ...any) {}
	}
	return &Service{store: st, o: o}
}

// IsAnonymous says whether the address is the shared one.
func (s *Service) IsAnonymous(address string) bool {
	return s.o.Anonymous != "" && address == s.o.Anonymous
}

// The error codes of the protocol (spec 3.4).
const (
	codeUsage    = "usage"
	codeUnknown  = "unknown_command"
	codeSyntax   = "syntax"
	codeTooShort = "too_short"
	codeReserved = "reserved"
	codeTaken    = "taken"
	codeNotChan  = "not_chan"
	codeAnon     = "anonymous"
	codeStatic   = "static"
	codeNotYours = "not_yours"
	codeNotFound = "not_found"
	codeLimited  = "rate_limited"
	codeBusy     = "busy"
	codeInternal = "internal"
)

var (
	errLimited = errors.New("too many changes")
	errBusy    = errors.New("the registry is busy")
)

// Handle answers one request. It never fails: whatever goes wrong becomes an error reply.
func (s *Service) Handle(req Request) (reply Reply) {
	defer func() {
		if r := recover(); r != nil {
			s.o.Log("panic handling an event: %v", r)
			reply = failure("", codeInternal, "Something went wrong. Try again later.", "").reply(req)
		}
	}()
	cmd, ok := commandOf(req)
	switch {
	case !ok || (!cmd.long && !knownCommand(cmd.word)):
		return failure("", codeUnknown, "I do not know that. Send /help to see what I can do.", "").reply(req)
	case cmd.long || len(cmd.args) > maxArgs:
		return failure(cmd.word, codeUsage, "That is too long. Send /help.", "").reply(req)
	}
	c := &call{req: req, args: cmd.args, anon: s.IsAnonymous(req.From)}
	var o outcome
	switch cmd.word {
	case "help":
		o = s.help()
	case "register":
		o = s.register(c)
	case "unregister":
		o = s.unregister(c)
	case "address":
		o = s.address(c)
	case "nickname":
		o = s.nickname(c)
	case "search":
		o = s.search(c)
	case "whoami":
		o = s.whoami(c)
	}
	return o.reply(req)
}

func knownCommand(word string) bool {
	switch word {
	case "help", "register", "unregister", "address", "nickname", "search", "whoami":
		return true
	}
	return false
}

type call struct {
	req  Request
	args []string
	anon bool
}

// failure is an error outcome. query is only ever something the command has checked.
func failure(cmd, code, words, query string) outcome {
	return outcome{lines: []string{words}, p: payload{Cmd: cmd, Error: code, Query: query}}
}

func (s *Service) internal(cmd string, err error) outcome {
	s.o.Log("%s: %v", cmd, err)
	return failure(cmd, codeInternal, "Something went wrong. Try again later.", "")
}

func usage(cmd, line string) outcome {
	return failure(cmd, codeUsage, "Usage: "+line, "")
}

const syntaxHelp = "That is not a valid nickname. A nickname is @, / or # followed by a name of 3 to " +
	"32 characters: a letter, then letters, digits, _ or -."

// help is a list: it reads as one in a letter's plain text and is one in a chat's Markdown, where
// padding to line up columns would only show as uneven gaps.
func (s *Service) help() outcome {
	return outcome{
		lines: []string{
			"This is nickbot (protocol 1): nicknames for Bitmessage addresses.",
			"",
			"- /register @nick: give your address a nickname (@person, /bot, #chan)",
			"- /unregister @nick: give it up",
			"- /address @nick: the address of a nickname",
			"- /nickname BM-...: the nicknames of an address (up to 8 addresses)",
			"- /search term: names that contain the term (4 characters or more)",
			"- /whoami: your own nicknames",
		},
		p: payload{Cmd: "help", Ok: true},
	}
}

func (s *Service) register(c *call) outcome {
	const cmd = "register"
	if len(c.args) != 1 {
		return usage(cmd, "/register @nick")
	}
	n, err := nick.Parse(c.args[0])
	if err != nil {
		return failure(cmd, codeSyntax, syntaxHelp, "")
	}
	query := n.String()
	if c.anon {
		return failure(cmd, codeAnon, "Register from your own address. This one is shared by everybody.", query)
	}
	from := c.req.From
	if n.Sigil == nick.Chan {
		// A chan's name is its passphrase, so the one who may have it is the address that passphrase
		// makes. Nobody else can say that address, and nobody can bind the name to another.
		derived, err := s.o.ChanAddress(n.Name)
		if err != nil {
			return s.internal(cmd, err)
		}
		if derived != from {
			return failure(cmd, codeNotChan, fmt.Sprintf("Only the chan itself can register %s: send this from the chan's address.", query), query)
		}
	}
	now := s.o.Now()
	text, _, err := s.store.Do(c.req.EventID, func(r *store.Reg) (string, error) {
		existing, found, err := r.ByNick(n)
		if err != nil {
			return "", err
		}
		if !found || existing.Address != from {
			// Saying what is already so costs nothing; anything else counts.
			if changes, err := r.CountChanges(from, now.Add(-24*time.Hour)); err != nil {
				return "", err
			} else if changes >= s.o.ChangesPerDay {
				return "", errLimited
			}
			if all, err := r.CountAllChanges(now.Add(-time.Hour)); err != nil {
				return "", err
			} else if all >= s.o.NewPerHour {
				return "", errBusy
			}
			if grows, err := growsRegistry(r, from, n.Sigil); err != nil {
				return "", err
			} else if grows {
				if total, err := r.Total(); err != nil {
					return "", err
				} else if total >= s.o.MaxNicks {
					return "", errBusy
				}
			}
		}
		res, err := r.Register(n, from, store.RegisterOptions{Static: n.Sigil == nick.Chan})
		if err != nil {
			return "", err
		}
		o := outcome{p: payload{Cmd: cmd, Ok: true, Query: query, Nick: res.Entry.Nick, Address: from}}
		switch {
		case res.Already:
			o.lines = []string{fmt.Sprintf("%s is already yours.", res.Entry.Nick)}
			o.p.Already = true
		case res.Replaced != "":
			o.lines = []string{fmt.Sprintf("Registered %s for %s (it replaces %s).", res.Entry.Nick, from, res.Replaced)}
			o.p.Replaced = res.Replaced
		default:
			o.lines = []string{fmt.Sprintf("Registered %s for %s.", res.Entry.Nick, from)}
		}
		return o.reply(c.req).Text, nil
	})
	if err != nil {
		return s.refusal(cmd, query, err)
	}
	return replied(cmd, text)
}

// growsRegistry says whether registering a name of this sigil adds a name, rather than taking the
// place of the address's own.
func growsRegistry(r *store.Reg, address string, sigil nick.Sigil) (bool, error) {
	mine, err := r.ByAddress(address)
	if err != nil {
		return false, err
	}
	for _, e := range mine {
		if strings.HasPrefix(e.Key, string(sigil)) {
			return false, nil
		}
	}
	return true, nil
}

// replied wraps text that was already put into words (a stored reply), so that Handle can return it.
func replied(cmd, text string) outcome {
	return outcome{p: payload{Cmd: cmd, Ok: true}, ready: text}
}

// refusal turns an error of the registry into the answer to the sender.
func (s *Service) refusal(cmd, query string, err error) outcome {
	switch {
	case errors.Is(err, store.ErrTaken):
		return failure(cmd, codeTaken, fmt.Sprintf("%s is taken.", query), query)
	case errors.Is(err, store.ErrReserved):
		return failure(cmd, codeReserved, fmt.Sprintf("%s is reserved.", query), query)
	case errors.Is(err, store.ErrStatic):
		return failure(cmd, codeStatic, fmt.Sprintf("%s is permanent and cannot change.", query), query)
	case errors.Is(err, store.ErrNotYours):
		return failure(cmd, codeNotYours, fmt.Sprintf("%s belongs to another address.", query), query)
	case errors.Is(err, store.ErrNotFound):
		return failure(cmd, codeNotFound, fmt.Sprintf("%s is not registered.", query), query)
	case errors.Is(err, errLimited):
		return failure(cmd, codeLimited, "You have changed your names too often today. Try again tomorrow.", query)
	case errors.Is(err, errBusy):
		return failure(cmd, codeBusy, "The registry is busy. Try again later.", query)
	}
	return s.internal(cmd, err)
}

func (s *Service) unregister(c *call) outcome {
	const cmd = "unregister"
	if len(c.args) != 1 {
		return usage(cmd, "/unregister @nick")
	}
	n, err := nick.Parse(c.args[0])
	if err != nil {
		return failure(cmd, codeSyntax, syntaxHelp, "")
	}
	query := n.String()
	if c.anon {
		return failure(cmd, codeAnon, "Unregister from your own address. This one is shared by everybody.", query)
	}
	text, _, err := s.store.Do(c.req.EventID, func(r *store.Reg) (string, error) {
		e, found, err := r.ByNick(n)
		if err != nil {
			return "", err
		}
		if err := r.Unregister(n, c.req.From); err != nil {
			return "", err
		}
		name := query
		if found {
			name = e.Nick
		}
		o := outcome{lines: []string{fmt.Sprintf("Removed %s.", name)}, p: payload{Cmd: cmd, Ok: true, Query: query, Nick: name}}
		return o.reply(c.req).Text, nil
	})
	if err != nil {
		return s.refusal(cmd, query, err)
	}
	return replied(cmd, text)
}

func (s *Service) address(c *call) outcome {
	const cmd = "address"
	if len(c.args) != 1 {
		return usage(cmd, "/address @nick")
	}
	n, err := nick.Parse(c.args[0])
	if err != nil {
		return failure(cmd, codeSyntax, syntaxHelp, "")
	}
	query := n.String()
	e, found, err := s.store.Reg().ByNick(n)
	if err != nil {
		return s.internal(cmd, err)
	}
	if !found {
		return outcome{lines: []string{query + " is not registered."}, p: payload{Cmd: cmd, Ok: true, Query: query, Found: boolPtr(false)}}
	}
	return outcome{
		lines: []string{fmt.Sprintf("%s is %s.", e.Nick, e.Address)},
		p:     payload{Cmd: cmd, Ok: true, Query: query, Found: boolPtr(true), Nick: e.Nick, Address: e.Address},
	}
}

func (s *Service) nickname(c *call) outcome {
	const cmd = "nickname"
	if len(c.args) == 0 {
		return usage(cmd, "/nickname BM-address [BM-address ...]")
	}
	var asked []string
	seen := map[string]bool{}
	for _, a := range c.args {
		if !bmaddr.Valid(a) {
			return failure(cmd, codeSyntax, "That is not a Bitmessage address.", "")
		}
		if !seen[a] {
			seen[a] = true
			asked = append(asked, a)
		}
	}
	if len(asked) > maxNicknameArgs {
		return usage(cmd, "/nickname with at most 8 addresses")
	}
	reg := s.store.Reg()
	o := outcome{p: payload{Cmd: cmd, Ok: true}}
	results := make([]nickResult, 0, len(asked))
	for _, a := range asked {
		list, err := reg.ByAddress(a)
		if err != nil {
			return s.internal(cmd, err)
		}
		names := make([]string, 0, len(list))
		for _, e := range list {
			names = append(names, e.Nick)
		}
		results = append(results, nickResult{Address: a, Nicks: names})
		if len(names) == 0 {
			o.lines = append(o.lines, a+" has no nickname.")
		} else {
			o.lines = append(o.lines, fmt.Sprintf("%s is %s.", a, strings.Join(names, " ")))
		}
	}
	if len(asked) == 1 {
		o.p.Query = asked[0]
	}
	o.p.Results = results
	return o
}

func (s *Service) search(c *call) outcome {
	const cmd = "search"
	if len(c.args) != 1 {
		return usage(cmd, "/search term")
	}
	term, err := nick.ParseTerm(c.args[0])
	switch {
	case errors.Is(err, nick.ErrTooShort):
		return failure(cmd, codeTooShort, fmt.Sprintf("A search needs at least %d letters, digits, _ or -.", nick.MinTerm), "")
	case err != nil:
		return failure(cmd, codeSyntax, "A search term is letters, digits, _ or - only.", "")
	}
	query := term.Text
	if term.Sigil != 0 {
		query = string(term.Sigil) + term.Text
	}
	list, more, err := s.store.Reg().Search(term, searchLimit)
	if err != nil {
		return s.internal(cmd, err)
	}
	results := make([]searchResult, 0, len(list))
	o := outcome{p: payload{Cmd: cmd, Ok: true, Query: query, More: boolPtr(more)}}
	for _, e := range list {
		results = append(results, searchResult{Nick: e.Nick, Address: e.Address})
	}
	o.p.Results = results
	switch {
	case len(results) == 0:
		o.lines = []string{fmt.Sprintf("No names for %q.", query)}
	default:
		noun := "names"
		if len(results) == 1 {
			noun = "name"
		}
		head := fmt.Sprintf("%d %s for %q", len(results), noun, query)
		if more {
			head += " (there are more)"
		}
		o.lines = []string{head + ":"}
		for _, r := range results {
			o.lines = append(o.lines, r.Nick+" "+r.Address)
		}
	}
	return o
}

func (s *Service) whoami(c *call) outcome {
	const cmd = "whoami"
	if len(c.args) != 0 {
		return usage(cmd, "/whoami")
	}
	if c.anon {
		return failure(cmd, codeAnon, "This address is shared by everybody; ask from your own.", "")
	}
	list, err := s.store.Reg().ByAddress(c.req.From)
	if err != nil {
		return s.internal(cmd, err)
	}
	names := make([]string, 0, len(list))
	for _, e := range list {
		names = append(names, e.Nick)
	}
	o := outcome{p: payload{Cmd: cmd, Ok: true, Address: c.req.From, Nicks: &names}}
	if len(names) == 0 {
		o.lines = []string{"You have no nickname."}
	} else {
		o.lines = []string{"You are " + strings.Join(names, " ") + "."}
	}
	return o
}
