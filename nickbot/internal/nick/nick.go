// Package nick is the syntax of a nickname: a sigil and a name, "@ben", "/echobot", "#general".
//
// The name is a letter followed by letters, digits, "_" and "-", 3 to 32 characters long, ASCII only.
// The same rules are in the C++ core (bmc/nickbot.h), and both are tested against
// testdata/nick_cases.txt.
package nick

import (
	"errors"
	"strings"
)

// Sigil says what a nickname names.
type Sigil byte

const (
	Person Sigil = '@' // a user
	Bot    Sigil = '/' // a bot or a service
	Chan   Sigil = '#' // a chan: the name is its passphrase
)

const (
	MinName = 3
	MaxName = 32
	// MinTerm is the shortest search term, not counting its sigil.
	MinTerm = 4
)

var (
	// ErrSyntax: not a nickname, or not a search term.
	ErrSyntax = errors.New("not a valid nickname")
	// ErrTooShort: a search term of fewer than MinTerm characters.
	ErrTooShort = errors.New("search term too short")
)

// Nick is a parsed nickname.
type Nick struct {
	Sigil Sigil
	Name  string // as it was written
}

func validSigil(c byte) bool {
	return c == byte(Person) || c == byte(Bot) || c == byte(Chan)
}

func isLetter(c byte) bool {
	return c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z'
}

func isNameByte(c byte) bool {
	return isLetter(c) || c >= '0' && c <= '9' || c == '_' || c == '-'
}

// Parse reads exactly one nickname; it trims nothing.
func Parse(s string) (Nick, error) {
	if len(s) < 1+MinName || len(s) > 1+MaxName || !validSigil(s[0]) {
		return Nick{}, ErrSyntax
	}
	name := s[1:]
	if !isLetter(name[0]) {
		return Nick{}, ErrSyntax
	}
	for i := 1; i < len(name); i++ {
		if !isNameByte(name[i]) {
			return Nick{}, ErrSyntax
		}
	}
	return Nick{Sigil: Sigil(s[0]), Name: name}, nil
}

// String is the nickname as it was written.
func (n Nick) String() string { return string(n.Sigil) + n.Name }

// Key is what makes two nicknames the same one: a person and a bot are case-insensitive, a chan is
// its passphrase and keeps its case.
func (n Nick) Key() string {
	if n.Sigil == Chan {
		return string(n.Sigil) + n.Name
	}
	return string(n.Sigil) + strings.ToLower(n.Name)
}

// Lower is the name in lower case, which a search looks in.
func (n Nick) Lower() string { return strings.ToLower(n.Name) }

// Term is what a search looks for: part of a name, in lower case, and optionally one sigil.
type Term struct {
	Sigil Sigil // 0: any
	Text  string
}

// ParseTerm reads a search term: one optional sigil, then MinTerm to MaxName letters, digits, "_" or
// "-". A term with another character is ErrSyntax and is never looked up; one that is too short is
// ErrTooShort.
func ParseTerm(s string) (Term, error) {
	var term Term
	if len(s) > 0 && validSigil(s[0]) {
		term.Sigil = Sigil(s[0])
		s = s[1:]
	}
	if len(s) > MaxName {
		return Term{}, ErrSyntax
	}
	for i := 0; i < len(s); i++ {
		if !isNameByte(s[i]) {
			return Term{}, ErrSyntax
		}
	}
	if len(s) < MinTerm {
		return Term{}, ErrTooShort
	}
	term.Text = strings.ToLower(s)
	return term, nil
}
