# nickbot

Nicknames for Bitmessage addresses: `@ben` for a person, `/echobot` for a bot, `#general` for a chan. A Go service on
[`libbmbot`](../bmbot/README.md) (cgo), with its registry in SQLite. It answers chats from ynotbit and letters from any client.
The protocol, the rules and how to run it are in [`docs/nickbot.md`](../docs/nickbot.md); the design is in
[`docs/superpowers/specs/2026-10-25-nickbot-design.md`](../docs/superpowers/specs/2026-10-25-nickbot-design.md).

```
cmake -S . -B build-core -DYNOTBIT_QT=OFF -DYNOTBIT_NICKBOT=ON && cmake --build build-core     # the binary: build-core/nickbot/nickbot
ctest --test-dir build-core -R nickbot                                                         # the Go tests and the end-to-end test
```

Without the library, the packages that make up the rules build and test with plain Go:

```
cd nickbot && go vet ./... && go test -race ./...
```

| Package | What it is |
|---|---|
| `internal/nick` | the syntax of a nickname and of a search term |
| `internal/bmaddr` | what a Bitmessage address is, the way the C++ core reads it |
| `internal/store` | the registry in SQLite: every rule of a name, in one transaction each |
| `internal/limit` | token buckets in bounded memory |
| `internal/service` | the commands and their answers, with the trailer for programs |
| `internal/loop` | the events of the library, the limits, the replies, and "done" only after the reply |
| `internal/bmbot` | the cgo layer over `bmbot.h` (build tag `libbmbot`) |
| `cmd/nickbot` | the process: flags, the status file, backups, the `admin` commands |
