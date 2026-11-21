# bmbot/go

The plumbing that bots written in Go share, as one Go module: `github.com/yshurik/ynotbit/bmbot/go`. It is the API of ynotbit's own
bots (nickbot, and btcbot in its own repository), kept small on purpose and at 0.x until two bots have run on it for a release.

| Package | What it is |
|---|---|
| `bmbot` | the cgo layer over [`bmbot.h`](../bmbot.h): `Open`, `Bot.Next/Done/SendChat/SendLetter/Status/Close`, `ChanAddress`. Build tag `libbmbot`; without it a stub returns `ErrNotBuilt`, so logic that uses a `Bot` tests without cgo |
| `loop` | the events of the library, the limits, the replies, and "done" only after the reply. A bot gives it a `Handler`: `Handle(Request) Reply` and `IsAnonymous(address) bool` |
| `limit` | token buckets in bounded memory |

The module needs nothing but the standard library; `imports_test.go` keeps it so. It needs libbmbot to link a program: the shared
library and `bmbot.h` from `cmake --install` (see [`bmbot/README.md`](../README.md)).

```
cd bmbot/go && go vet ./... && go test -race ./...                         # no cgo
cmake -S . -B build-core -DYNOTBIT_QT=OFF -DYNOTBIT_NICKBOT=ON && cmake --build build-core --target bmbot
ctest --test-dir build-core -R bmbot-go --output-on-failure                # with the library
```

A bot answers in the way described in [`docs/bot-trailer.md`](../../docs/bot-trailer.md).

The versions of the module are tags of the form `bmbot/go/vX.Y.Z`.
