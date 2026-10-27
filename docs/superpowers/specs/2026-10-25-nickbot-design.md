# nickbot: nicknames for Bitmessage addresses

Status: written 2026-10-25 from the user's brief, with the user's instruction to plan and then build. Everything marked **(my call)** is a decision the brief left open; the user can overrule any of them. Where the brief had a slip I say how I read it.

## 1. Intent

A Bitmessage address is 38 characters of base58. People cannot remember it, cannot say it aloud and cannot tell two of them apart at a glance. A *nickbot* is a public service that maps short, speakable names to addresses and back, so that `@ben` can be found, trusted a little more than a bare address, and added to the address book.

Two pieces:

1. **nickbot**, a Go service in a container. It has an address and a key of its own, like the echo bot, takes commands by chat or by letter, and keeps its registry in SQLite on a volume.
2. **ynotbit support**: a list of nickbots (at most five, the first is the default, each with a colour), registration of a new identity, a search that adds people to the address book, nicknames shown wherever the app shows an address, and a lookup path that does not reveal who is asking.

## 2. Goals, non-goals, anti-goals

Goals:
- A person owns their nickname because they own the key of the address (Bitmessage signs every message, so the sender of a command is proved, not claimed).
- Looking someone up reveals nothing about the one who looks.
- The bot is cheap to run for months: bounded memory, bounded work per message, no state that cannot be rebuilt from the database.
- Other clients can use the bot too. A command is plain text, and PyBitmessage can send it in a letter.

Non-goals:
- No identity verification beyond key ownership. A nickname says "the owner of this key asked for this name first", not who the person is.
- No federation between nickbots. Each nickbot is its own registry; the client keeps them apart (colours) and never merges them.
- No payments, no expiry of live registrations, no moderation workflow beyond an operator command line.

Anti-goals (what would make this a failure even if it works):
- The bot, or anyone watching it, learning which user asked for which name.
- A nickname that silently changes owner under a client that cached it.
- A client that sends lookups the user did not ask for to a bot they have switched off.

## 3. The service

### 3.1 Nicknames

A nickname is a sigil and a name: `@ben`, `/echobot`, `#general`.

| Sigil | Kind | Who may hold it |
|---|---|---|
| `@` | a person | the owner of any address |
| `/` | a bot or service | the owner of any address |
| `#` | a chan | only the chan's own address, see 3.2 |

The name after the sigil is a letter followed by letters, digits, `_` and `-`: `[A-Za-z][A-Za-z0-9_-]*`. **(my call)** It is 3 to 32 characters long (`@ben` is the shortest the brief shows). The brief says "for now just these symbols, maybe it is enough"; ASCII only also keeps look-alike characters out. The syntax is one function in each language (`nick` in Go, `bmc::nickbot` in the C++ core) and both are tested with the same table of cases.

**(my call)** Uniqueness is case-insensitive for `@` and `/` (`@Ben` and `@ben` are one name; the spelling the owner registered is kept for display). A chan name is its passphrase, and passphrases are case-sensitive, so `#Chan` and `#chan` are two different chans and two different nicknames.

**(my call)** Reserved names: the operator's *seed file* lists nicknames that nobody can register. A line with an address (`/echobot BM-87echo...`) is a permanent registration owned by the operator; a line with no address only keeps the name free. The file that ships with the repo has `/nickbot` (its own address) and `/echobot`, so nobody can claim the two names ynotbit's own contacts use.

### 3.2 Rules

- **One nickname per sigil per address**, and one address per nickname. An address can be `@ben` and `/benbot`; it cannot be `@ben` and `@benny`.
- **Register.** `/register @ben` from address A gives A the name if it is free. If A already has an `@` name, the new one replaces it. If A already has that name, the answer is "already yours" (idempotent: a command may be answered twice, see 3.5).
- **Change and quarantine.** **(my call)** A name an address gives up (by changing or unregistering) is kept free for that address alone for 30 days. Without this, a client that cached `@ben -> A` would, a minute after A changed names, show `@ben` as someone else.
- **Unregister.** `/unregister @ben` from the owner removes it. It does nothing to the owner's other names.
- **Chans are static.** `/register #general` is accepted only when it comes from the address that the passphrase `general` makes. The bot derives that address itself (through the C library, the same code the client uses), so nobody can bind a chan name to another address. It is then permanent: it cannot be unregistered and cannot move (it could not move anyway, the address is the passphrase). The same goes for seeded names.
- **The anonymous address cannot register.** The shared lookup address of 4.1 is a chan: everybody who knows its phrase can send from it, so a name registered from it would belong to everybody. `/register` and `/unregister` from it are refused (`anonymous`).
- **An address whose key is shared is not safe to register.** Any chan address that is not `#` registered can be re-registered by every member. The bot cannot tell such addresses apart, so this is documented, not prevented.

### 3.3 Commands

Case-insensitive command word, arguments separated by spaces. By chat the whole message is the command. By letter the first non-empty line of the body is the command, or the subject if the body has none (PyBitmessage users type into the subject).

| Command | Does |
|---|---|
| `/register <nick>` | gives the sender's address that nickname |
| `/unregister <nick>` | removes it, if the sender owns it |
| `/address <nick>` | the address of a nickname |
| `/nickname <address> [<address> ...]` | the nicknames of one address, or of up to 8 **(my call: a batch is one message, one proof of work and one moment instead of eight)** |
| `/search <term>` | names containing the term; at least 4 characters |
| `/whoami` | the nicknames of the address that asks |
| `/help` | the commands, and the protocol version |

The brief describes `/address nickname` as "get the nickname for the address" and `/nickname address` as "get nickname for the address", which cannot both be right. I read them as `/address @ben` returning the address and `/nickname BM-...` returning the nickname.

`/search` is safe by construction: the term is matched against `[A-Za-z0-9_-]` after one optional leading sigil, anything else is refused before the database sees it, it is passed as a bound parameter, the match is `instr()` on the lower-cased column (no `LIKE`, so `_` is not a wildcard), and the answer is cut at 10 rows. Four characters is counted without the sigil, so `@ben` is too short and `@bena` is not.

### 3.4 Replies

A reply goes back the way the command came: a chat gets a chat, a letter gets a letter (subject `Re: ...`).

A reply to a **chat** has one or more lines of English for a person and, as its **last line**, a *trailer* for programs: `nickbot: ` followed by one line of JSON. The first line a person reads is therefore enough, and a program never parses the English. A reply to a **letter** has no trailer.

```
@ben is BM-2cT...
nickbot: {"v":1,"cmd":"address","ok":true,"query":"@ben","found":true,"nick":"@ben","address":"BM-2cT..."}
```

Fields: `v` (protocol version, 1), `cmd`, `ok` (the command was carried out; a lookup that finds nothing is `ok` with `found:false`), `error` (a code, when not `ok`), `query` (what was asked, echoed, so that a reply can be matched to its request with no token), and the result: `nick`/`address` for `address` and `register`, `results` for `nickname` (one entry per address asked: `{"address", "nicks":[...]}`), `results` and `more` for `search` (`[{"nick","address"}]`). A program ignores fields it does not know.

Error codes: `usage`, `unknown_command`, `syntax`, `too_short`, `reserved`, `taken`, `not_chan` (the sender is not the chan it named), `anonymous`, `static`, `not_yours`, `rate_limited`, `busy`, `internal`. The client turns the code into a message in the user's language; the English line is for people who chat with the bot.

### 3.5 Limits and crash safety

- **Per sender**: a token bucket for commands (burst 10, one every 6 seconds) and a separate quota for registrations (5 in 24 hours, changes included). Over the limit, the sender gets one `rate_limited` reply a minute and is then ignored.
- **The shared anonymous address** is one sender for everybody, so one person could use up its bucket for all. It gets its own, much larger, bucket (120 a minute, burst 60) and nobody can make it the reason for refusing a registration.
- **Global budgets** (replies per hour, new names per hour) protect the bot's own CPU: every reply costs a proof of work. Over budget, the bot reads and drops.
- **At least once, answered once.** The C library hands each chat or letter out until the bot says it is done, so after a crash an event can come twice. The bot writes the event's id to the database in the same transaction as the registration, and a repeated id is marked done without a second action or a second reply.
- **Nothing is kept about lookups.** The log says counts and errors, never the query, and never an address that asked anonymously (the bot only sees the shared one anyway).

### 3.6 Storage

SQLite in WAL mode on the `/data` volume (`nickbot.sqlite`), through a pure-Go driver (`modernc.org/sqlite`). **(my call)** A C driver is out: the process also loads SQLCipher through libbmbot, and two copies of SQLite's symbols in one process interpose each other.

```
nicks(nick_key PRIMARY KEY, nick, sigil, address, static, created, updated)   UNIQUE(address, sigil)
released(nick_key PRIMARY KEY, address, until)     -- the 30-day hold
reserved(nick_key PRIMARY KEY, note)               -- the seed file's names with no address
handled(event_id PRIMARY KEY, at)                  -- 3.5; pruned after 7 days
history(id, at, address, action, nick, detail)     -- who registered, changed, removed what
PRAGMA user_version = 1; every change is a numbered migration
```

### 3.7 Operation

- One container from the shared image (target `nickbot`), `/data` a volume, the secret phrase a file at `/run/secrets/phrase`, `tini` as init, healthcheck on its status file, memory 256 MB.
- `nickbot admin -db FILE stats|export|remove <nick>|reserve <nick>|history <address>` works on the database while the service runs (`docker exec`). That is the whole moderation story for now.
- A daily snapshot (`VACUUM INTO`) to `/data/backup/`, seven kept.
- Its address and node are separate from the echo bot's: another phrase, another published port.
- The address of the default nickbot is a constant in the client: the vanity address `BM-87nickbot3AwHg3ZfcuZaRpHiCH9Kdk4Z5z`, found with `tools/vanity` on 2026-10-27 (the echo bot's `BM-87echo...` was searched with the first `bm-vanity`). The service runs from the address's two keys (`--keys-file`), which the maintainer keeps outside the repository, and replacing the address is one line. Until then the repository carried the address of a development phrase.

## 4. Privacy

There are two lanes.

| Lane | Commands | Sent from | Who can link it to a person |
|---|---|---|---|
| identified | `register`, `unregister`, `whoami` | the user's own address | the bot, by design: the name is public |
| anonymous | `address`, `nickname`, `search` | the shared chan address from the phrase `anonymous` | nobody: every ynotbit user is that address |

### 4.1 The shared address

The phrase `anonymous` makes a chan (everybody who knows it has its keys). ynotbit joins it for the user when it first needs a lookup, hides it from every list of identities, and sends all lookups from it. It joins it to the *vault*, under the label `#nickbot lookups`, and knows it by that label (not by a setting of the mailbox that happened to be open: a vault has many mailboxes, and a second one would take the chan for one the user had joined by hand). **(my call)** The bot sees one sender. It answers to that address; the answer is encrypted to the chan's key, so **every ynotbit client that has the chan receives every answer**. That is the price of the shared address:

- Anyone can read the answers (they are public registry data) but cannot tell who asked.
- Every client sees every answer, so it can fill its own cache from other people's lookups for free **(my call)**: fewer questions, fewer to correlate.
- Anyone can post to the chan. So the client treats the chan as machine traffic only: whatever arrives for it is diverted to a hidden *Service* folder, processed there, and deleted. Only answers that come from a configured nickbot, whose sender is proved by its signature, are believed. Nothing else is shown to the user.
- Answers are validated again on arrival (nickname syntax, address checksum): a nickbot that goes wrong, or is taken over, cannot put markup or a bad address into the UI. They are rendered as plain text.

### 4.2 What is still observable, and what the client does about it

| Leak | Mitigation |
|---|---|
| Timing: a registration at 12:00:01 and a lookup at 12:00:05 look like one person | accepted. *Revised 2026-10-27, at the user's word, for speed: the first design queued a lookup for a random 3 to 30 seconds and kept it 2 minutes away from an identified message to the same bot; both are gone, and a lookup goes at the next tick.* Lookups still go in batches |
| A burst of lookups reads as one client | `/nickname` carries up to 8 addresses, and the messages of a check of the whole address book go ten seconds apart |
| The bot's network neighbours see where a message first appeared | out of scope: Tor (the proxy setting) covers it |
| What a user looks up | the query is in the clear to chan members; nothing links it to the user. Nothing is looked up unless the user asks (Find people, Check nicknames): the app sends no message of its own accord |
| A nickbot the user switched off | master switch: off means no message of any kind is sent |

Requests carry a short life, 1 hour, not the day a chat gets: they cost less proof of work and they cannot be replayed from the network later.

## 5. The client

### 5.1 The list of nickbots

Settings gets a page **Nickbots**: up to five rows (colour, label, address). The first row is the default. The colour is the row's position, taken from the same palette as the quote levels in a letter (blue, green, amber, purple, and a fifth, coral). Moving a row moves its colour; the page says so. A nickname seen in the UI in the first colour is the default nickbot's.

Kept in the open mailbox's settings (`nickbots`, a JSON list), like the other per-mailbox choices; a mailbox that has never set it uses the built-in default list. Rules: valid address, no duplicates, not one of the user's own, at most five. **(my call)** One switch turns every nickbot message off (`Use nickbots`).

The user's own nicknames are not on this page: they are on the Identities page (5.9).

### 5.2 Transport inside ynotbit

- A request is a **chat** from the sending address to the nickbot (an identified request from the user's identity; an anonymous one from the shared chan address). The chat machinery already does the proof of work, the key lookup, the acknowledgment and the expiry.
- A request from the shared chan address needs two small changes in the core: `Client` may send a chat *from a chan* when asked to (the chat rule "not a chan" stays for everything else), and `Delivery::scan` takes a set of **service addresses**; a message to one of them is filed in the hidden folder `Service` instead of `Channels`. Nothing counts a letter in `Service` as unread, notifies about it or lists it; the folder lists of the UI are fixed.
- The answer to an identified request is an ordinary chat in the conversation with the nickbot. The user can read it, and the app reads its trailer.
- The anonymous conversation (chan address with the nickbot) is hidden from the Chats page.

### 5.3 Lookups

`Session` owns one queue per nickbot. A lookup is a request to be sent by the scheduler, not by the click: the click only queues it, the scheduler sends it at the next tick (under a second; a random delay of 3 to 30 seconds and a separation of 2 minutes from a message that names the user stood here until 2026-10-27), and the answer, minutes later, updates the cache and tells the window. Every function that asks is asynchronous and says so in the UI ("Asked @nickbot; answers take a few minutes").

The cache is the address book: a contact's nickname is the setting `nickname:<address>` that already exists (it holds `/echobot` for the echo bot today); beside it `nickbot:<address>` says which nickbot said so (it picks the colour) and nothing more. A nickname typed or seeded by hand has no nickbot and no colour. Addresses that are not contacts (search results, other people's answers) are cached in memory only.

**Checking the address book, when the user asks.** *Revised 2026-10-26: the first design refreshed the contacts' nicknames on its own, in the background (on by default, with a switch); the user decided against anything the app does without being asked.* Contacts gets a button, **Check nicknames**: it asks the default nickbot for the nicknames of every contact, eight to a message (`/nickname`), the first at the next tick and each next one ten seconds behind the one before, so that a long address book is no burst on the shared chan or the bot's limits. While it is under way the button waits and a line under the filter says so; when the last message is answered (or has timed out) the line says that it is over. The answers fill the Nickname column as they come, as any answer does (5.6); a nickname a nickbot vouched for earlier and the nickbot no longer has is taken away. The setting `nickchecked:<address>` and the automatic switch are gone. A second press while a check is under way does nothing (the button is grey).

### 5.4 Registering

An identified request from the identity: `/register @ben`. The page's state for that identity moves from *waiting* to *registered* or *refused (taken)* when the answer's trailer arrives; the chat with the nickbot has the English line too. If no answer comes within the chat's life (24 hours) the state is *no answer*, and the button offers to try again. The identity's local label is not touched.

### 5.5 Finding people

Contacts gets **Find people…**: a dialog with the nickbot (colour swatch, default first), a field, and a table. `@ben` (a whole nickname) asks `/address`; anything else of 4 characters or more asks `/search`; a `BM-` address asks `/nickname`. Results arrive in the table; **Add** puts the address in the address book named by its nickname, and stores the nickname and its nickbot. The dialog may be closed meanwhile; the answer is cached anyway.

### 5.6 Showing nicknames

Wherever the app shows a name for an address, an address with a nickname shows it too: the nickname in its nickbot's colour, after the local name; where the local name is only the generated title of the address, the nickname replaces it. That covers the letter list (from and to), the reader's header, the chat list and chat title, and the Contacts table. A tooltip names the nickbot.

### 5.7 New identity

The dialog that makes an identity (alone, or with a new vault) has a checkbox **Register this nickname with <nickbot>**, checked, and a field with the nickname as it will be registered: the Nickname field turned into a valid name (`Quick Otter of Zeta` becomes `@quick_otter_of_zeta`), editable, checked as you type. With a new *vault* there is no mailbox yet, so the request waits in the session and goes out when the first mailbox opens.

### 5.8 Default

`bm::updates::defaultContacts()` gets the default nickbot (`/nickbot`) beside `/echobot`, and the built-in list of nickbots has it as the only entry.

### 5.9 The Identities page

*Reworked after the first review, 2026-10-26: the nicknames of the user's identities were a table on the Nickbots page, and the page of identities was a list of cards.* Now the Identities page is the address book's table (a tree view over a sorted proxy, the same toolbar, filter and empty note) with the columns **Address** (with its identicon), **Nickname** and **Local name**. What the first design put on the Nickbots page and on the cards is in one place:

- **Rows.** One per identity and chan, in the vault's order until a header is clicked. The local name carries a pill, *Default* or *Channel*. The filter matches the name, the nickname and the address.
- **Nickname cell.** The nickname that stands (said by a nickbot, or set by hand) in its nickbot's colour, with the nickbot's name as the tip. Where none stands: a try that waits is shown muted, one that was refused or went unanswered in red, with the words of the state as the tip. An own identity with nothing to show offers *Register a nickname…* in muted italics, while nicknames can be asked for at all. A chan has no nickname to ask for here **(my call: a chan's nickname is its passphrase, which the vault does not keep for a secret chan)**.
- **Toolbar.** *Nickname…* (for the selected identity), a pill of icon buttons (copy the address, QR code, set as default, rename, delete), *Join a channel…*, *New identity*. A button is on while a row is selected; *Set as default* is grey on the default, *Nickname…* on a chan. The right-click menu holds the same actions in words, and a double-click does what its cell is about: the address is copied (a tip says so), the nickname is asked for, the local name is renamed (the prompt starts from the name there is).
- **Nickname dialog.** The identity (identicon, name, address); how the last try stands (muted while it waits, in red with the reason when refused, *Registered: @name*); the nickname, offered as the last try, else the one that stands, else one made from the identity's name; the nickbot, when several are listed; the problem that is in the way before anything is typed (nickbots off, none listed); and *Register*, grey for a nickname that is not valid, is a chan's (`#`), or is the one that stands. A request the client refuses keeps the dialog open with the reason.
- **Smoothness.** The page opens on the default identity, so that its buttons work at once; a redraw keeps the selection and happens only when what the table shows has changed (nicknames come in as the session ticks, and a redraw per tick would cost the user the row); an identity that was just made, joined or imported is selected and scrolled to; the answer of a nickbot changes the cell as it arrives.

## 6. Missing pieces considered

| Piece | Decision |
|---|---|
| A name changing hands under a cached copy | 30-day quarantine; clients re-check contacts every 7 days |
| Look-alike names | ASCII only; the colour and the tooltip say which nickbot; no more |
| Squatting | per-address and global rate limits, seed file for known names; no more (the bot cannot know who is a person) |
| A flood of lookups costing the bot proof of work | per-sender and global budgets; the shared sender has its own bucket so it cannot starve registration |
| The shared address as a way to spam every client | the chan is machine traffic: diverted, processed, deleted, never shown |
| A takeover of a nickbot | answers are re-validated; plain-text rendering; only configured nickbots are believed |
| A repeated event after a crash | `handled` table in the same transaction |
| Timing correlation | accepted since 2026-10-27, at the user's word: nothing is held back; lookups go in batches |
| Latency (minutes) | every lookup is asynchronous in the UI; pending state is persisted for registration |
| A nickname as a recipient in the To field | not in this slice; `Find people` and the address book cover it. Follow-up |
| Language of the bot's answers | English; the client translates from the codes |
| Protocol evolution | `v` in the trailer, unknown fields ignored, `/help` states the version |
| A user who joined the chan `anonymous` by hand | the app does not take it over: lookups stay off, the page says why |
| A vault with several mailboxes | the lookup chan is the vault's, found by its label, so every mailbox uses the one chan; a user who then tries to join `anonymous` by hand is told "Channel already joined", which is true: it is the same chan, hidden from the lists **(my call)** |
| Backup and moderation | daily `VACUUM INTO`, `nickbot admin` |
| Opting out | master switch; nothing is asked unless the user asks |

## 7. Testing

- Go: unit tests per package with a table for the syntax, a store test for every rule of 3.2, an injection test for `/search` (quotes, `%`, `_`, `\`, NUL, very long, Unicode look-alikes, SQL), the limits with a fake clock, the reply and trailer for every command, a fake `Bot` for the main loop (at-least-once, crash between reply and done), and, when `libbmbot` is built, a test of the cgo layer.
- End to end: the Go bot over a real `bmnode`, with a second bot (the ctypes client of `tests/bmbot`) sending commands and reading the trailers. Run in the `core-no-qt` CI job.
- C++ core: the nickname syntax with the same cases as Go, the trailer parser, the Service folder, the chan sender.
- Widgets: the Nickbots page, the Find people dialog, the identity dialog's checkbox, colours, the hidden chan and conversation, the Service folder processing with a canned answer, the nickname shown in the list.
- Mutation checks on the rules that matter: ownership, quarantine, chan derivation, the anonymous refusal, the sender check on answers, the divert predicate.

## 8. Decisions that are mine

Name length 3 to 32 (the sigil is not counted); case rules; reserved names and the seed file; quarantine of 30 days; batch lookups; the `nickbot:` trailer instead of a JSON-only protocol; the 1-hour life of a request (the 2-minute pacing was dropped on 2026-10-27, at the user's word); opportunistic caching from other people's answers; the master and the contacts-refresh switches; pure-Go SQLite; the development address until a vanity one is chosen; the fifth colour (coral); the Service folder; leaving a user's own `anonymous` chan alone; a short delay for a lookup somebody waits for (dropped on 2026-10-27, at the user's word); the chat list shows a nickname as text, not coloured; a nickbot's last line is hidden in the chats; a nickname set by hand is confirmed, never replaced; contacts are looked up at most once every ten minutes and again after a week.

## 9. Follow-ups, not in this slice

The chats the app sends from its lookup chan stay in the mailbox (a few hundred bytes each, as many as the user's lookups and checks): a prune of the old ones; nicknames as recipients in the To field and the New chat dialog; a vanity address for the nickbot; deployment on tos (needs the user's go-ahead); a web page of the registry; reading the nickname into letters' signatures; a second ABI call for cheaper proof of work on lookups.
