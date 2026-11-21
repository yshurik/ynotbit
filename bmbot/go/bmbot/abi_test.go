//go:build libbmbot

package bmbot

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// This module is written for one ABI of libbmbot (wrapperABI). It is pinned apart from the SDK image a bot builds on, so a
// program must not run on a library of another one: its calls would reach functions that are not what it was written for.
func TestOpenRefusesALibraryOfAnotherABI(t *testing.T) {
	was := abiVersion
	t.Cleanup(func() { abiVersion = was })
	abiVersion = func() int { return wrapperABI + 1 }
	_, err := Open(Config{})
	var e *Error
	if !errors.As(err, &e) || e.Code != CodeABI || !e.Permanent() {
		t.Fatalf("Open = %v, want a permanent CodeABI error", err)
	}
	for _, n := range []int{wrapperABI, wrapperABI + 1} {
		if !strings.Contains(e.Text, strconv.Itoa(n)) {
			t.Errorf("the text %q does not say %d", e.Text, n)
		}
	}
}

func TestChanAddressRefusesALibraryOfAnotherABI(t *testing.T) {
	was := abiVersion
	t.Cleanup(func() { abiVersion = was })
	abiVersion = func() int { return wrapperABI + 1 }
	address, err := ChanAddress("a phrase")
	var e *Error
	if !errors.As(err, &e) || e.Code != CodeABI || !e.Permanent() || address != "" {
		t.Fatalf("ChanAddress = %q, %v, want no address and a permanent CodeABI error", address, err)
	}
}

func TestTheLinkedLibraryIsOfTheABIThisWrapperIsWrittenFor(t *testing.T) {
	if got := abiVersion(); got != wrapperABI {
		t.Errorf("the library says ABI %d, this wrapper is written for %d", got, wrapperABI)
	}
}

// The bot's go.mod pins this module and its Dockerfile pins the SDK image, and the two can drift: a bmbot.h and a library that
// agree with each other, of an ABI this wrapper was not written for, must stop the build, not produce a program that runs and
// drops the events it cannot read.
func TestABuildAgainstAHeaderOfAnotherABIFails(t *testing.T) {
	header, err := os.ReadFile(filepath.Join("..", "..", "bmbot.h"))
	if err != nil {
		t.Skipf("the header of the repository is not here: %v", err)
	}
	define := []byte("#define BMBOT_ABI_VERSION " + strconv.Itoa(wrapperABI))
	if !bytes.Contains(header, define) {
		t.Fatalf("bmbot.h has no line %q to change", define)
	}
	other := bytes.Replace(header, define, []byte(fmt.Sprintf("#define BMBOT_ABI_VERSION %d", wrapperABI+1)), 1)
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "bmbot.h"), other, 0o644); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command("go", "build", "-tags", "libbmbot", ".")
	cmd.Env = append(os.Environ(), "CGO_CFLAGS=-I"+dir)
	out, err := cmd.CombinedOutput()
	if err == nil {
		t.Fatalf("the package built against a header of ABI %d; it must refuse", wrapperABI+1)
	}
	if !strings.Contains(string(out), "BMBOT_GO_ABI") {
		t.Errorf("the build failed, but not with the ABI message:\n%s", out)
	}
}
