# Qt-free core and bot platform — design (2026-10-15)

## Why

`tools/chat_responder` was made robust on 2026-10-15 (reconnect policy, node watchdog, durable
queue, rate limits), but it cannot be shipped lean: it is a Qt program. Its code closure is
6,657 lines of `.cpp` with 1,131 Qt uses over 57 Qt classes. It links Qt Core, Gui and Widgets
because it reaches the vault, the mailbox and the node through `Session`, which holds about 14
dialog and clipboard calls, and it starts a `QApplication` on the `offscreen` platform only to run. A
container for it would carry the whole Qt stack.

The goal is larger than the responder. It is a **platform for bots and services**: a library that
Go, Python and other languages link, running for months in a container, with no Qt anywhere in
its source.

Qt is not what the client logic is made of, only what its signatures are spelled in:

- The node (`third_party/notbit`, C) has no Qt except one file, `src/relay_bridge.cpp` (104
  lines), and it only relays. Its daemon runs "without an identity keyring"
  (`ntb-daemon.c`, `run_main_loop`), so it neither decrypts nor sends. It meets the app in files:
  `objects.sqlite` in, `publish/*.object` out, `receipts/`, `status.json`.
- Everything that makes a client lives in `bmcore` (identity, decryption, chat format,
  acknowledgments, proof of work, sending, the mailbox; about 3,500 lines in twelve files) and
  `Session` (about 1,300). That logic is plain classes with no Qt event loop: `Delivery::scan`
  and `Delivery::tick` are called by one timer in `Session::tick`. Only Qt value types
  (`QString`, `QByteArray`, `QVector`) and a few utilities (`QDataStream`, `QCryptographicHash`,
  `QSaveFile`, `QUuid`, `QJson*`) tie it to Qt.

## Decisions

1. **Zero Qt in the source of the bot platform.** The user chose this over keeping Qt Core
   (static or shared) on 2026-10-15.
2. **Refactor in place, not a second implementation.** The core moves out of Qt and the app
   uses it too. One implementation of the protocol, shared by the app and the bots. (User's choice
   over a parallel core.)
3. **Three layers:** `bmclient` (C++20, std only, namespace `bmc`) at the bottom; `bmbot` (a C
   ABI and a bot runtime) over it; a Qt "veneer" in `src/` (target `bmcore`) over it. The names
   `bmclient`, `bmbot`, `bmc` and `bmnode` are chosen here; renaming later is mechanical.
4. **Language and ABI:** the core is C++20 like the app, with std and the libraries the app already
   uses (OpenSSL, libsodium, SQLCipher, libsecp256k1, the notbit C code). `bmbot` is `extern "C"`,
   with no struct crossing the ABI, no callbacks, and JSON for events and status.
5. **The node stays a child process** that the library spawns (isolation, a watchdog that can
   kill it). A new Qt-free executable `bmnode` is the node for bots; the app keeps its `--node`
   re-exec.
6. **On-disk formats do not change.** Vault, mailbox, `objects.sqlite`, `status.json`,
   `publish/` and `receipts/` stay byte-compatible. Existing user data keeps opening, and the
   previous release can open what the new code writes.
7. **Errors:** the core throws `std::runtime_error` subclasses whose `what()` is the same English
   text as today, with a `Code` for the C ABI. No localized text in the core. `Session::errorText`
   keeps translating `what()` through the `bm::Errors` context.
8. **Scope of the move: the spine.** Message search, the UPnP port mapper, the key-search threads
   (`KeySearch`), the GPU solver, `i18n`, `appearance`, the models and the widgets stay Qt. GPU proof of
   work plugs into the core through an interface.
9. **Platforms:** the core must compile on Linux, macOS and Windows (CI builds it in the app jobs).
   `bmbot` and the node supervisor are POSIX first (Linux, macOS); Windows later.
10. **API v1 exposes chats and letters.** The core keeps all app behaviour (broadcasts, chans,
    contacts, subscriptions).
11. **Method:** work on the branch `feature/qt-free-core`, in slices that each leave the whole test
    suite green, with golden vectors taken from today's code before each port (TDD). Nothing is
    committed or pushed until the user asks, slice by slice. The robustness work now sitting
    uncommitted in the tree needs committing before slice 1 (the user decides when).

## Design

### Layers

```
third_party/notbit  (C)      engine, object db, codec            — unchanged except relay_bridge
bmclient/           (C++20)  Vault, Mailbox, Protocol, Wire, ProofOfWork, Delivery, Cache, Client, node tools
bmbot/              (C ABI)  libbmbot + bmbot.h: inbox, send, state, status, supervisor, limits
src/  (bmcore)      (Qt)     veneer: bm::Vault, bm::Mailbox, ... with today's Qt signatures, over bmclient
src/  (the rest)    (Qt)     Session, windows, models, dialogs                  — unchanged API use
tools/chat_responder         terminal app on libbmbot's C ABI, no Qt
```

Dependencies: `bmclient` uses std, `notbit_codec`, `ntb_object_db`, libsecp256k1, OpenSSL,
libsodium, SQLCipher. `bmbot` uses `bmclient`. `bmcore` uses `bmclient` and Qt. Nothing in
`bmclient/` or `bmbot/` includes or links Qt; this is checked by building them in a container with
no Qt installed (see Build) and by `nm`/`ldd` on `libbmbot`.

### What moves, what stays

| Today | Goes to | Notes |
|---|---|---|
| `storage.{h,cpp}`: `Secret`, `Identity`, `Message`, `ChatMessage`, `ChatSummary`, `Vault`, `Mailbox`, records | `bmclient/{secret,records,vault,mailbox}.*` | `Secret` is already Qt-free; `SensitiveBytes` becomes a wiped `Bytes` |
| `mailbox_ops.cpp`, `chat_store.cpp`, `sqlite_helpers.h` | `bmclient/mailbox_*.cpp`, `bmclient/sqlite.h` | `message_search` keeps a Qt-flavoured copy of the helper |
| `protocol.*`, `protocol_wire.*` | `bmclient/{protocol,wire}.*` | |
| `pow.*` | `bmclient/pow.*` | `PowSolver` interface; `gpu_pow.*`, `gpu_opencl.cpp`, `gpu_metal.mm` stay and register as the solver |
| `delivery.*`, `cache.*`, `scanner.*` | `bmclient/{delivery,cache,scanner}.*` | |
| `chat_format.*`, `updates.*` | `bmclient/*` | needs `bmc::json` |
| `key_search.*`: `phraseIdentity`, `firstPair`, `zeroBytes` | `bmclient/phrase.*` | the `KeySearch` QObject stays and calls them |
| `relay_bridge.cpp`, `node_watch.h` | `bmclient/*` | the bridge is rewritten in std; every node executable links it |
| `Session`: the `tick()` loop, chat sending and validation, `nodeArguments()` | `bmc::Client`, `bmc::NodeConfig` | `Session` keeps the timers, `QProcess`, dialogs, `QSettings`, signals and `tr()` |
| `tools/chat_bot.*` | `bmbot/` (rate limits, retry, bounded memory) | |
| `tools/rotating_log.*` | `tools/` (std) | terminal app only |

Everything else in the app stays as it is.

### Conventions of the core

- **Bytes and text.** `using Bytes = std::vector<uint8_t>`; `using ByteView =
  std::span<const uint8_t>` (the app is C++20); text is UTF-8 `std::string` /
  `std::string_view`; addresses are `std::string` (`BM-...`); integers are `int64_t`/`uint64_t`.
- **Unicode.** `QString` counts UTF-16 units and `trimmed()`, `left()`, `toLower()` act on them. The
  core has `bmc::utf8` helpers (validate, truncate at a code point, trim Unicode white space, UTF-8
  to UTF-16BE and back). Every place that depends on a `QString` rule is found by `grep`, pinned
  by a test, and either kept or changed on purpose (listed in the slice that touches it). No
  locale- or ICU-dependent behaviour in the core.
- **Time.** Seconds since the epoch, `int64_t`. Functions that need the time take `now`, or a
  `bmc::Clock` that tests replace. Budgets use `std::chrono::steady_clock`.
- **Errors.** `bmc::Error : std::runtime_error` with `enum class Code` and the exact English
  `what()` of today. `scripts/update-error-catalog.py` and `scripts/check-translations.py` learn the
  new directories, so the `bm::Errors` catalogue and the translations stay complete.
- **Threads.** The core is single-threaded except `ProofOfWork`, which has its own workers. A
  `Client` is used by one thread at a time. `bmbot` runs a driver thread and serializes every call
  on one mutex, so the C API is thread-safe. The app keeps using the core from the GUI thread.
- **Logging.** The core never prints. Problems and watchdog restarts are values
  (`Client::notes()`), which `bmbot` turns into events.
- **Qt to std, for the port:**

| Qt | Core |
|---|---|
| `QString`, `QStringList` | `std::string`, `std::vector<std::string>` |
| `QByteArray` | `Bytes` / `ByteView` |
| `QVector`, `QMap`, `QHash` | `std::vector`, `std::map`, `std::unordered_map` |
| `QFile`, `QSaveFile`, `QDir`, `QFileInfo` | `std::filesystem` (paths as UTF-8 through one helper) and an atomic write (temp file, `fsync`, rename) |
| `QDateTime`, `QElapsedTimer` | `<chrono>`, seconds as `int64_t` |
| `QUuid::createUuid()` | `bmc::uuid()` (random version 4, same 36-character lowercase text) |
| `QCryptographicHash` | OpenSSL EVP (SHA-1, 256, 512) |
| `QtEndian` | explicit big-endian helpers |
| `QJsonDocument` | `bmc::json`: a small strict parser and writer (objects, arrays, strings with `\u` and surrogates, int64 and double numbers, bool, null) |
| `QDataStream` | `bmc::qtstream`, only what the vault uses (below) |
| `QMutex`, `QThread` | `std::mutex`, `std::thread` |

### On-disk compatibility

- **Vault.** File: `BMVAULT2`, 16-byte salt, 24-byte nonce, then XChaCha20-Poly1305 over the
  payload with the 48-byte header as associated data; the key is Argon2id (3 passes, 64 MiB) of the
  password. `BMVAULT1` (no default flag) is still read. The payload is a `QDataStream` at
  `Qt_6_0`: big-endian `quint32` count; per identity a `QString` label, a `QString` address, a
  `bool` chan, a `bool` default, 64 raw key bytes; then a `quint32` count and, per mailbox key, a
  `QString` id and 32 raw bytes. A `QString` is a `quint32` byte length followed by UTF-16BE (a null
  string is length `0xFFFFFFFF`, which the reader accepts as empty). `bmc::qtstream` writes and reads
  exactly this and nothing else.
- **Mailbox (`.bmmail`).** A SQLCipher database. The schema, the `user_version` migrations
  and every SQL statement move as they are. The partial index `chat_unread` stays.
- **`objects.sqlite`.** Read-only to the client, through the same SQL. The node writes it through
  `ntb-object-db.c`, untouched.
- **Node link.** `status.json`, `publish/<uuid>.object` (the name matches `^[a-f0-9-]{36}$`,
  at most 8 offered per tick) and `receipts/<uuid>.json` keep their names and fields.
- **Settings.** `desktop.ini` (`QSettings`) stays the app's. The core takes a `NodeConfig` struct;
  `Session` fills it from the ini, `bmbot` from its JSON config.

### The Qt veneer

`src/storage.h`, `protocol.h`, `protocol_wire.h`, `delivery.h`, `pow.h`, `cache.h`,
`chat_format.h`, `key_search.h` (the `KeySearch` class), `updates.h` keep their names, namespace
(`bm`) and Qt signatures. Their `.cpp` files become wrappers over `bmc` objects:

- `bm::Mailbox::store(QString, ...)` converts its arguments (`src/qt_bridge.h`: `s()`, `q()`, byte and
  struct converters), calls `bmc::Mailbox`, converts the result back. About 100 wrappers for
  `Mailbox`, the rest are small. `bm::Secret` becomes an alias of `bmc::Secret`.
- The app and all 27 `ctest` suites keep compiling against the same headers. A test that reaches
  into an internal (a `sqlite3*`, `detail::Transaction`) gets a small edit, listed in the slice plan.
- GUI code may adopt `bmc` types later, or never; nothing requires it.
- A `bm::` object that belongs to a `Session` and a `bmc::Client` share one `bmc::Mailbox` through
  `std::shared_ptr`.

### The client loop

`bmc::Client` owns (through shared pointers) a `Vault`, a `Mailbox`, a `Cache` and a `Delivery`.

- `step(now, online)` is `Session::tick` without the Qt: bind the mailbox to the cache id, note
  pruned objects, `scan` with a time budget (512 objects, 150 ms), `Delivery::tick`, return a
  `StepReport` (activity, working, pruned).
- Chat and letter services with the rules `Session` has now: `sendChat` (sender is a non-chan
  identity of the vault, valid recipient different from the sender, 1 to 8192 body bytes, 24 h
  expiry), `unreadChats`, `markChatMessageRead`, `chatState`, `sendLetter`.
- `Session::tick` and the chat methods call it; `Session` keeps `QTimer`, `QProcess`, `emit`.

### The node

- **`bmnode`** (new, Qt-free): `ntb_daemon` plus the std relay bridge. It checks its parent as today
  (`YNOTBIT_PARENT_PID`). The app and the tools keep `--node` re-exec.
- **`bmc::NodeConfig` and `nodeArguments()`:** a pure function of peer, proxy, listen, port,
  `maxOutgoing`, `maxIncoming`, retention and the public-or-private choice
  (`-b -B -e` unless public). `Session::nodeArguments` calls it.
- **`bmc::NodeWatch`** (already pure) and a **`bmc::NodeSupervisor`** (POSIX: `posix_spawn`, own
  process group, `waitpid` polling, SIGTERM then SIGKILL after 3 s, restart on exit, watchdog restarts
  at least 300 s apart). The behaviours are those of `Session` today.
- **Node status** is read by a `bmc::NodeStatus` parser over `status.json`.

### The bot library

`bmbot/bmbot.h`, a C ABI. All strings are UTF-8 `char*`; every function that can fail returns an `int`
code (`0` ok, `1` timeout, negative errors: argument, state, auth, io, network, internal) and
takes an optional `char **error` (English text, freed with `bmbot_free`), so it is safe from Go
goroutines and Python threads (no thread-local state). Nothing but `bmbot *`, `int`, `const char *`
and `char **` crosses the ABI.

```c
int  bmbot_abi_version(void);
int  bmbot_open(const char *config_json, bmbot **bot, char **error);
void bmbot_close(bmbot *bot);                       /* wakes waiters, which get a state error */
int  bmbot_next(bmbot *bot, int timeout_ms, char **event_json, char **error);
int  bmbot_done(bmbot *bot, const char *event_id, char **error);
int  bmbot_send_chat(bmbot *bot, const char *from, const char *to, const char *text,
                     char **id, char **error);
int  bmbot_send_letter(bmbot *bot, const char *from, const char *to, const char *subject,
                       const char *body, char **id, char **error);
int  bmbot_state(bmbot *bot, const char *id, char **state, char **error);
int  bmbot_status(bmbot *bot, char **status_json, char **error);
void bmbot_free(void *ptr);
```

- **Config (JSON):** `data_dir`; `phrase_file` or `phrase` (the address comes from the phrase as
  today; a secret phrase also derives the vault password, the public default phrase keeps its
  constant password); `node` (`bin`, `peers`, `public`, `listen`, `port`, `max_outgoing`,
  `max_incoming`, `watchdog_minutes`); `limits` (per-peer and global burst and per-minute rates, off by
  default in the library, on in `chat_responder`); `hang_seconds` (the process aborts if the driver
  loop stalls that long, so Docker or systemd restarts it; off by default in the library, on in
  `chat_responder`).
- **Events (`bmbot_next`):** one JSON object: `id`, `kind` (`chat`, `letter`, `note`), `to`, `from`,
  `subject`, `text`, `seqno`, `received`. The inbox is **durable and at least once**: it is the
  mailbox's unread chats and letters, oldest first. An event is handed out once per run; it is marked
  read only by `bmbot_done`; after a crash or restart the ones not done come back. `note` events
  carry problems and watchdog restarts (the library never prints); they have no `id`, are not
  durable and need no `bmbot_done`.
- **States (`bmbot_state`):** the strings the app shows for a chat's delivery, unchanged.
- **Status:** peers (outgoing, incoming), objects, uptime, counts taken, answered, limited, failed,
  working, watchdog restarts, the network line, the notes.
- **Distribution:** `cmake --install` puts `libbmbot` (shared and static), `bmbot.h`, `bmbot.pc`,
  `bmnode`, `chat_responder` under a prefix. Examples: `bmbot/examples/echo.py` (ctypes) and
  `echo.go` (cgo).

### chat_responder and the container

- `tools/chat_responder.cpp` is rewritten without Qt, **on the C ABI** (it is the reference
  consumer): the flags of today (`--phrase-file`, `--public`, `--peer`, `--listen`, `--port`,
  `--max-outgoing`, `--max-incoming`, `--watchdog`, `--rate`, `--burst`, `--global-rate`,
  `--global-burst`, `--silent`, `--hello`, `--log`, `--status-file`, `--data-dir`, `--fresh`) keep
  their meaning and become config. Its answers, rotating log and status file stay.
- `chat_responder --check <status-file> [--max-age 60] [--peers 1]` exits 0 or 1, for a container
  health check with no shell tools in the image.
- `docker/Dockerfile` (multi-stage): a build stage with the toolchain and `scripts/build-dependencies.sh`,
  configured `-DYNOTBIT_QT=OFF`; a runtime stage (a slim Debian) holding `chat_responder`, `bmnode`
  and `libbmbot`, running as a non-root user. `docker/compose.yaml`: a data volume, the phrase as a
  secret file, `restart: unless-stopped`, `init: true`, `stop_grace_period: 30s`, log rotation
  options, a memory limit, a read-only root with a tmpfs `/tmp`, port 8444, the health check. In
  bridge networking UPnP cannot work; the doc says to use host networking or forward the port.

### Build and CI

- `option(YNOTBIT_QT "Build the Qt app, tools and tests" ON)`. Off: no `find_package(Qt6)`; the targets are the engine
  libraries, `bmclient`, `bmbot`, `bmnode`, `chat_responder` and the Qt-free tests. The engine,
  codec and dependency targets move out of the top of `CMakeLists.txt` into a file both
  configurations include. (Today `find_package(Qt6 ... Test LinguistTools)` is unconditional.)
- **Qt-free test harness:** `bmclient/test.h` (registered tests, `CHECK`/`REQUIRE`, `--filter`),
  no new third-party dependency. Tests live in `tests/bmclient/` and `tests/bmbot/`.
- **CI:** a new job on a plain Ubuntu image with no Qt packages builds `-DYNOTBIT_QT=OFF`, runs the
  Qt-free tests, the Python ctypes test and the responder end-to-end test; the existing app jobs
  build `bmclient` too (this is the Windows and macOS portability check).

## Order of work

Each slice ends with: the full `ctest` of the app green (27 suites plus the new ones), the
relevant tests green on big1 (Linux), the Qt-free build green in a Qt-less container, and
the old behaviour unchanged. Estimates are focused working days.

1. **Skeleton and wire (2–3 days).** `YNOTBIT_QT`, the CMake split, `bmclient` skeleton (bytes, text,
   json, error, hash, uuid, test harness), `Protocol`, `Wire`, `chat_format`, `updates`,
   `ProofOfWork` with `PowSolver`, the phrase functions. Veneers for them. Golden vectors first (see
   Testing).
2. **Storage (3–4 days).** `Secret`, `Vault` with `qtstream`, `Mailbox` and its helpers, chats,
   records. Veneers `bm::Vault`, `bm::Mailbox`. Golden vault and mailbox files. The riskiest slice: it
   holds user data.
3. **Delivery (2–3 days).** `Cache`, `scanMailbox`, `Delivery`, `Client`. `Session` calls `Client`.
   Qt-free scenario tests (two clients over a node-less "fake node": objects exchanged through
   `objects.sqlite` files written with the C writer).
4. **Node (1–2 days).** std relay bridge, `bmnode`, `NodeConfig`, `NodeStatus`, `NodeWatch`,
   `NodeSupervisor`. The relay and peer-connectivity tests also run against `bmnode`; supervisor
   tests (kill -9, SIGSTOP).
5. **Bot platform (3 days).** `bmbot`, `bmbot.h`, install rules, `chat_responder` on the ABI, `--check`,
   the heartbeat abort, Python and Go examples, Dockerfile and compose, `docs/bot-platform.md`,
   `docs/chat-responder.md` updated, a 24 h soak.

The user can stop after any slice with a working app and a smaller Qt surface. One implementation
plan is written per slice, when the previous slice is done, because each slice depends on what the
earlier ones find.

## Testing

- **Golden vectors, taken from today's code before each port:** address from phrases and labels, the
  phrase key pairs, `decodeMessage` / `decodeBroadcast` / `decodePubkey` of fixed ciphertexts and
  objects, the deterministic parts of every encoder, `inventoryHash`, `trialValue`,
  `ProofOfWork::valid`, chat JSON in and out (with the invalid cases of `parseChatMessage`), vault v1 and
  v2 files, mailbox files at every schema version the migrations know (migration tests). Stored under
  `tests/golden/`, with the small program that made them kept in `scripts/`.
- **Cross-implementation, while both exist:** objects and files written by the new code are read by the
  old code and the other way round. Because the veneer replaces the old code, this runs as a manual
  gate, `scripts/compat-check.sh`: it builds the commit before the slice in a worktree and opens each
  one's vault and mailbox with the other.
- **Existing suites** are the regression net for the veneer.
- **Scenario tests, Qt-free:** chats end to end between two `Client`s (queued, ack and message proof of
  work, awaiting ack, acknowledged), expiry, retry, restart with unread chats (at least once), rate
  limits, bad input; they replace the `Session`-based `chatbot_tests`.
- **`bmbot`:** the ABI called from C, from Python ctypes (CI) and from Go cgo (when `go` is present),
  two bots chatting over a private node link (`-P` each other), a SIGKILL of the node, a SIGSTOP of the
  node, a stalled driver (abort), `bmbot_close` while another thread waits in `bmbot_next`.
- **Qt-freedom:** the Qt-less container build; `nm -D`/`ldd` on `libbmbot`, `bmnode`,
  `chat_responder` show no Qt.
- **Performance:** the scan throughput (objects per second against ten identities) measured before and
  after slice 3.
- **Mutation checks** on the risky logic (vault serialization, at-least-once inbox, rate limits).

## Acceptance

- A fresh Debian container with no Qt builds and runs `chat_responder`; the image holds no Qt file
  and its size is recorded.
- The app builds on Linux, macOS and Windows (CI), passes its suite where it does today and behaves
  as before. A vault and a mailbox made by the previous release open in the new one, and the previous
  release opens what the new one wrote (`compat-check.sh`).
- The data folder `chat_responder` has today (the default public phrase, and a secret phrase) keeps
  opening with the same address and layout.
- A live chat between the app (new build) and a containerized responder works with the delivery
  states the app shows today.
- The Python and Go examples run.
- A 24 h soak of the container: the peers never stay at 0, memory is flat, the watchdog's actions are
  logged, and a `docker kill` is survived with the same address.

## Risks

1. **Vault bytes.** `QDataStream` is Qt's format. Mitigation: `qtstream` limited to what the vault uses,
   golden v1 and v2 files, the cross-open gate.
2. **Unicode.** UTF-16 rules become UTF-8 rules. Mitigation: the `grep` list and tests above; a chat
   answer is cut at a code point, not at a byte.
3. **The veneer is large and mechanical.** The compiler finds most mistakes and the existing suites
   exercise the rest.
4. **Windows.** MSVC C++20 and path encoding in `std::filesystem` are checked only by CI. Mitigation:
   a `bmc::path()` helper, no POSIX call outside the supervisor.
5. **Threading.** One SQLite connection per `Mailbox`; `bmbot` serializes access; `message_search`
   keeps its own connection as now.
6. **Scope creep and time.** Slices have exit criteria and a stopping point. The estimate (11 to 15
   working days) is a guess that slice 2 will most likely move.
7. **Two spellings of one thing during a slice** (old and new code): kept short by deleting the old
   implementation at the end of each slice; the golden files outlive it.

## Out of scope

- A Windows `bmbot` and node supervisor; message search, UPnP and GPU in the core; `KeySearch`
  threads in the core; broadcast, chan and contact functions in `bmbot` v1.
- A daemon speaking JSON over a socket on top of `libbmbot` (about a day, later, if wanted).
- Python wheels, a Go module, other packaging.
- Changing the GUI code to use `bmc` types.

## Later

- `bmbot` v2: broadcasts and chans, subscriptions, contacts.
- The JSON-over-socket daemon.
- Windows.
- A canary: a second bot that `--hello`s the first and a monitor that waits for the answer.

## Notes after slice 1

What slice 1 settled that the design left open, for the plans of the next slices:

- **Layout.** Headers are `bmclient/include/bmc/*.h` (included as `<bmc/...>`), sources `bmclient/src/*.cpp`, tests `tests/bmclient/`. The Qt wrappers keep their file names in `src/` and convert through `src/qt_bridge.h`.
- **`IdentityRef`.** The codec takes `bmc::IdentityRef`, a borrowed view of an identity (address, chan flag, the 64 key bytes), so a wrapper does not copy keys per call. `bm::bridge::CoreIdentity` makes one from a `bm::Identity`.
- **Update notices.** `bmc::updates` holds the pure functions and the two addresses. The default subscriptions with their translated label, and the setting keys, stay in `src/updates.{h,cpp}` (lupdate and `scripts/check-translations.py` read that file); slice 3 decides whether `Delivery` needs them in the core.
- **`Wire::encodeMessageText`.** A msg whose text is given as bytes; `encodeMessage` is it with `Subject:..\nBody:..`. Text that is not UTF-8 is refused when sending (it cannot come from the Qt app); when reading, it is decoded the way `QString::fromUtf8` did (a leading BOM dropped, one U+FFFD per byte of an invalid sequence).
- **Qt behaviours measured (Qt 6.8.3) and reproduced**: `QString::fromUtf8` (above), `QChar::isSpace`, `QString::toInt`, and `QJsonDocument::fromJson` with its leniencies (raw control characters and unknown escapes in strings, `.5` and `5.` as numbers, lone surrogate escapes become `?`, last duplicate key wins, a leading BOM is skipped, depth limit 1024, underflow to zero is an error). `tests/bmclient/parity/*_test.cpp` run Qt beside the core and stay while the Qt app exists.
- **Parity method.** `scripts/legacy-snapshot.sh <commit>` copies the old sources of a slice into `tests/legacy` under the namespace `legacy` behind plain types (`oracle::`); the parity tests ran old and new side by side; the answers were frozen into `tests/golden` and the snapshot deleted. Slices 2 to 4 can repeat it (the script lists slice 1's files; change the list).
- **The error catalogue** (`scripts/update-error-catalog.py`) reads `bmclient/src/*.cpp` too. Messages that must reach the translators are written as `require(cond, "text")` or `std::runtime_error("text")`; a new message that only a bot can meet is thrown as `bmc::Error(Code, "text")`, which the script does not list.
- **CI.** A new job `core-no-qt` in `.github/workflows/release.yml` builds and tests `-DYNOTBIT_QT=OFF` on a plain Ubuntu runner. It is not among the `needs` of the release job.

## Notes after slice 2

What slice 2 settled: the storage layer moved into the core, and the bytes stayed the previous release's.

- **`qtstream`.** The vault payload is a `QDataStream` at version `Qt_6_0`, and `bmc::qtstream` is that subset and nothing else: a `quint32` is big-endian, a string is a `quint32` **byte** length and then UTF-16BE, a null string is `0xFFFFFFFF` (read as empty, which is what a core `std::string` can hold), a bool is one byte, raw bytes are as given. A probe program first confirmed each of those against Qt 6.8.3; `tests/golden/qtstream.txt` now freezes Qt's own bytes for the corners (a 255-character string, 85 euro signs, non-BMP text, a null string, a lone surrogate, the `quint32` corners).
- **The golden files.** `tests/golden/vault-*.bmvault` and `mailbox-*.bmmail` are a vault and a mailbox the previous release wrote: one identity, one mailbox key, letters, a draft, a chat, a subscription, a contact and a public key. The vault's and the mailbox's tests read them; the v1 pair also covers the migration and the `[chan] label` rename.
- **The compat gate.** `scripts/compat-check.sh` builds the pre-slice commit in a worktree, puts `scripts/compat-open.cpp` (a Qt program using that tree's real `bm::Vault` and `bm::Mailbox`) beside its tests, and cross-opens: the core writes, the previous release reads; the previous release writes, the core reads; and both read the golden files. Each direction diffs the two reports of the same files. The plan had an in-tree snapshot of the old sources behind an `oracle::` namespace for the same purpose; the gate does it with the previous release's own code, so the snapshot was not built. `scripts/legacy-snapshot.sh` from slice 1 is not in the tree any more: plan the next slices against the gate rather than the snapshot.
- **Two SQLite helpers now.** `bmc::Statement`/`bmc::Transaction` (`bmclient/include/bmc/sqlite.h`) are the core's; the Qt `src/sqlite_helpers.h` stays for `src/message_search.cpp`, which keeps a connection of its own. A core `Statement` binds an empty string or blob as an empty value and never as NULL, as Qt's `QByteArray::constData()` did, because the columns it lands in are NOT NULL.
- **Records kept Qt in the veneer.** `bm::Identity`, `bm::Message`, `bm::ChatMessage`, `bm::ChatSummary`, `bm::OutboxItem`, `bm::NetworkJob`, `bm::StoredPublicKey`, `bm::Subscription`, `bm::Contact` and `bm::DeliveryEvent` remain the app's own structs with `QString`/`QByteArray` fields; the core has `bmc::` twins, and `src/qt_bridge.h` converts. `bm::Mailbox` holds a `std::shared_ptr<bmc::Mailbox>` (so slice 3's `Session` can hand the same one to a `Client`) and every body is a conversion.
- **Deviations that carried meaning.** `bmc::Mailbox::updateJob` takes `const Bytes *payload = nullptr` for a payload that is absent, because Qt's `QByteArray::isNull()` tells absent from empty and `bmc::Bytes` cannot. `bmc::nowMillis()` rounds to the nearest millisecond, as `QDateTime::currentMSecsSinceEpoch()` does: truncating left a chat's seqno a millisecond behind the Qt clock about half the time.
- **The error catalogue** picked up the vault's messages (they were missing from slice 1's run), and `scripts/check-translations.py` still passes with the 6 translations.
- **The container gate.** `scripts/check-qt-free.sh` builds SQLCipher 4.6.1 inside the container rather than installing Debian's `libsqlcipher-dev`: bookworm ships 3.4.1, whose SQLite has no `INSERT ... ON CONFLICT ... DO UPDATE` and whose key derivation is not the one of SQLCipher 4, so every mailbox test failed on the library rather than on the code. It builds it shared, not the static one the project ships, so the link line does not also need the private dependencies of SQLite; and it runs the build and the tests as an unprivileged user, because as root the permission bits do not bind and a test of a failed write would pass without testing anything. Reported: 5/5 ctest and `QT-FREE BUILD OK` on a machine with no Qt.
- **Qt on Linux.** Debian bookworm's Qt is 6.4.2 and the project requires 6.8, so the Qt suites on Linux ran on Ubuntu 25.04's Qt 6.8.3, the version the app ships: `chat`, `core`, `lifecycle`, `chatbot`, `outbox`, `delivery`, `bmclient` and `bmclient-parity` all pass there (8/8). The desktop and node suites were not part of that run.
- **Qt beside the core.** `tests/bmclient/parity/qtstream_parity_test.cpp` asks a live `QDataStream` at `Qt_6_0` what it writes, over the grid `tests/golden/qtstream.txt` freezes, so a change in Qt's own encoding is caught in the Qt build as well as by the frozen file.

## Notes after slice 3

What slice 3 settled: the cache, the delivery engine and the client loop moved into the core, and `Session` became a view over a `bmc::Client`.

- **The cache, the engine, the loop.** `bmc::Cache` (`bmclient/src/cache.cpp`) is `src/cache.cpp` with `std::string`/`bmc::Bytes` in place of `QString`/`QByteArray`; it opens the node's `objects.sqlite` with `sqlite3` under the file name and the `user_version` the node's own header (`ntb-object-db.h`) declares, and still removes the pre-0.5.2 `cache.sqlite` on construction. `bmc::Delivery` is `src/delivery.cpp`'s plan, proof of work, spool, receipts and scan: the job and delivery state names (`queued`, `submitted`, `ready`, `mining`, `awaiting_pubkey`, `calculating_message`, `calculating_ack`, `publishing`, `awaiting_ack`, `acknowledged`, `published`, `cancelled`, `failed`, `expired`), the detail lines the UI shows, and the `publish/`, `receipts/`, `objects/` layout are the ones the app and the node already used, so a mailbox and a node folder carry over as they are. `bmc::Client` owns the four through `shared_ptr` and runs one `step(now, online)`: bind the mailbox to the cache's id, note retention-pruned objects, scan with a time budget (512 objects, 150 ms), tick the engine, and report what it did (`StepReport`).
- **The time is an argument.** `Delivery::scan`/`tick` and `Client::step`/`sendChat`/`sendLetter` take `int64_t now`; the veneers pass `bmc::now()` (`src/delivery.cpp`, `src/session.cpp`) and a test passes its own, so the same scenario is reproducible. `Delivery::retry` keeps `bmc::now()` because it reaches back into the mailbox's own clock. The setting keys the engine reads are spelled once: `updates:notify` and `updates:latest` are `bmc::updates::kNotifySetting`/`kLatestSetting`, and `src/updates.h` derives the Qt keys from them, so a bot and the app cannot drift apart. The remaining keys (`scan:subscriptions`, `scan:catchup-from`, `scan:catchup-to`, `pubkey:<address>`, `draftkind:<id>`) keep the strings they had, in the core.
- **One conversion layer each.** `bm::Cache`, `bm::Delivery` and `bm::scanMailbox` keep their Qt signatures and hand every call to the core through `core()`. `bm::Vault` now holds its core through a `shared_ptr` too, as `bm::Mailbox` already did, so `Session` builds `Vault vault_{vaultCore_}` and `Mailbox mailbox_{mailboxCore_}` around the very cores its `bmc::Client` uses: one vault, one mailbox, one store, whichever side drives them. `bm::Cache`/`bm::Delivery`/`bm::scanMailbox` stay for `tests/lifecycle_tests.cpp` and `tests/delivery_tests.cpp`, which pin the veneer; nothing else constructs them any more.
- **Error text and the catalogue.** The messages `bmc::Client` and `bmc::Delivery` raise are listed in `src/i18n/error_catalog.cpp` and reach the UI through `Session::errorText`'s `QCoreApplication::translate("bm::Errors", ...)`. The catalogue gained six lines: the five the chat and letter services took over (`Choose a sender from this vault`, `Enter a valid Bitmessage recipient address`, `Chat message must be between 1 and 8192 bytes`, `Subject must be one line`, `Write a message before sending`) still live in the `.ts` files under `bm::Session`, where the `tr()` calls this slice deleted put them, so until the next `update_translations` refresh moves them to `bm::Errors` they read in English in the six non-English locales; the sixth is `Open a mailbox first`, which the wrapper still guards with its own `tr()` for the UI and which the client now also throws, for a bot that calls it with nothing open. `scripts/check-translations.py` passes as it stands. That refresh is a release-time job, not this slice's. One message had to keep a shape the script lists: the spool failure in `bmc::Delivery` is `require(false, "Cannot queue encrypted object for relay", Code::Io)`, because the extractor reads `require(...)`/`runtime_error(...)` text and a bare `throw Error(Code::Io, ...)` would have dropped the line from the catalogue. `Session::sendLetter` keeps the one refusal with UI wording of its own ("This letter is %1; Bitmessage carries at most %2..."); the client's "Letter is too large" is a bot's backstop, and the plan's rule was to fix the core, not the test, so the client's text is the exact string the catalogue lists.
- **Known gap, not fixed here.** The extractor still does not list a message thrown as `bmc::Error(Code, "text")`, and the core has about forty of those (`Mailbox is closed`, `Invalid hexadecimal text`, `Digest failed`, ...). They are as user-visible as the rest — `errorText` translates any exception — but listing them now would add forty new strings to the next translation pass, which is a decision for the slice that wants them translated.
- **The two-client test.** `tests/bmclient/scenario_test.cpp` runs two vaults, two mailboxes and two object stores over `tests/bmclient/fake_node.h`, which does the node's and the relay's work in the test: a letter goes A to B and its acknowledgment comes back, a chat is answered, a broadcast reaches a subscriber, and a second scan stores nothing twice. It runs on `bmc::setClockForTesting`, because the proof of work needs real seconds while the mailbox's own expiry checks read `bmc::now()`: with a wall clock, a slow proof of work could expire the letter the test is waiting on. `keepKeyOf()` seeds the recipient's public key where the round trip is not the point of the test, so the suite does not pay for a 28-day proof of work four times.
- **Deviations that carried meaning.** Task 2's case list landed under different names and, twice, in different files: the two-mailbox loop is `a_letter_reaches_the_other_side_and_its_acknowledgment_comes_back`, cancel and retry are one case (`a_cancelled_letter_loses_its_jobs_and_a_retry_queues_it_again`), the broadcast case also covers the oversized object in front of it, and the double-store case is `scenario_test.cpp`'s `a_second_scan_stores_nothing_twice`. Task 3's four `a_step_*` cases landed as `a_step_reports_nothing_until_a_mailbox_is_open`, `a_step_reads_the_store_and_then_reports_being_up_to_date`, `a_step_reports_objects_retention_removed` and `a_step_reports_the_mailbox_changing_under_it` (which now covers the outbox write as well as the scan's), and the poll case as `a_chat_from_the_network_is_a_service_a_bot_can_poll`. The spool failure is `require(false, ..., Code::Io)` rather than a bare `throw Error(Code::Io, ...)` for the catalogue reason above. The plan's Review Focus section now names the tests that exist.
- **Deliberately left uncovered.** Slice 2's list stands (SQLCipher-init/EVP/allocation failures, `protocol.cpp`'s post-decryption decode branches, the pow solver's races, `fwrite`/`fchmod` failures, `migrate()`'s pre-release `ALTER`s). Slice 3 adds: the `try`/`catch` backstops that turn a broken database into an empty chat list (`Client::chatState`, `unreadChats`, `markChatMessageRead`), and a filesystem that refuses the spool write, which `Delivery` still converts into the same English message it always did. (`Cache`'s `unreadable()` and the `pruned` report are covered: `a_store_that_is_not_a_store_reads_as_empty` and `a_step_reports_objects_retention_removed`.)
- **For slice 4.** The node half is the next move. `bm::NodeWatch`'s judgement (`src/node_watch.h`) is already Qt-free in everything but its `qint64`s; `Session`'s node members (`QProcess node_`, `nodeLock_`, `listenPort_`, `lastNodeStatus_`, `restartSoon_`, `nodeWatch_`, `watchdogNote_`, `watchdogRestarts_`, `stoppingNode_`, `nodeRestarts_`), `nodeArguments()`, `validEndpoint`, `configuredPort()` and `retentionMB_`/`retentionDays_` are what a `bmc::NodeConfig`/`NodeStatus`/`NodeSupervisor` takes, and the `status.json` parse in `Session::tick` moves with them. `src/relay_bridge.cpp` and the `--node` re-exec are untouched so far. `bmc::scanMailbox` has no production caller (only `tests/lifecycle_tests.cpp`), so the older direct-message-only scan can be retired with that test. `bmc::nowMillis()` still has no test seam, and `bm::updates::defaultSubscriptions()` still lives in the Qt veneer because its label is translated; both are deliberate.

## Notes after slice 4

What slice 4 settled: the node — its settings, its status file, its watchdog, its process and its relay bridge — is in the core, and `bmnode` is the node on its own.

- **The desktop still starts itself.** `Session` starts `QCoreApplication::applicationFilePath()` with `--node`, as it always did, so the program a user runs, the program the desktop suites run and the program `tests/relay_test.py` drives are one binary; `bmnode` is that node for the places that have no app to start: the container (`scripts/check-qt-free.sh`), the bot library of a later slice, and the node tests in the Qt-free tree. `bmnode` strips a leading `--node` and calls `ntb_daemon`, so one argument list, one `YNOTBIT_TEST_NODE_FLAG` (`--node`) and one set of scripts serve both: the five Python node tests and the two retention checks are registered twice in `CMakeLists.txt`, once against `$<TARGET_FILE:ynotbit>` and once against `$<TARGET_FILE:bmnode>`. `find_package(Python3)` stays optional in the Qt-free part: without it those tests simply do not exist, and the container has no Python.
- **What moved and what stayed in `Session`.** `bmc::NodeConfig` owns the settings→argv mapping, `bmc::NodeStatus` the `status.json` keys, `bmc::NodeWatch` the judgement and `bmc::NodeSupervisor` the process. The timers that decide *when* stayed: the 750 ms `tick()`, the 1 s `restartSoon_` that coalesces a settings change, the 30 s start after an unasked exit, and the watchdog's 120 s / 10 s grace / 300 s spacing. The three `QProcess` connections became `pollNode()`, called once a tick: it reads the last stderr line the supervisor kept and, on the first look after an exit, names it either way — `nodeError_` (what `status()` shows) is the last line, `error_` is `Node: ` and the trimmed chunk, which is the split the two signals had before.
- **The supervisor's semantics.** `posix_spawn` with `POSIX_SPAWN_SETPGROUP`, so the node and everything it starts are one group; `poll()` reaps with `waitpid(WNOHANG)` and returns true exactly once per unasked exit, so `restartNode()` is not reported as a crash; `stop()` sends SIGTERM to the group, waits up to 3 s for the child *and* for the group (`kill(-pid, 0) == ESRCH`), then SIGKILLs the group and reaps for up to 1 s — a node that ignores SIGTERM, or a grandchild that does, still goes. `start()` reaps first and is a no-op when the node is already running, `restart()` is `stop()` then `start()`, and `YNOTBIT_PARENT_PID` goes in an explicit environment, which is what the relay bridge's `parentGone()` reads: a node whose app died stops on its own rather than lingering. `src/node_supervisor_windows.cpp` mirrors all of it (a job object, `TerminateProcess`); it is build-verified only.
- **Restart, unlike the plan said.** `restartNode()` calls `stop()` and then `startNode()` rather than the supervisor's `restart()`: the latter would start a node while `offline_` is set, and would skip what `startNode()` does — remove `status.json`, clear the last status read and the last stderr line, and tell the watchdog the node was started again. The plan says so now.
- **The argv and the endpoints are goldens.** Task 0 measured what the app builds today, on the app, and the two tables are frozen in `tests/bmclient/node_config_test.cpp` — eight configurations, order `--node -D -m -i -R -A -o -n -p -P -L -r -B`, and forty-one endpoint strings. Two deviations from the plan's table were found by the probe and are now the rule: a proxy adds `-i` as well as `-r ... -B` (the proxy would publish the real address the `-i` suppresses), and `validEndpoint` accepts a trailing space or tab or newline where Qt refused it, because the core trims both sides while `QUrl` trimmed the front only. Nothing reaches it untrimmed (`SettingsPane` and `saveEndpoint` trim first), so the superset is invisible; the test says which strings differ and why. The `-p` quirk is kept: whether the option is added is the raw `listenPort > 0`, its value is the clamped port, so a settings file saying 65536 gives `-p 8444`.
- **The relay bridge exists twice.** `bmclient/src/relay_bridge.cpp` is the std one, `bmnode`'s, with `relay_api.h` copied beside it; `src/relay_bridge.cpp` and `src/relay_api.h` stay the app's, because `announce_release`, `post_digest` and `chat_responder` re-exec themselves with `--node` and link the Qt one. The std copy is compiled into the `bmnode` target and deliberately not into the `bmclient` library: it defines a symbol the node engine calls (and provides the `main`-side hooks), so in the library every consumer of the core would link the engine and carry a node it does not run. Slice 5 moves the tools and deletes the Qt copy.
- **A Linux-only build break, found by the gate.** `Session::unreadChats` handed a core `int64_t` (`bmc::records.h`) straight to `QVariant`, which has six integer constructors: on macOS `int64_t` is `long long` and matches `QVariant(qlonglong)` exactly, on Linux it is `long` and every candidate is a conversion, so GCC calls it ambiguous. It went unnoticed because it only became reachable when `chatbot_tests` took `session.cpp` (and `Session::unreadChats`) into itself, and because the Linux Qt gate of slices 2 and 3 ran a subset that did not include the app target. One explicit `qint64(...)` fixes it; the whole app and all its tests now build on Linux for the first time.
- **Deliberately left uncovered.** The Windows unit (build-verified only), a spawn that fails between `fork` and `exec` (the bad-`bin` test covers `ENOENT`), a child that dies while `stop()` is between SIGTERM and SIGKILL, a negative `YNOTBIT_PARENT_PID`, and `NodeStatus::fromFile` on a file that is being replaced underneath it.
- **A review found the node's log arriving as its errors.** The supervisor's first version put the child's stdout and stderr on one pipe, which is what a `pipe()` and two `dup2`s invite. `ntb-daemon.c` sends the node's log to `/dev/stdout`, so every line of it ("Loading saved object store") became `standardError()` and then `error_ = tr("Node: %1")` in the window — text the app never showed, because `QProcess` kept the two channels apart and `Session` read the stdout one only to drop it. There are two pipes now: stdout is drained and dropped (drained, not ignored — a pipe nobody reads fills at 64 KB and the node would block on a line of its own log), stderr is what the app shows. `tests/bmclient/node_supervisor_test.cpp` pins both halves: `what_it_logs_is_not_what_it_says` (the complaint first and the log line last, so a merged stream answers with the wrong one) and `what_it_logs_is_drained_even_though_it_is_dropped` (4000 log lines, far past a pipe's worth, and the child must still reach its last line). The Windows unit had the same defect in its `STARTF_USESTDHANDLES`; its stdout goes to the NUL device. The reading has to cover the way out as well: `stop()` waits up to three seconds for a node it has just sent SIGTERM to, and a node still writing its log blocks on a pipe nobody reads — so the `stop()` that is waiting for it is what kills it. The waits drain as they go, which is what `QProcess::waitForFinished()` did for the app. `a_child_that_logs_its_way_out_is_not_killed_for_it` is that node: 4000 lines on SIGTERM, then a line on stderr and a clean exit. It spends the whole three seconds and ends in a SIGKILL if the waits do not read.
- **The status and receipt files are Qt's bytes, not only Qt's values.** `QJsonDocument(obj).toJson()` defaults to the indented form — four spaces a level, `": "` between a key and its value, and a trailing newline — and the first version of the std bridge wrote compact JSON. The constraint is the files' bytes, so `jsonObject()` in `bmclient/src/relay_bridge.cpp` writes exactly Qt's, in `QJsonObject`'s sorted key order. Checked with a probe that parses a node's `status.json` and a `receipts/*.json` with a live `QJsonDocument` and re-emits them: byte-identical, 333 and 132 bytes.
- **The review's coverage finding, closed.** The spool's id filter (`^[a-f0-9-]{36}$`) and its symlink refusal had no test: the Python node tests published valid ids and rejected a bad object *body*, so those two skip rules were only read side by side in the two bridges. `tests/relay_test.py` now spools a name of a job id's length in the wrong alphabet and a symlink pointing at a real object, waits until two more ticks have gone by, and finds neither a receipt nor a removal. The app's Qt bridge runs the same case, so both are held to the rule together.
- **What the review left standing, as differences that stay.** The stderr line and chunk are cut at 200 and 400 *bytes* where the app cut 200 and 400 *QChars*, so a multibyte error line can be cut mid-character, and the trimming around them is ASCII whitespace where `QString::trimmed()` also stripped the Unicode spaces (U+00A0, U+2000–U+200A, U+3000), so a line ending in one would keep it; a child that writes the same line twice running is not shown again (the same text, so nothing changes on screen); and a `status.json` that exists but cannot be parsed reads as "no news yet" and shows "Node starting" where the app, having read an empty object out of it, showed "Node status unavailable" (the relay replaces the file atomically, so this takes a disk fault). `crashed()` after a `stop()` this side asked for is a signal death on POSIX — a SIGKILL of a node that ignored SIGTERM reads as a crash — and nothing reads it, since `poll()` is false after a stop; the Windows unit calls an NTSTATUS exit code a crash, which is what it can tell, rather than reporting every deliberate stop as one — a program that returns `-1` lands in that range and is called one too; and `stop()`'s group kill is one `kill` wide from a pid that has just been reaped, which is written down where it happens.
- **For slice 5.** `announce_release`, `post_digest` and `chat_responder` still re-exec themselves with `--node` and still link `src/session.cpp` with `src/relay_bridge.cpp`; when they use `bmnode` (or `bmbot`) instead, the Qt bridge and `src/relay_api.h` are deleted and `Session` stops being compiled into four executables. `bmc::scanMailbox` still has no production caller (only `tests/lifecycle_tests.cpp`), `bmc::nowMillis()` still has no test seam, and `bm::updates::defaultSubscriptions()` still lives in the veneer because its label is translated — all three as slice 3 left them. `bmnode` and `bmbot` share a shape worth keeping: the engine's `main` plus `bmc`'s bridge, and the same `--node`-tolerant argv.
