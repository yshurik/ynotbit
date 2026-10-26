package service

import (
	"bufio"
	"flag"
	"os"
	"strings"
	"testing"
)

var update = flag.Bool("update", false, "rewrite testdata/trailers.txt")

const goldenFile = "../../testdata/trailers.txt"

// The trailers a scripted conversation gets are kept in a file, and the C++ core reads the same
// file (tests/bmclient/nickbot_test.cpp): the two sides of the protocol meet there.
func TestTrailersAreTheOnesTheCoreReads(t *testing.T) {
	f := newFixture(t)
	script := []struct{ from, text string }{
		{addrA, "/register @Ben"},
		{addrA, "/register @Ben"},
		{addrA, "/register /benbot"},
		{addrA, "/register @benny"},
		{addrB, "/register @benny"},
		{addrB, "/register ben"},
		{addrB, "/address @BENNY"},
		{addrB, "/address @nobody"},
		{addrB, "/nickname " + addrA + " " + addrC},
		{addrB, "/nickname " + addrA},
		{addrB, "/search benn"},
		{addrB, "/search none1"},
		{addrB, "/search be"},
		{addrA, "/whoami"},
		{addrC, "/whoami"},
		{addrAnon, "/register @fresh"},
		{addrA, "/unregister @benny"},
		{addrA, "/unregister @benny"},
		{addrRoom, "/register #room"},
		{addrB, "/register #room"},
		{addrB, "/help"},
		{addrB, "hello"},
	}
	var got []string
	for _, step := range script {
		r := f.chat(step.from, step.text)
		lines := strings.Split(r.Text, "\n")
		got = append(got, step.text+"\t"+lines[len(lines)-1])
	}
	if *update {
		out := "# Written by `go test ./internal/service -update`: the command a scripted conversation sent, a tab, and the\n" +
			"# trailer the bot ended its answer with. Read by the Go test that wrote it and by the C++ core's.\n" +
			strings.Join(got, "\n") + "\n"
		if err := os.WriteFile(goldenFile, []byte(out), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	file, err := os.Open(goldenFile)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	var want []string
	scan := bufio.NewScanner(file)
	for scan.Scan() {
		if line := scan.Text(); line != "" && !strings.HasPrefix(line, "#") {
			want = append(want, line)
		}
	}
	if len(want) != len(got) {
		t.Fatalf("%d lines in the file, %d in the conversation (go test -update rewrites it)", len(want), len(got))
	}
	for i := range got {
		if got[i] != want[i] {
			t.Errorf("line %d\n got  %s\n want %s", i+1, got[i], want[i])
		}
	}
}
