package main

import (
	"strings"
	"testing"
)

const (
	seedBot  = "BM-87echoAkzstiHuPGSN8PH88m4WPm4attre8"
	seedNick = "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN"
	seedRoom = "BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr"
)

func TestParseSeed(t *testing.T) {
	text := "\xef\xbb\xbf# the names ynotbit's own contacts use\r\n" +
		"\r\n" +
		"#\n" +
		"/echobot " + seedBot + "\r\n" +
		"  /nickbot\t" + seedNick + "  \n" +
		"@admin\n" +
		"#general " + seedRoom + "\n"
	entries, err := parseSeed(strings.NewReader(text))
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 4 {
		t.Fatalf("%d entries: %+v", len(entries), entries)
	}
	want := []struct{ nick, address string }{{"/echobot", seedBot}, {"/nickbot", seedNick}, {"@admin", ""}, {"#general", seedRoom}}
	for i, w := range want {
		if entries[i].Nick.String() != w.nick || entries[i].Address != w.address {
			t.Errorf("entry %d = %s %q, want %s %q", i, entries[i].Nick, entries[i].Address, w.nick, w.address)
		}
	}
}

func TestACommentNeedsASpaceAfterTheHash(t *testing.T) {
	entries, err := parseSeed(strings.NewReader("# a comment\n#room " + seedRoom + "\n"))
	if err != nil || len(entries) != 1 || entries[0].Nick.String() != "#room" {
		t.Errorf("%+v, %v: #room is a chan name, not a comment", entries, err)
	}
}

func TestParseSeedRefusesWhatItCannotReadAndSaysWhere(t *testing.T) {
	for name, text := range map[string]string{
		"bad nick":        "@b " + seedBot + "\n",
		"no sigil":        "echobot " + seedBot + "\n",
		"bad address":     "/echobot BM-nope\n",
		"three fields":    "/echobot " + seedBot + " extra\n",
		"lower-case bm":   "/echobot bm-87echoAkzstiHuPGSN8PH88m4WPm4attre8\n",
		"a repeated name": "/echobot " + seedBot + "\n/EchoBot " + seedBot + "\n",
		// An address has one name, so the second line would take the first one's place.
		"two names for an address": "/echobot " + seedBot + "\n/nickbot " + seedBot + "\n",
	} {
		_, err := parseSeed(strings.NewReader(text))
		if err == nil {
			t.Errorf("%s: no error", name)
			continue
		}
		if !strings.Contains(err.Error(), "line ") {
			t.Errorf("%s: the error does not say which line: %v", name, err)
		}
	}
}

func TestAnEmptySeedIsFine(t *testing.T) {
	entries, err := parseSeed(strings.NewReader(""))
	if err != nil || len(entries) != 0 {
		t.Errorf("%+v, %v", entries, err)
	}
}
