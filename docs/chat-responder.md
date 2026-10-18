# chat_responder as a service

`chat_responder` (`tools/chat_responder.cpp`) answers the chats that come to an address of its own. It is a test peer for ynotbit's
Chats, and made to run for months: the base of a bot or a service on Bitmessage. It runs headless, with a vault, a mailbox and a node
of its own, and it uses no Qt: it is the reference bot on the C ABI of `bmbot` (`bmbot/bmbot.h`, see `bmbot/README.md`), so what it
does a bot in Go or Python can do.

```
cmake -S . -B build-core -DYNOTBIT_QT=OFF
cmake --build build-core --target chat_responder bmnode
build-core/chat_responder --help
```

(`chat_responder` and `bmnode` are built in a tree with Qt too: `cmake --build build --target chat_responder bmnode`.) It starts its node
as a child process, `bmnode`, which it finds beside itself or on the `PATH`; `--node-bin <path>` names it.

## What it keeps up, and how

- **Its network.** The node dials a peer it was told to link to (`--peer`) again every half minute at most, however long the peer was
  away, and forgets the backoff an earlier run saved; a peer that connected within 28 days is tried again within five minutes after a
  failure, not the hour a dead address waits. Sockets are kept alive by the system (a peer that vanished is found in about two
  minutes). With `--public` it joins the public network and asks for 12 outgoing and 24 incoming peers (`--max-outgoing`,
  `--max-incoming`; the defaults of a desktop are 8 and 8), and `--listen` accepts incoming connections.
- **A node that does no good is started again.** One whose status stopped for two minutes is restarted; with `--watchdog <minutes>`
  (15 with `--public`) one that has had no peer that long is too. Never twice within five minutes. A node that exits is started
  again after 30 seconds, and a node that cannot be started (a missing binary, a busy port) is tried again as often.
- **A process that does no good is started again too.** `--hang <seconds>` (120; 0 never) makes the process abort when its driver loop
  has not run for that long, so that Docker or systemd starts it again; the node goes with it.
- **What came while it was down is answered.** What has come and nobody has looked at is the queue, and the mailbox keeps it. A chat is
  marked read only once its answers are queued, so a bot that was stopped, crashed or lost its network answers what waited when it
  runs again. That is *at least once*: a crash between the last answer and the mark answers a chat twice.
- **A peer cannot make it answer without limit.** `--rate` answers a minute to one peer after a first `--burst`, and `--global-rate`
  and `--global-burst` for all peers together; what is over is dropped and read, not kept for later.
- **What it remembers is bounded.** At most `batch` (50) chats are held, the rates of recent peers are remembered for ten minutes, and
  at most 200 answers are followed to the end of their delivery.
- **One process to a folder.** Two bots on one data folder would write one mailbox: the second one is refused ("in use by another
  process"), whatever started it.
- **Its log keeps to a size.** `--log <file>` writes what it says to a file as well, rotated at 10 MB with five kept.
- **It can be watched.** `--status-file <file>` is made again every ten seconds (peers, counts, uptime), and the log says when the
  number of peers changes and, every ten minutes, that it is alive. `chat_responder --check <status-file> [--max-age 60] [--peers 1]`
  exits 0 when that file is fresh and says that many peers, else 1: a health check that needs no shell tools.
- **Its node's disk is bounded.** `--retention-mb` and `--retention-days` (2048 and 90) say how much of the network's objects the node
  keeps; a small machine wants less (512 and 4 are plenty for a bot, since the network drops an object after about three days).

## The address and its secret

The address comes from a phrase. The default phrase is public, so anyone can read what is sent to that address: it is for trying chats
out. For a service, give it a phrase of its own in a file (or in `$YNOTBIT_RESPONDER_PHRASE`), not in the command line, where the
process list shows it:

```
head -c 32 /dev/urandom | base64 > responder.phrase && chmod 600 responder.phrase
build-core/chat_responder --phrase-file responder.phrase ...
```

The vault's password comes from the phrase then, so the vault and the mailbox on the disk are of no use without it. Keep the phrase:
it *is* the address, and a lost phrase is a lost address. A data folder made with the public phrase does not open with a secret one, or
the other way round, and neither opens with another phrase ("made with another phrase").

The phrase is the first line of the file, as it is; the address is the first key pair of that phrase, as PyBitmessage derives it. A
phrase can therefore be searched for an address with a word in it (a phrase `<secret>:<number>`, the number the search's): the responder
needs nothing to know.

`--data-dir <dir>` is the folder as it is given. With none, a folder for each phrase (named by a hash of it) is made under
`~/.local/share/YnotbitTools/chat_responder` (`~/Library/Application Support/...` on a Mac), as it always was. A folder of the first
`chat_responder` (`responder.bmvault` in it) opens as it was.

## Running it under systemd

A user service, tried on Linux (systemd 255) with a `SIGKILL` of the main process (systemd started it again ten seconds later,
with the same address, and its node was gone with the old process) and a graceful stop (`Bye.` in the log, exit status 0):

```
# ~/.config/systemd/user/ynotbit-responder.service
[Unit]
Description=ynotbit chat responder
After=network-online.target

[Service]
ExecStart=%h/ynotbit/chat_responder --public --listen --phrase-file %h/.ynotbit/responder.phrase --data-dir %h/.ynotbit/responder --log %h/.ynotbit/responder.log --status-file %h/.ynotbit/status.json
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
connections to work; without it the responder still connects out. (There is no UPnP here: a desktop's ynotbit asks the router, a
service does not.)

## In a container

`docker/Dockerfile` builds an image with `chat_responder`, `bmnode` and `libbmbot` on a slim Debian, with no Qt, running as an
unprivileged user under `tini`:

```
scripts/build-image.sh [ssh-host] [tag]       # from the tree as it is; with a host, built there
```

The data (the vault, the mailbox, the node's objects, the log and the status file) is the volume `/data`; the phrase is a file mounted
read-only at `/run/secrets/phrase`. The image's health check is `chat_responder --check` on the status file.

```
mkdir -p data && head -c 32 /dev/urandom | base64 > phrase && chmod 400 phrase
sudo chown -R 10001:10001 data phrase           # the image's user
docker run -d --name responder --restart unless-stopped --stop-timeout 30 \
  --read-only --tmpfs /tmp:size=16m --cap-drop ALL --security-opt no-new-privileges \
  --memory 512m --cpus 2 --ulimit memlock=33554432:33554432 \
  --log-opt max-size=10m --log-opt max-file=3 -p 8444:8444 \
  -v "$PWD/data:/data" -v "$PWD/phrase:/run/secrets/phrase:ro" ynotbit-bmbot:dev
docker logs responder | head                    # "Address:  BM-..." is on the first lines
```

The `memlock` limit is not for show: SQLCipher locks the memory its keys are in, and under a small limit (64 KiB on some hosts)
every lock fails and says so on the log, thousands of lines a minute. `docker/compose.yaml` is the same with Docker Compose. A restart, a crash, a reboot of the host and a new image all keep the address and
what waited: they live in `/data` and the phrase. Port 8444 is published, and the host's firewall or router must let it in; in bridge
networking nothing can ask a router for it.

## Trying it

`--hello <address>` says hello to an address once at start, and `--silent` answers nothing: two responders, one `--silent --hello` the
address of the other, are a chat across a link in a minute, a test of the link or the probe of a monitor that waits for the answer
(`tests/bmbot/responder_e2e_test.py` does it over two real nodes). Ask it for `/help`, `/ping`, `/time`, `/md`, `/long` and
`/many 5`; anything else it says back.
