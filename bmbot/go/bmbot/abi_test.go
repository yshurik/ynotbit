//go:build libbmbot

package bmbot

import (
	"errors"
	"strconv"
	"strings"
	"testing"
)

// A program built against one bmbot.h must not run on a library of another ABI: its calls would
// reach functions that are not what it was written for.
func TestOpenRefusesALibraryOfAnotherABI(t *testing.T) {
	was := abiVersion
	t.Cleanup(func() { abiVersion = was })
	abiVersion = func() int { return headerABI + 1 }
	_, err := Open(Config{})
	var e *Error
	if !errors.As(err, &e) || e.Code != CodeABI || !e.Permanent() {
		t.Fatalf("Open = %v, want a permanent CodeABI error", err)
	}
	for _, n := range []int{headerABI, headerABI + 1} {
		if !strings.Contains(e.Text, strconv.Itoa(n)) {
			t.Errorf("the text %q does not say %d", e.Text, n)
		}
	}
}

func TestTheLinkedLibraryAndTheHeaderAgree(t *testing.T) {
	if got := abiVersion(); got != headerABI {
		t.Errorf("the library says ABI %d, the header %d", got, headerABI)
	}
}
