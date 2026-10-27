package service

import (
	"bytes"
	"encoding/json"
	"strings"
	"unicode"
	"unicode/utf8"
)

// Transport is how a request came, and so how its answer goes.
type Transport int

const (
	Chat   Transport = iota // a ynotbit chat: the answer has a trailer for programs
	Letter                  // a letter from any client: the answer is plain words
)

// ProtocolVersion is the "v" of the trailer.
const ProtocolVersion = 1

// Request is a chat or a letter the bot was sent.
type Request struct {
	EventID   string
	Transport Transport
	From      string // the address that sent it; the library has checked its signature
	Subject   string // letters only
	Text      string // the chat's text, or the letter's body
}

// Reply is what to send back.
type Reply struct {
	Text    string // for a chat, the words and, as the last line, the trailer; for a letter, the words
	Subject string // for a letter
	Cmd     string // the command that was understood, or "" if none
	Code    string // the error code, or "" when the command was carried out
}

// payload is the JSON of the trailer. The order of the fields is part of the protocol.
type payload struct {
	V        int       `json:"v"`
	Cmd      string    `json:"cmd"`
	Ok       bool      `json:"ok"`
	Error    string    `json:"error,omitempty"`
	Query    string    `json:"query,omitempty"`
	Found    *bool     `json:"found,omitempty"`
	Nick     string    `json:"nick,omitempty"`
	Address  string    `json:"address,omitempty"`
	Already  bool      `json:"already,omitempty"`
	Replaced string    `json:"replaced,omitempty"`
	Nicks    *[]string `json:"nicks,omitempty"` // a pointer: an empty list is still sent
	Results  any       `json:"results,omitempty"`
	More     *bool     `json:"more,omitempty"`
}

// nickResult is one answer of /nickname.
type nickResult struct {
	Address string   `json:"address"`
	Nicks   []string `json:"nicks"`
}

// searchResult is one answer of /search.
type searchResult struct {
	Nick    string `json:"nick"`
	Address string `json:"address"`
}

// outcome is what a command came to, before it is put into words for a transport.
type outcome struct {
	lines []string
	p     payload
	// ready is the words of an answer that were put together before (a recorded reply): when it is
	// set, lines and p only say which command it was.
	ready string
}

func boolPtr(b bool) *bool { return &b }

func trailerLine(p payload) string {
	p.V = ProtocolVersion
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(p); err != nil {
		return `nickbot: {"v":1,"cmd":"","ok":false,"error":"internal"}`
	}
	return "nickbot: " + strings.TrimSuffix(buf.String(), "\n")
}

// hardBreak ends a line of a chat's words. A chat is shown as Markdown, in which a newline alone only
// continues the paragraph, so a reply of several lines would be shown as one. Two spaces before the
// newline make it a line break; where a block ends (before a blank line or a list item) they do
// nothing.
const hardBreak = "  \n"

// reply puts an outcome into words for the way the request came. Nothing a sender wrote goes into
// it unless the command has checked it first.
func (o outcome) reply(req Request) Reply {
	r := Reply{Cmd: o.p.Cmd, Code: o.p.Error}
	switch {
	case o.ready != "":
		r.Text = o.ready
	case req.Transport == Chat:
		r.Text = strings.Join(o.lines, hardBreak) + "\n" + trailerLine(o.p)
	default:
		r.Text = strings.Join(o.lines, "\n")
	}
	if req.Transport == Letter {
		r.Subject = replySubject(req.Subject)
	}
	return r
}

// replySubject is "Re: " and the subject of the letter answered, cleaned to one short line.
func replySubject(subject string) string {
	var b strings.Builder
	count := 0
	for _, r := range subject {
		if unicode.IsControl(r) || unicode.Is(unicode.Cf, r) || r == '\u2028' || r == '\u2029' || r == utf8.RuneError {
			continue
		}
		if count >= 100 {
			break
		}
		b.WriteRune(r)
		count++
	}
	clean := strings.TrimSpace(b.String())
	if clean == "" {
		return "Re: nickbot"
	}
	return "Re: " + clean
}

// RateLimited is the one answer a sender over its rate is given, once a minute; the caller keeps the
// count.
func RateLimited(req Request) Reply {
	return failure("", codeLimited, "Slow down: you are sending too many commands. Try again in a minute.", "").reply(req)
}
