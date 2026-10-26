package nick

import (
	"bufio"
	"errors"
	"os"
	"strings"
	"testing"
)

// The cases file is shared with the C++ core: both implementations must agree on every line.
func TestSharedCases(t *testing.T) {
	f, err := os.Open("../../testdata/nick_cases.txt")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	scan := bufio.NewScanner(f)
	count := 0
	for scan.Scan() {
		line := scan.Text()
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Split(line, "\t")
		if len(fields) < 2 {
			t.Fatalf("bad case line %q", line)
		}
		kind, input := fields[0], fields[1]
		n, err := Parse(input)
		switch kind {
		case "ok":
			if len(fields) != 3 {
				t.Fatalf("ok case without a key: %q", line)
			}
			if err != nil {
				t.Errorf("Parse(%q): %v, want ok", input, err)
				continue
			}
			if n.Key() != fields[2] {
				t.Errorf("Parse(%q).Key() = %q, want %q", input, n.Key(), fields[2])
			}
			if n.String() != input {
				t.Errorf("Parse(%q).String() = %q, want the input back", input, n.String())
			}
		case "no":
			if err == nil {
				t.Errorf("Parse(%q) accepted it as %q", input, n.Key())
			} else if !errors.Is(err, ErrSyntax) {
				t.Errorf("Parse(%q): %v, want ErrSyntax", input, err)
			}
		default:
			t.Fatalf("unknown kind in %q", line)
		}
		count++
	}
	if err := scan.Err(); err != nil {
		t.Fatal(err)
	}
	if count < 40 {
		t.Fatalf("only %d cases were read", count)
	}
}

func TestNameLengthBounds(t *testing.T) {
	for _, tc := range []struct {
		nick string
		ok   bool
	}{
		{"@ab", false},
		{"@abc", true},
		{"@" + strings.Repeat("a", 32), true},
		{"@" + strings.Repeat("a", 33), false},
		{"#" + strings.Repeat("A", 32), true},
		{"#" + strings.Repeat("A", 33), false},
	} {
		_, err := Parse(tc.nick)
		if (err == nil) != tc.ok {
			t.Errorf("Parse(%q): %v, want ok=%v", tc.nick, err, tc.ok)
		}
	}
}

func TestControlAndSpaceCharactersAreRefused(t *testing.T) {
	for _, bad := range []string{"@ben\t", "@be\tn", "@ben\n", "@ben\r", "@ben\x00", "@be\x00n", "@ben\u00a0", "\u00a0@ben", "@ben\x7f"} {
		if _, err := Parse(bad); !errors.Is(err, ErrSyntax) {
			t.Errorf("Parse(%q) = %v, want ErrSyntax", bad, err)
		}
	}
}

func TestKeyFoldsCaseOnlyForPeopleAndBots(t *testing.T) {
	a, _ := Parse("@Ben")
	b, _ := Parse("@bEN")
	if a.Key() != b.Key() {
		t.Errorf("@Ben and @bEN are one person: %q vs %q", a.Key(), b.Key())
	}
	c, _ := Parse("/Ben")
	if a.Key() == c.Key() {
		t.Errorf("a person and a bot of one name are two names: %q", a.Key())
	}
	d, _ := Parse("#Ben")
	e, _ := Parse("#ben")
	if d.Key() == e.Key() {
		t.Errorf("two chans that differ in case are two chans: %q", d.Key())
	}
}

func TestSigilsAndKinds(t *testing.T) {
	for sigil, want := range map[string]Sigil{"@": Person, "/": Bot, "#": Chan} {
		n, err := Parse(sigil + "name")
		if err != nil || n.Sigil != want || n.Name != "name" {
			t.Errorf("Parse(%qname) = %+v, %v", sigil, n, err)
		}
	}
}

func TestParseTerm(t *testing.T) {
	for _, tc := range []struct {
		in    string
		sigil Sigil
		text  string
		err   error
	}{
		{"bena", 0, "bena", nil},
		{"BENA", 0, "bena", nil},
		{"@bena", Person, "bena", nil},
		{"/bena", Bot, "bena", nil},
		{"#Bena", Chan, "bena", nil},
		{"1234", 0, "1234", nil},
		{"_-_-", 0, "_-_-", nil},
		{"ben", 0, "", ErrTooShort},
		{"@ben", 0, "", ErrTooShort},
		{"", 0, "", ErrTooShort},
		{"@", 0, "", ErrTooShort},
		{"abc%", 0, "", ErrSyntax},
		{"a_bc%", 0, "", ErrSyntax},
		{"ab cd", 0, "", ErrSyntax},
		{"'; DROP TABLE nicks;--", 0, "", ErrSyntax},
		{"\\", 0, "", ErrSyntax},
		{"@@bena", 0, "", ErrSyntax},
		{strings.Repeat("a", 32), 0, strings.Repeat("a", 32), nil},
		{strings.Repeat("a", 33), 0, "", ErrSyntax},
		{"béna", 0, "", ErrSyntax},
	} {
		term, err := ParseTerm(tc.in)
		if tc.err != nil {
			if !errors.Is(err, tc.err) {
				t.Errorf("ParseTerm(%q) = %+v, %v, want %v", tc.in, term, err, tc.err)
			}
			continue
		}
		if err != nil || term.Sigil != tc.sigil || term.Text != tc.text {
			t.Errorf("ParseTerm(%q) = %+v, %v, want sigil %q text %q", tc.in, term, err, tc.sigil, tc.text)
		}
	}
}

func TestTooShortIsNotSyntax(t *testing.T) {
	if _, err := ParseTerm("abc"); errors.Is(err, ErrSyntax) {
		t.Error("a short term is ErrTooShort, not ErrSyntax: the client says different things")
	}
}
