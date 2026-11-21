# The bot trailer

A bot that answers in a Bitmessage chat can end its reply with one line for programs: the trailer. A client shows the words above
the line and keeps the JSON for whoever wants it. nickbot does this (`docs/nickbot.md`), and any other bot may.

## The rule

The last line of the chat text that has anything on it is a trailer when it is:

    <id>: <one line of JSON>

- `id` is a lower case letter followed by 1 to 31 lower case letters, digits or underscores (`[a-z][a-z0-9_]{1,31}`). It names the
  bot or the service, for example `nickbot`.
- A colon and one space follow, then a JSON object on one line.
- The object has a member `v`, a whole number of 1 or more. Every other member is the bot's own.

Only the last line counts: a line that looks like a trailer anywhere else is words. Text that does not follow the rule has no
trailer, and neither has a text longer than the largest chat. A message that is only a trailer has no words.

The words keep their lines; only the white space that ends them is taken off. The JSON is kept as the bot wrote it: a number or a
string is never read and written again (`0.00012345` and `1e-05` stay as they are).

An example, from nickbot:

    @ben is BM-2cT...
    nickbot: {"v":1,"cmd":"address","ok":true,"query":"@ben","found":true,"nick":"@ben","address":"BM-2cT..."}

## What a client does

ynotbit shows the words and not the line. A small `{ }` button in the bubble copies the JSON, written with a line for each member
and two spaces of indent per level, with every number as written. The words are Markdown with GitHub's tables, strikethrough and
bare links, so a bot may answer with a table. This holds for any bot, from any peer, and it asks for nothing but the line.

A client that does not read the trailer shows the line as text, below the words. The words are never wrong in such a client, only
plainer. Older ynotbit versions hid the line for nickbot only and showed any other bot's as text.

## What a bot does

- A reply to a **chat** is the words, then the trailer as its last line. A chat is shown as Markdown, in which a newline alone does
  not start a line, so the lines of the words end in two spaces (Markdown's line break).
- The words should stand on their own, for the clients that do not read the trailer.
- A reply to a **letter** (a client such as PyBitmessage has no chat) is plain words with no trailer, as nickbot's is.
- `v` is the bot's own protocol version (nickbot's is 1).

## The one parser

`bmc::splitBotTrailer` in `bmclient/include/bmc/bot_trailer.h` returns the words, the id and the JSON of a text, or nothing.
`bmc::indentJson` writes JSON for reading. A program in another language follows the rule above and should agree with the tests in
`tests/bmclient/bot_trailer_test.cpp`, which are the fine print.
