package service

import "strings"

const (
	maxLine = 1024 // a command longer than this is refused unread
	maxArgs = 16
)

// A command found in a request.
type command struct {
	word string   // lower case, without the slash
	args []string // split on white space
	long bool     // the line was longer than maxLine; nothing else was read
}

// commandOf finds the command a request holds. A chat is one command: its first line that has
// anything on it. A letter's command is the first line of its body, or, when that is not a command,
// its subject (PyBitmessage users type into the subject). ok is false when there is none.
func commandOf(req Request) (cmd command, ok bool) {
	candidates := []string{firstContentLine(req.Text)}
	if req.Transport == Letter {
		candidates = append(candidates, strings.TrimSpace(req.Subject))
	}
	for _, line := range candidates {
		if !strings.HasPrefix(line, "/") {
			continue
		}
		if len(line) > maxLine {
			return command{long: true}, true
		}
		fields := strings.Fields(line[1:])
		if len(fields) == 0 {
			continue
		}
		return command{word: strings.ToLower(fields[0]), args: fields[1:]}, true
	}
	return command{}, false
}

// firstContentLine is the first line of the text that has something on it, trimmed.
func firstContentLine(text string) string {
	for len(text) > 0 {
		line := text
		if i := strings.IndexByte(text, '\n'); i >= 0 {
			line, text = text[:i], text[i+1:]
		} else {
			text = ""
		}
		if line = strings.TrimSpace(line); line != "" {
			return line
		}
	}
	return ""
}
