package bmbotgo

import (
	"go/parser"
	"go/token"
	"io/fs"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// The module is shared by every bot, so it holds no bot's code and no third-party code: what it
// imports is the standard library and itself, and its go.mod requires nothing.
func TestTheModuleImportsOnlyItselfAndTheStandardLibrary(t *testing.T) {
	const self = "github.com/yshurik/ynotbit/bmbot/go"
	fset := token.NewFileSet()
	err := filepath.WalkDir(".", func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			if d.Name() == "testdata" {
				return filepath.SkipDir
			}
			return nil
		}
		if !strings.HasSuffix(path, ".go") || strings.HasSuffix(path, "_test.go") {
			return nil
		}
		f, err := parser.ParseFile(fset, path, nil, parser.ImportsOnly)
		if err != nil {
			return err
		}
		for _, imp := range f.Imports {
			p, err := strconv.Unquote(imp.Path.Value)
			if err != nil {
				return err
			}
			first, _, _ := strings.Cut(p, "/")
			standard := !strings.Contains(first, ".")
			if !standard && p != self && !strings.HasPrefix(p, self+"/") {
				t.Errorf("%s imports %s", path, p)
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	mod, err := os.ReadFile("go.mod")
	if err != nil {
		t.Fatal(err)
	}
	for _, line := range strings.Split(string(mod), "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "require") {
			t.Errorf("go.mod requires something: %q", line)
		}
	}
}
