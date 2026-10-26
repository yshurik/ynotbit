package main

import (
	"bufio"
	"fmt"
	"io"
	"strings"

	"github.com/yshurik/ynotbit/nickbot/internal/bmaddr"
	"github.com/yshurik/ynotbit/nickbot/internal/nick"
	"github.com/yshurik/ynotbit/nickbot/internal/store"
)

// parseSeed reads the operator's seed file: one name a line, with an address to make it that
// address's permanent name or without one to keep it free. A line that is empty, or begins with
// "#" and a space (or is only "#"), is a comment: "#room" is a chan's name.
func parseSeed(r io.Reader) ([]store.SeedEntry, error) {
	var entries []store.SeedEntry
	seen := map[string]int{}
	scan := bufio.NewScanner(r)
	for number := 1; scan.Scan(); number++ {
		line := strings.TrimSpace(scan.Text())
		if number == 1 {
			line = strings.TrimSpace(strings.TrimPrefix(line, "\xef\xbb\xbf"))
		}
		if line == "" || line == "#" || strings.HasPrefix(line, "# ") || strings.HasPrefix(line, "#\t") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) > 2 {
			return nil, fmt.Errorf("line %d: expected a nickname and optionally an address", number)
		}
		n, err := nick.Parse(fields[0])
		if err != nil {
			return nil, fmt.Errorf("line %d: %q is not a nickname", number, fields[0])
		}
		entry := store.SeedEntry{Nick: n}
		if len(fields) == 2 {
			if !bmaddr.Valid(fields[1]) {
				return nil, fmt.Errorf("line %d: %q is not a Bitmessage address", number, fields[1])
			}
			entry.Address = fields[1]
		}
		if first, dup := seen[n.Key()]; dup {
			return nil, fmt.Errorf("line %d: %s is already on line %d", number, n, first)
		}
		seen[n.Key()] = number
		entries = append(entries, entry)
	}
	return entries, scan.Err()
}
