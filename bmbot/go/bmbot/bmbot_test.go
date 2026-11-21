//go:build libbmbot

package bmbot

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestChanAddressesMatchPyBitmessage(t *testing.T) {
	got, err := ChanAddress("general")
	if err != nil || got != "BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r" {
		t.Errorf("ChanAddress(general) = %q, %v", got, err)
	}
	other, _ := ChanAddress("General")
	if other == got {
		t.Error("a phrase's case is part of the chan")
	}
	_, err = ChanAddress("")
	var e *Error
	if !errors.As(err, &e) || e.Code != CodeArgument || !e.Permanent() || !strings.Contains(e.Text, "empty") {
		t.Errorf("ChanAddress(\"\") = %v", err)
	}
}

func TestABotWithNoNodeOpensAnswersAndCloses(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "bot")
	b, err := Open(Config{DataDir: dir, Phrase: "go cgo test phrase", Node: Node{Bin: ""}, StepMs: 20})
	if err != nil {
		t.Fatal(err)
	}
	defer b.Close()
	st, err := b.Status()
	if err != nil || !strings.HasPrefix(st.Address, "BM-") || st.Node.Managed {
		t.Fatalf("status = %+v, %v", st, err)
	}
	if _, ok, err := b.Next(50 * time.Millisecond); ok || err != nil {
		t.Errorf("Next on an empty inbox = %v, %v; want a timeout", ok, err)
	}
	// A bad recipient is a permanent refusal, which the loop treats as done.
	_, err = b.SendChat("", "not an address", "hi")
	var e *Error
	if !errors.As(err, &e) || !e.Permanent() {
		t.Errorf("SendChat to nobody = %v", err)
	}
	if err := b.Done("no such event"); err == nil {
		t.Error("Done of an unknown event had no error")
	}
	// The directory is held while it is open.
	if _, err := Open(Config{DataDir: dir, Phrase: "go cgo test phrase", Node: Node{Bin: ""}}); err == nil {
		t.Error("a second Open of one directory worked")
	}
	b.Close()
	b.Close() // harmless
	if _, _, err := b.Next(time.Millisecond); err == nil {
		t.Error("Next on a closed bot had no error")
	}
	if err := b.Done("x"); err == nil {
		t.Error("Done on a closed bot had no error")
	}
}

func TestABotOpensFromAKeysFileAndKeepsItsAddress(t *testing.T) {
	// Two private keys, one to a line: the address is theirs, and no phrase made it.
	keys := filepath.Join(t.TempDir(), "keys")
	text := "# a made-up pair\n" + strings.Repeat("11", 32) + "\n" + strings.Repeat("22", 32) + "\n"
	if err := os.WriteFile(keys, []byte(text), 0o600); err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(t.TempDir(), "bot")
	open := func() string {
		b, err := Open(Config{DataDir: dir, KeysFile: keys, Node: Node{Bin: ""}, StepMs: 20})
		if err != nil {
			t.Fatal(err)
		}
		defer b.Close()
		st, err := b.Status()
		if err != nil || !strings.HasPrefix(st.Address, "BM-") {
			t.Fatalf("status = %+v, %v", st, err)
		}
		return st.Address
	}
	first := open()
	if again := open(); again != first {
		t.Errorf("the address changed on the second open: %s, then %s", first, again)
	}
	// A phrase as well is a mistake, and a permanent one.
	_, err := Open(Config{DataDir: dir, KeysFile: keys, Phrase: "and a phrase", Node: Node{Bin: ""}})
	var e *Error
	if !errors.As(err, &e) || !e.Permanent() || !strings.Contains(e.Text, "keys_file") {
		t.Errorf("a phrase and keys: %v", err)
	}
}

func TestAChatToOneselfAnswersNothingButQueues(t *testing.T) {
	// The address is only known after Open; queue a chat to a real address (the bot's own is refused).
	other, err := ChanAddress("another address to write to")
	if err != nil {
		t.Fatal(err)
	}
	b, err := Open(Config{DataDir: filepath.Join(t.TempDir(), "bot"), Phrase: "go cgo test phrase two", Node: Node{Bin: ""}, StepMs: 20})
	if err != nil {
		t.Fatal(err)
	}
	defer b.Close()
	id, err := b.SendChat("", other, "hello")
	if err != nil || id == "" {
		t.Fatalf("SendChat = %q, %v", id, err)
	}
	if _, err := b.SendLetter("", other, "subject", "body"); err != nil {
		t.Errorf("SendLetter: %v", err)
	}
}
