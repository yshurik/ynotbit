//go:build !libbmbot

package bmbot

import (
	"strings"
	"testing"
)

// The module is shared by every bot: a program built without the library is not told that nickbot was.
func TestTheNoLibraryErrorNamesNoBot(t *testing.T) {
	if msg := ErrNotBuilt.Error(); !strings.Contains(msg, "libbmbot") || strings.Contains(msg, "nickbot") {
		t.Errorf("ErrNotBuilt = %q", msg)
	}
}
