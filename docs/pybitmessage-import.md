# Importing PyBitmessage data

**File → Import PyBitmessage data…** brings the identities, chans, address book, subscriptions and
letters of a PyBitmessage installation into the open vault and mailbox. The code is in `bmclient`
(`bmc/pybitmessage.h`: reading; `bmc/pybm_import.h`: the import) and the dialog in
`src/pybm_import_dialog.*`. PyBitmessage's files are only read.

## Where PyBitmessage keeps its data

From PyBitmessage's `src/paths.py` and `helper_startup.py`: `BITMESSAGE_HOME` if set; on macOS
`~/Library/Application Support/PyBitmessage`; on Windows `%APPDATA%\PyBitmessage`; on Linux
`$XDG_CONFIG_HOME/PyBitmessage` or `~/.config/PyBitmessage` (older versions kept it in
`~/.PyBitmessage` and move it when they start). A portable install keeps `keys.dat` beside the
program: the dialog offers "Choose another folder…" for it. `singleton.lock` is there while
PyBitmessage runs.

- `keys.dat` is a Python `SafeConfigParser` file (so `%` is written `%%`): one `[BM-…]` section per
  identity with `label`, `enabled`, `decoy`, `privsigningkey` and `privencryptionkey` (wallet import
  format), `chan = true` for a chan (label `[chan] <phrase>`) and `mailinglist`.
- `messages.dat` is SQLite (`settings.version` 11 in the current source): `inbox`, `sent`,
  `subscriptions`, `addressbook`, `blacklist`, `whitelist`, `pubkeys`, `inventory` (every network
  object, by far the biggest table), `settings`. The columns `msgid`, `sighash`, `ackdata` are
  declared BLOB but hold their bytes with TEXT storage class, so they are read as bytes, never as
  text. `inbox.received` is epoch seconds as text; `read = 0` is unread; a broadcast is `to =
  "[Broadcast subscribers]"`; a chan's post is `to = <chan address>`.

## What becomes of each row

| PyBitmessage | here |
|---|---|
| inbox row to an identity | Inbox, unread as it was, dated as it was |
| inbox row that is a chat message of this app (below) | Chats: the conversation with the sender, unread as it was, dated as it was; deleted over there: left out |
| inbox row to a chan of keys.dat or the vault | Channels, recipient = the chan |
| inbox row to `[Broadcast subscribers]` | Broadcasts, sender = recipient = the broadcaster |
| inbox / sent row in `trash` | Trash, remembering where it came from |
| sent row, status `msgsent`, `msgsentnoackexpected`, `broadcastsent`, `ackreceived` | Sent |
| sent row, any other status (queued, waiting for a key, working, failed) | Drafts: nothing is sent |
| address book, subscriptions | contacts and subscriptions not here yet (existing names win) |
| identities and chans of keys.dat | the vault, last; disabled ones come in active |

A chat message of this app is a letter with no subject whose text is the app's JSON document
(`{"app":"ynotbit","body":…,"seqno":…,"type":"chat","version":1}`, see `bmc/chat_format.h`).
PyBitmessage shows one as that document in its inbox when it shares an identity with this app. The
import tells them the way the scan does when one arrives (`parseChatMessage`, on the text cleaned of
bytes that are not UTF-8): a direct letter, no subject, a document of the app that this version can
read. It stores the text of the message in its conversation (`chat_messages`, from the sender to the
identity), not the letter. A post to a chan or a broadcast with such a text stays a post, a letter
whose sender is missing stays a letter, and so does a document of a version this one cannot read.
A chat has no folder to be deleted into, so one in PyBitmessage's trash is counted and left out.

A letter with no usable date is dated 1970 and counted. A row in a folder PyBitmessage itself never
lists is not imported. Network data, cached public keys, blacklist and whitelist are not imported.

## No duplicates

Adding identities makes the scan read the node's retained objects again (90 days by default), so
PyBitmessage's recent letters would be found a second time. A letter with an acknowledgment is
stored here under an id made of what it says, not under the hash of its object, and PyBitmessage
keeps no acknowledgment token, so the ids alone cannot tell. The mailbox therefore remembers the
network object each letter came from (`messages.object_hash`, a partial unique index): the scan
passes the object's hash, an imported inbox row carries PyBitmessage's `msgid`, and one object is
stored once whichever comes first. A direct letter is also compared by sender, recipient, subject
and body with the letters the mailbox already has (a mailbox that took the identities earlier has
its letters without an object hash). The scan is paused while an import goes, and the identities
come last. Running an import again adds only what is missing.

A chat message is held to its conversation instead: the chat table keeps one message per identity,
peer, direction and number (the sender's number for it, a millisecond clock), so one that the scan
stored under another id, one that an earlier import stored and one that the mailbox received itself
are all the same message, whichever comes first.

## Being kind to a running PyBitmessage

The file is opened read-only, rows are read in chunks by rowid, and nothing is held between two
chunks, so PyBitmessage's own writes go through. A lock it holds for a while is waited for (a minute
at most); a journal left by a killed PyBitmessage is not replayed (a read-only look does not repair
it) and the dialog says to start and close PyBitmessage once.

## Tests

`tests/bmclient/pybitmessage_test.cpp`, `mailbox_import_test.cpp`, `vault_import_test.cpp` and
`pybm_import_test.cpp` use files made the way PyBitmessage makes them
(`tests/bmclient/pybm_fixtures.h`); `testPyBitmessageImport` in `tests/widgets_tests.cpp` drives
the dialog. With `YNOTBIT_PYBM_SAMPLE` set to a folder holding a copy of a real `keys.dat` and
`messages.dat`, the real data is imported whole and once; with `YNOTBIT_PYBM_BIG_ROWS` set, a
synthetic folder of that many letters times the import and the longest step (about 6,000 letters a
second on an M-class Mac; 200,000 letters in half a minute).
