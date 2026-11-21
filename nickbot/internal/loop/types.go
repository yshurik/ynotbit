package loop

// Transport is how a request came, and so how its answer goes.
type Transport int

const (
	Chat   Transport = iota // a ynotbit chat: the answer has a trailer for programs
	Letter                  // a letter from any client: the answer is plain words
)

// ProtocolVersion is the "v" of the trailer.
const ProtocolVersion = 1

// Request is a chat or a letter the bot was sent.
type Request struct {
	EventID   string
	Transport Transport
	From      string // the address that sent it; the library has checked its signature
	Subject   string // letters only
	Text      string // the chat's text, or the letter's body
}

// Reply is what to send back.
type Reply struct {
	Text    string // for a chat, the words and, as the last line, the trailer; for a letter, the words
	Subject string // for a letter
	Cmd     string // the command that was understood, or "" if none
	// Code is the error code, or "" when the command was carried out. The code "unknown_command" marks
	// the answer to text that is no command: the loop gives it to a sender at most twice in ten minutes.
	Code string
}
