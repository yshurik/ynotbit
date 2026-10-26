# nickbot

nickbot maps nicknames to Bitmessage addresses and back. A nickname is short and can be said aloud: `@ben`. The service is a
Go program on `libbmbot` ([`nickbot/`](../nickbot/README.md)); ynotbit talks to it for the Nickbots page in Settings, for
*Find people* in Contacts, and for the nicknames it shows beside addresses. Any other client can use it too, with a chat or a
letter. The design and the reasons are in [the spec](superpowers/specs/2026-10-25-nickbot-design.md).

## Nicknames

A nickname is a sigil and a name.

| Sigil | Names | Who may hold it |
|---|---|---|
| `@` | a person | the owner of any address |
| `/` | a bot or a service | the owner of any address |
| `#` | a chan | only the chan's own address; permanent |

The name is 3 to 32 characters, ASCII only: a letter, then letters, digits, `_` and `-`. `@Ben` and `@ben` are one
name for a person or a bot (the spelling registered is kept for show); `#Chan` and `#chan` are two chans, since a chan's name is
its passphrase.

- An address has at most one nickname of each sigil. Registering another of the same sigil replaces the first.
- A name an address gives up, by changing or by `/unregister`, is kept free for that address for 30 days.
- A chan registers its name from its own address: the bot makes the address from the passphrase `#name` and the command must come
  from it. The name is then permanent.
- The operator's *seed file* keeps names free or gives them to an address for good (`nickbot/seed.example.txt`).

## Commands

By chat the message is the command. By letter the first line of the body is the command, or the subject if that is not one.
Commands are case-insensitive.

| Command | Does |
|---|---|
| `/register @nick` | gives the sender's address the nickname |
| `/unregister @nick` | takes it away, if it is the sender's |
| `/address @nick` | the address of a nickname |
| `/nickname BM-... [BM-... ...]` | the nicknames of up to 8 addresses |
| `/search term` | names that contain the term: 4 or more letters, digits, `_` or `-`, with an optional sigil in front that narrows it; at most 10 are returned |
| `/whoami` | the sender's own nicknames |
| `/help` | the commands |

Registering and unregistering must come from the address that owns the name. Lookups (`address`, `nickname`, `search`) come from
anywhere, and ynotbit sends them from a shared address so that the bot cannot tell who asks (below).

## Answers

An answer goes back the way the command came. To a letter it is a letter (`Re: ...`) of plain words. To a chat it is plain words
followed by one last line for programs, which starts with `nickbot: ` and holds one line of JSON:

```
@ben is BM-2cT...
nickbot: {"v":1,"cmd":"address","ok":true,"query":"@ben","found":true,"nick":"@ben","address":"BM-2cT..."}
```

| Field | Meaning |
|---|---|
| `v` | protocol version, 1; a program ignores fields it does not know |
| `cmd` | the command that was understood (empty if none) |
| `ok` | the command was carried out; a lookup that finds nothing is `ok` with `found:false` |
| `error` | the error code, when not `ok` |
| `query` | what was asked, only when it was valid (the bot never repeats text it has not checked) |
| `found`, `nick`, `address` | the result of `address`; `nick` and `address` also in `register` and `unregister` |
| `already`, `replaced` | for `register`: the name was the sender's already; the name it took the place of |
| `nicks` | for `whoami` |
| `results` | for `nickname`: `[{"address","nicks":[...]}]` in the order asked; for `search`: `[{"nick","address"}]` |
| `more` | for `search`: there were more than the 10 returned |

Error codes: `usage`, `unknown_command`, `syntax`, `too_short`, `reserved`, `taken`, `not_chan`, `anonymous`, `static`,
`not_yours`, `not_found`, `rate_limited`, `busy`, `internal`.

## Privacy

Looking a name up should not tell anyone who looked. ynotbit joins the chan whose passphrase is `anonymous` (everybody can: the
phrase is public) and sends every lookup from its address, so the bot sees one sender. The bot answers to that address; the
answer is encrypted to the chan's key, so every ynotbit client receives every answer. They are public registry data, and a
client can fill its own cache from other people's answers. Registering needs the user's own address, because it is the proof of
who owns the name.

The chan lives in the vault, under the label `#nickbot lookups`, so every mailbox of the vault finds it and none makes a
second one; the label is how ynotbit tells it from a chan `anonymous` the user joined by hand (that one it leaves alone).

The bot keeps no record of lookups: its log says counts and errors, never a query. The shared address may look up and may not
register, unregister or ask `/whoami`.

## In ynotbit

- **Settings → Nickbots** lists up to five nickbots. The first is the default; each has a colour (the first four are the colours of
  the quote levels of a letter, the fifth is coral) and moving a nickbot moves its colour. The page also has a switch, *Use
  nickbots* (off, and nothing is ever sent to one). The list is kept in the open mailbox (`nickbots:list`); a mailbox that never
  set one has the built-in default, `/nickbot`.
- **The Identities page** is a table like the address book, with the columns *Address*, *Nickname* and *Local name* (the default
  identity and a chan carry a pill beside the name). The buttons for the selected row are in the toolbar: *Nickname…*, then
  copy the address, show its QR code, make it the default, rename, delete, and *Join a channel…* and *New identity* beside
  them; the right-click menu holds the same, and a double-click does what its cell is about (copy an address, ask for a
  nickname, rename). *Nickname…* asks a nickbot, by a chat from the identity, to give it the nickname. The cell then shows the
  nickname as it stands, in its nickbot's colour; while a try waits for its answer it is muted, and a try that was refused or
  went unanswered is red, with the reason in the tip. A chan has no nickname to ask for here.
- **Contacts → Find people…** asks a nickbot: a whole nickname (`@ben`) for its address, an address for its nicknames, any other
  word of four letters or more is searched for. The answer takes minutes (the window can be closed), the table shows it in the
  nickbot's colour, and *Add to contacts* names each contact by its nickname.
- **Contacts → Check nicknames** asks the default nickbot for the nicknames of every contact, eight to a message and the messages
  ten seconds apart, so that a long address book is no burst. The page says that the check is under way (the button waits) and
  when it is over; what the nickbot has appears in the Nickname column as the answers come, and a nickname the nickbot vouched for
  earlier and no longer has is taken away. **Nothing is asked about the contacts unless the user presses it**: there is no
  setting that does it on its own.
- **New identity** has *Register with the nickbot as*, on, with the nickname made from the Nickname field (`Quick Otter of Zeta`
  becomes `@quick_otter_of_zeta`) unless you write one. With a new vault, which has no mailbox yet, the registration goes out when
  the first mailbox opens.
- **Where a nickname shows**: the letter list (from and to), the header of a letter, the chat list and the Contacts table. The
  nickname comes first, in its nickbot's colour, then the name you gave the contact; a name that is only the one made up from the
  address gives way to the nickname. A nickname set by hand or built in (`/echobot`) is never replaced by a nickbot's, only
  confirmed; one a nickbot vouched for goes when the nickbot says the address has none.
- **What goes out, and when.** A registration is a chat from the identity to the nickbot (the user can read it and the answer in the
  Chats page, without the last line, which is for programs). A lookup is a chat of the chan `anonymous` to the nickbot, with an
  hour to live, sent 3 to 30 seconds after the click (the messages of a check of the address book ten seconds apart), and never within
  two minutes of a message that names the user. Only the user's own action sends one. What comes back to that chan is filed under
  a hidden folder (`Service`), read once, and deleted; only a listed nickbot's answers are believed, and each is checked again
  for a bad nickname or address before anything is kept.
- A chan the user joined by hand under the phrase `anonymous` is not taken over: lookups then say why they are off.

## Running it

A container from the shared image, target `nickbot`:

```
scripts/build-image.sh user@host ynotbit-nickbot:2026-10-25 nickbot       # builds on that host's Docker
```

It needs a phrase (the phrase *is* its address; keep a copy) and a volume (an address that a vanity search found has no phrase: see
below the flags):

```
mkdir -p /data/nickbot/data /data/nickbot/secrets
head -c 32 /dev/urandom | base64 > /data/nickbot/secrets/phrase            # or a keys file from tools/vanity, see below
chown -R 10001:10001 /data/nickbot/data /data/nickbot/secrets && chmod 400 /data/nickbot/secrets/phrase
docker run -d --name nickbot --restart unless-stopped --memory 256m --cpus 1 \
  --ulimit memlock=33554432:33554432 --read-only --tmpfs /tmp:size=16m,noexec,nosuid \
  --cap-drop ALL --security-opt no-new-privileges:true \
  -p 8445:8444 -v /data/nickbot/data:/data -v /data/nickbot/secrets/phrase:/run/secrets/phrase:ro \
  -v /data/nickbot/seed.txt:/etc/nickbot/seed.txt:ro \
  --log-opt max-size=10m --log-opt max-file=3 \
  ynotbit-nickbot:2026-10-25 --seed /etc/nickbot/seed.txt --data-dir /data --phrase-file /run/secrets/phrase \
  --listen --port 8444 --retention-mb 512 --retention-days 4
docker logs nickbot | head          # "Address: BM-..." and "Lookups come from the chan ..." are on the first lines
```

(Publish another host port, as above, when the echo bot already uses 8444.) What is in `/data`: `state/` (the bot's vault, mailbox
and node), `nickbot.sqlite` (the registry), `backup/` (a copy a day, seven kept), `status.json` (every ten seconds).

| Flag | Default | |
|---|---|---|
| `--data-dir` | `/data` | |
| `--phrase-file` | `/run/secrets/phrase` | first line is the secret phrase |
| `--keys-file` | none | instead of the phrase file: the address's two private keys, one to a line (see below) |
| `--seed` | none | the operator's names |
| `--public` | true | join the public network; `--public=false --peer IP:port` links to one peer |
| `--listen`, `--port` | off, 8444 | accept incoming connections |
| `--retention-mb`, `--retention-days` | 512, 4 | what the node keeps of the network's objects |
| `--anon-phrase` | `anonymous` | the phrase of the lookup chan; clients must agree |
| `--changes-per-day` | 5 | names one address may register or change in 24 hours |
| `--new-per-hour` | 200 | registrations and changes the whole registry takes in an hour |
| `--max-nicks` | 1000000 | |
| `--hang` | 120s | abort when the library's loop stalls, so that Docker starts it again |

**An address with a word in it.** [`tools/vanity`](../tools/vanity/README.md) searches for addresses such as `BM-87nickbot...` and
finds their two keys, not a phrase. `bm_vanity --export ADDRESS -f hits.txt -o nickbot.keys` writes the keys (the signing key, then the
encryption key, one to a line); put that file where the phrase would be (mode 400, owned by the container's user, mounted at
`/run/secrets/keys`) and start the service with `--keys-file /run/secrets/keys` in place of `--phrase-file`. The first log line,
`Address: BM-...`, has to be the address `bm_vanity --address-of nickbot.keys` prints. The keys file is the address: keep a copy.
Move it between machines with `ssh ... | ssh ...` under `umask 077`, never through a chat, a mail or a repository.

Health: `nickbot --check /data/status.json --max-age 90s --peers 1` (the image's `HEALTHCHECK`). Stop with `docker stop` (SIGTERM,
"Bye."). Limits: 10 commands at once and one every 6 seconds per address, 60 and two a second for the shared address, 600 answers
and one every 6 seconds in all; what is over is read, noticed once a minute, then dropped. A message that is no command is
answered twice per sender per ten minutes, so two bots cannot keep each other talking.

### Operating on the registry

The registry can be read and changed while the service runs:

```
docker exec nickbot nickbot admin -db /data/nickbot.sqlite stats
docker exec nickbot nickbot admin -db /data/nickbot.sqlite export > registry.tsv
docker exec nickbot nickbot admin -db /data/nickbot.sqlite remove @someone      # free at once
docker exec nickbot nickbot admin -db /data/nickbot.sqlite reserve @someone     # removes it and keeps it free
docker exec nickbot nickbot admin -db /data/nickbot.sqlite history BM-...
```

An event the library hands out twice (after a crash) is answered with the same words and done once: the id of a handled event and
its answer are kept for seven days in the same transaction as the change.

## Tests

`cd nickbot && go test -race ./...` for the rules (no C needed); `ctest --test-dir build-core -R nickbot` with
`-DYNOTBIT_NICKBOT=ON` for the cgo layer and for `tests/nickbot/nickbot_e2e_test.py`, which runs the binary over a real `bmnode`
with a Python bot as its client. The nickname syntax is one table, `nickbot/testdata/nick_cases.txt`, read by the Go and the C++
tests.
