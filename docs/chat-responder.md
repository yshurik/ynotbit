# chat_responder as a service

`chat_responder` (`tools/chat_responder.cpp`) answers the chats that come to an address of its own. It is a test peer for ynotbit's
Chats, and made to run for months: the base of a bot or a service on Bitmessage. It runs headless, with a vault, a mailbox and a node
of its own.

```
cmake --build build --target chat_responder
build/chat_responder --help
```

## What it keeps up, and how

- **Its network.** The node dials a peer it was told to link to (`--peer`) again every half minute at most, however long the peer was
  away, and forgets the backoff an earlier run saved; a peer that connected within 28 days is tried again within five minutes after a
  failure, not the hour a dead address waits. Sockets are kept alive by the system (a peer that vanished is found in about two
  minutes). With `--public` it joins the public network and asks for 12 outgoing and 24 incoming peers (`--max-outgoing`,
  `--max-incoming`; the defaults of a desktop are 8 and 8), and `--listen` accepts incoming connections.
- **A node that does no good is started again.** One whose status stopped for two minutes is restarted; with `--watchdog <minutes>`
  (15 with `--public`) one that has had no peer that long is too. Never twice within five minutes.
- **What came while it was down is answered.** What has come and nobody has looked at is the queue, and the mailbox keeps it. A chat is
  marked read only once its answers are queued, so a bot that was stopped, crashed or lost its network answers what waited when it
  runs again. That is *at least once*: a crash between the last answer and the mark answers a chat twice.
- **A peer cannot make it answer without limit.** `--rate` answers a minute to one peer after a first `--burst`, and `--global-rate`
  and `--global-burst` for all peers together; what is over is dropped and read, not kept for later.
- **What it remembers is bounded.** At most `batch` (50) chats are held, the rates of recent peers are remembered for ten minutes, and
  at most 200 answers are followed to the end of their delivery.
- **Its log keeps to a size.** `--log <file>` writes what it says to a file as well, rotated at 10 MB with five kept.
- **It can be watched.** `--status-file <file>` is made again every ten seconds (peers, counts, uptime), and the log says when the
  number of peers changes and, every ten minutes, that it is alive.

## The address and its secret

The address comes from a phrase. The default phrase is public, so anyone can read what is sent to that address: it is for trying chats
out. For a service, give it a phrase of its own in a file (or in `$YNOTBIT_RESPONDER_PHRASE`), not in the command line, where the
process list shows it:

```
head -c 32 /dev/urandom | base64 > responder.phrase && chmod 600 responder.phrase
build/chat_responder --phrase-file responder.phrase ...
```

The vault's password comes from the phrase then, so the vault and the mailbox on the disk are of no use without it. Keep the phrase:
it *is* the address, and a lost phrase is a lost address. A data folder made with the public phrase does not open with a secret one, or
the other way round.

## Running it under systemd

A user service, tried on Linux (systemd 255) with a `SIGKILL` of the main process (systemd started it again ten seconds later,
with the same address, and its node was gone with the old process) and a graceful stop (`Bye.` in the log, exit status 0):

```
# ~/.config/systemd/user/ynotbit-responder.service
[Unit]
Description=ynotbit chat responder
After=network-online.target

[Service]
ExecStart=%h/ynotbit/chat_responder --public --listen --peer "" --phrase-file %h/.ynotbit/responder.phrase --data-dir %h/.ynotbit/responder --log %h/.ynotbit/responder.log --status-file %h/.ynotbit/status.json
Restart=always
RestartSec=10
TimeoutStopSec=30
StandardOutput=null
StandardError=journal

[Install]
WantedBy=default.target
```

```
systemctl --user daemon-reload
systemctl --user enable --now ynotbit-responder
loginctl enable-linger $USER    # keeps a user's services running when nobody is logged in
```

`--listen` asks for port 8444; where something else holds it (another ynotbit node on the machine) the node listens on a port the
system picks and tries 8444 again every 15 minutes, or give `--port <n>` a port of its own. Open the port on the router for incoming
connections to work; without it the responder still connects out.

## Trying it

`--hello <address>` says hello to an address once at start, and `--silent` answers nothing: two responders, one `--silent --hello` the
address of the other, are a chat across a link in a minute, a test of the link or the probe of a monitor that waits for the answer.
Ask it for `/help`, `/ping`, `/time`, `/md`, `/long` and `/many 5`; anything else it says back.
