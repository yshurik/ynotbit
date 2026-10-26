package service

import (
	"testing"

	"github.com/yshurik/ynotbit/nickbot/internal/nick"
)

func mustNick(t *testing.T, text string) nick.Nick {
	t.Helper()
	n, err := nick.Parse(text)
	if err != nil {
		t.Fatalf("Parse(%q): %v", text, err)
	}
	return n
}
