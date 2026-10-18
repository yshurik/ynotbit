# Qt-free core, slice 4: the node — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `bmclient` holds the node half of the app. `bmc::NodeConfig` builds the argv the node is started with, `bmc::NodeStatus` reads its `status.json`, `bmc::NodeWatch` judges a node that runs but does no good, and `bmc::NodeSupervisor` starts it, watches it, stops it (SIGTERM, then SIGKILL after three seconds) and restarts it. `bmnode`, a new Qt-free executable, runs `ntb_daemon` with a `std` relay bridge, so the node needs no Qt at all: the container, a bot and the tests run it, and its `status.json`, spool and receipts are the ones the app's node already writes. `Session` keeps the `QTimer`s and the translated strings and drives the core for everything else.

**Architecture:** The same method as slices 1 to 3, with one new shape. The parts of `Session` that talk to the node process move to `bmclient` as a **move**: `src/node_watch.h` becomes `bmc::NodeWatch` (`qint64`→`int64_t`), `Session::nodeArguments`/`configuredPort`/`validEndpoint`/`wantsPortMapping` become `bmc::NodeConfig` and its one function, the two `status.json` reads in `Session::status()` and `Session::tick()` become `bmc::NodeStatus`, and `QProcess node_` (with its `finished`/`errorOccurred`/stderr handlers, the 30 s restart and the 1 s `scheduleRestart` coalescing) becomes `bmc::NodeSupervisor`, a POSIX `posix_spawn` child in its own process group with a `waitpid` poll, plus a Windows translation unit. What is **new** is `bmnode`: `third_party/notbit`'s `ntb_daemon` needs the symbol `ynotbit_relay_tick`, which `src/relay_bridge.cpp` provides in Qt today; `bmclient/src/relay_bridge.cpp` provides it in `std`, and `bmnode/main.cpp` is a `main` that calls the daemon.

**Tech Stack:** C++20, std only, plus OpenSSL, libsodium, SQLCipher, libsecp256k1 and the notbit C code; `posix_spawn`/`waitpid`/`kill` on POSIX, `CreateProcess` and a job object on Windows; the Qt 6.8 app (`bmcore`) links `bmclient`.

**Spec:** `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md`, "Order of work" item 4 and the section "The node". Slice 3's plan ends with "Notes for slice 4"; this plan is the execution of those notes.

## Global Constraints

Copied from the spec; every task includes them.

- Nothing in `bmclient/` or `bmbot/` includes or links Qt; checked by `scripts/check-qt-free.sh` in a Qt-less container. The one Qt-free target this slice adds is `bmnode`.
- The core is C++20, std only, plus OpenSSL, libsodium, SQLCipher, libsecp256k1 and the notbit C code. **`bmc::NodeSupervisor` is the one place in the core that calls the operating system** (`posix_spawn`, `waitpid`, `kill`, `pipe`; `CreateProcess` and a job object on Windows). Everything else in the core still compiles on Linux, macOS and Windows/MSVC unchanged.
- Bytes and text: `using Bytes = std::vector<uint8_t>`; text is UTF-8 `std::string` / `std::string_view`; integers are `int64_t` / `uint64_t`. The Qt veneer is the only place with `QString`, `QProcess` and `QJsonObject`.
- Errors: `bmc::Error : std::runtime_error` with the exact English `what()` of today; no localized text in the core; `Session::errorText` translates `what()` in the `bm::Errors` context; `scripts/update-error-catalog.py` reads `src/*.cpp` (except `session.cpp`, `desktop_window.cpp`, `main.cpp`) and `bmclient/src/*.cpp`.
- Time: seconds since the epoch as `int64_t`; code that can be handed the time takes `now`.
- **On-disk formats do not change.** The node's `objects.sqlite`, the app's `status.json`, the spool `publish/<job>.object`, the relay's `receipts/<job>.json` and `desktop.ini` keep their names, their keys and their bytes. The node's argv is byte-identical for every configuration.
- **The desktop's behaviour does not change.** `Session` starts the same binary (`QCoreApplication::applicationFilePath()`) with the same arguments, restarts it under the same rules, and shows the same strings.
- Work happens on `feature/qt-free-core`; core tests live in `tests/bmclient/`.
- Nothing is committed or pushed unless the user has said so. Never touch, stop or replace the `ynotbit` processes and the `~/w/ynotbit` copies that are already running: every test this slice adds spawns its own children in its own temporary directory, on loopback or with no socket at all.

## Review Focus

The inputs the spec implies but a plain port could get wrong. Each line has a test in the task named in brackets.

1. **The node's argv is the one the app builds today.** `Session::nodeArguments()` reads `desktop.ini` and produces `--node -D <root> -m <root>/unused-maildir`, `-i` when not listening, `-R <mb> -A <days>`, `-o`/`-n` only when set, `-p` only when set, `-P <peer> -L`, `-r <proxy> -B`; listening is off while a proxy is set. `bmc::nodeArguments(NodeConfig)` must produce the same vector for every configuration, and `Session::nodeArguments()` must be it. [Task 2: `the_arguments_are_the_ones_the_app_builds` (a frozen table taken from today's Qt code in Task 0); Task 8: the unchanged `peer-connectivity`, `relay`, `listen-fallback` and `desktop` suites]
2. **The relay bridge's behaviour on disk does not change.** `status.json` keeps its keys (`peers`, `pending`, `time`, `objects`, `object_bytes`, `established_outgoing`, `pending_outgoing`, `established_incoming`, `known_addresses`, `eligible_addresses`, `attempts`, `setup_timeouts`, `rejections`, `listen_port`) — and its bytes, indentation and trailing newline included — the spool keeps its eight-object cap, its `^[a-f0-9-]{36}$` id filter and its symlink refusal, and a receipt keeps its three states (`rejected`, `offered`, `accepted`) and the rule that the spool file is removed only when the object was accepted and peers exist. [Task 6: `relay`, `relay-verack-first`, `relay-download` against `bmnode` — the keys, the cap, the states and the removal rule. `relay_test.py` also puts a name of a job id's length in the wrong alphabet and a symlink pointing at a real object into the spool, waits two ticks, and finds neither a receipt nor a removal; the app's bridge runs the same case and passes it]
3. **A node is stopped the way `QProcess::terminate` plus `kill` stopped it, and only its own process group.** `stop()` sends SIGTERM, waits up to three seconds, then SIGKILL, and the child is in a process group of its own so a helper it forked dies with it. A stop of a child that is already gone is harmless, and no zombie is left behind. [Task 4: `a_child_that_ignores_sigterm_is_killed`, `a_child_that_exited_is_not_left_behind`, `its_children_die_with_it`]
4. **The watchdog's judgement is unchanged.** `bmc::NodeWatch` is `bm::NodeWatch` with `int64_t` in place of `qint64`: the hung grace, the "stale for a while, not for one tick" rule that a Mac waking from sleep needs, the lonely rule, and the spacing between two restarts. Its assertions move from `tests/core_tests.cpp` into the core harness without losing one. [Task 3: `node_watch_test.cpp`]
5. **The app's Qt surface shrinks and its tests do not know.** After Task 7 `Session` has no `QProcess`, no `QJsonObject` status read, no endpoint parsing of its own and no `src/node_watch.h`; the strings it shows ("Node stopped: %1 · outgoing objects stay queued", "Node starting · connecting to peers", "port %1 is taken, listening on %2", the watchdog notes) are the ones it showed before, `tr()` for `tr()`. [Task 7: the unchanged `desktop` (it writes its own `status.json` into a temporary node folder and reads the object count back), `lifecycle` and `chatbot` suites; Task 8: the whole Qt `ctest`]

## Execution rules

- Run every command from `/Users/user/w/ynotbit`. In each shell first: `export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig`.
- Build directories: `build` (the existing Qt build, Release, Ninja) and `build-core` (Qt-free).
- Every new or changed C++ file is formatted with `clang-format -i <file>`. Code blocks are close to that style, not exact.
- **Commit steps run only if the user has allowed commits on this branch.** Commit messages are Conventional Commits with a prose body and end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- Never stage or commit `docs/superpowers/plans/2026-10-04-sqlite-object-store.md`, `docs/superpowers/plans/2026-10-15-qt-free-core-slice-1.md`, `docs/superpowers/work-state-2026-10-16.md`, `reports/` or `research_notes/`.
- A plan test that fails: fix the core, not the test, unless this plan says the test was wrong.
- The Mac stalls under swap pressure: keep tool calls short; build with `--parallel 4`; run the Qt and core `ctest`s in sequence, not together, and never beside `compat-check.sh`.
- Every test that starts a process starts **its own** process, in its own temporary directory, and stops it in a way that cannot reach a process it did not start. No test contacts a public peer; the node tests use loopback and `-i` (no listening) where they can.

## What is a move, what is new

**Moves** (behaviour preserved to the byte):

| Today | This slice |
| --- | --- |
| `src/node_watch.h` (`bm::NodeWatch`, `qint64`) | `bmclient/include/bmc/node_watch.h` (`bmc::NodeWatch`, `int64_t`) |
| `Session::nodeArguments()`, `configuredPort()`, `validEndpoint()`, `wantsPortMapping()` | `bmc::NodeConfig::nodeArguments()`, `listenPort()`, `bmc::validEndpoint()`, `bmc::wantsPortMapping()` |
| the `status.json` read in `Session::status()` and `Session::tick()` | `bmc::NodeStatus::fromFile()` |
| `QProcess node_`, its three handlers, `startNode()`, `restartNode()`, the stop in `~Session` | `bmc::NodeSupervisor` (`start`, `poll`, `stop`, `restart`) |
| `src/relay_bridge.cpp` (Qt) | `bmclient/src/relay_bridge.cpp` (`std`) — **both exist after this slice** (see below) |
| the `NodeWatch` assertions in `tests/core_tests.cpp` | `tests/bmclient/node_watch_test.cpp` |

**New:** `bmnode`; `bmc::NodeStatus` as a value; the supervisor's process group and its Windows translation unit; `tests/bmclient/node_supervisor_test.cpp`; the `bmnode`-backed `ctest` entries.

**What stays, and why.** `Session` keeps starting *itself* with `--node` (`QCoreApplication::applicationFilePath()`), so the desktop's node is the same binary, started with the same argv, as today; `src/relay_bridge.cpp` therefore stays, and `bmnode` is the Qt-free node for the container, the bot library and the tests. The Qt bridge is deleted in slice 5, with the tools that still re-exec themselves (`announce_release`, `post_digest`, `chat_responder`). `bmnode` accepts and ignores a leading `--node` so that one argv — and one `YNOTBIT_TEST_NODE_FLAG` — serves both binaries.

## File structure

New in the core:

- `bmclient/include/bmc/node_config.h`, `bmclient/src/node_config.cpp` — the node's settings and the argv they make.
- `bmclient/include/bmc/node_status.h`, `bmclient/src/node_status.cpp` — the `status.json` read.
- `bmclient/include/bmc/node_watch.h` — the judgement (header only, as `src/node_watch.h` is).
- `bmclient/include/bmc/node_supervisor.h`, `bmclient/src/node_supervisor_posix.cpp`, `bmclient/src/node_supervisor_windows.cpp` — the process.
- `bmclient/src/relay_api.h` — the notbit entry points the bridge calls (a copy of `src/relay_api.h`, which stays for the Qt bridge).
- `bmclient/src/relay_bridge.cpp` — `extern "C" void ynotbit_relay_tick(...)`, in `std`.
- `bmnode/main.cpp` — the executable.
- `tests/bmclient/node_watch_test.cpp`, `node_status_test.cpp`, `node_config_test.cpp`, `node_supervisor_test.cpp`.

Changed: `bmclient/CMakeLists.txt`, `tests/bmclient/CMakeLists.txt`, `CMakeLists.txt` (the `bmnode` target and its tests), `src/session.h`, `src/session.cpp`, `src/node_watch.h` (deleted), `tests/core_tests.cpp`, `cmake/qt-app.cmake` (the two retention tests against `bmnode`), `bmclient/README.md`, the design spec.

## Task 0: Preconditions and baseline

**Files:** none. This task writes nothing but notes to itself.

- [ ] **Step 1: Confirm the branch and a clean tree.**

```sh
cd /Users/user/w/ynotbit && git status --short && git log --oneline -1
```
Expected: HEAD is on `feature/qt-free-core`; the only untracked entries are `docs/superpowers/plans/2026-10-04-sqlite-object-store.md`, `docs/superpowers/plans/2026-10-15-qt-free-core-slice-1.md`, `docs/superpowers/work-state-2026-10-16.md`, `reports/`, `research_notes/`.

- [ ] **Step 2: Record the baseline.** Slice 3 left 136 core tests (5 `ctest` entries) and 29 Qt tests. Run both and write down the numbers.

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build --parallel 4 && (cd build && ctest --output-on-failure | tail -3)
cmake --build build-core --parallel 4 && (cd build-core && ./tests/bmclient/bmclient_tests | tail -1)
```
Expected: `100% tests passed, 0 tests failed out of 29` and `136 run, 0 failed`.

- [ ] **Step 3: The argv the app builds today — done, and frozen here.** `core_tests` does not link `Session` (`src/session.cpp` is compiled into each executable, not into `bmcore`), so the probe went into `desktop_tests` instead: a temporary `--node-args` branch in `tests/widgets_tests.cpp` that writes a `desktop.ini`, constructs `Session(root, /*offline=*/true)` — offline matters, online the constructor starts a node — and prints `nodeArguments().join(' ')`. The Qt answers are the table below; Task 2 freezes them in the core test, and the probe is removed again (both done).

| `desktop.ini` | argv, with `<root>` for the node folder |
| --- | --- |
| (empty) | `--node -D <root> -m <root>/unused-maildir -R 2048 -A 90` |
| `listen=false` | `--node -D <root> -m <root>/unused-maildir -i -R 2048 -A 90` |
| `listenPort=8445` | `--node -D <root> -m <root>/unused-maildir -R 2048 -A 90 -p 8445` |
| `maxOutgoing=16, maxIncoming=4` | `--node -D <root> -m <root>/unused-maildir -R 2048 -A 90 -o 16 -n 4` |
| `peer=127.0.0.1:8444` | `--node -D <root> -m <root>/unused-maildir -R 2048 -A 90 -P 127.0.0.1:8444 -L` |
| `proxy=127.0.0.1:9050` | `--node -D <root> -m <root>/unused-maildir -i -R 2048 -A 90 -r 127.0.0.1:9050 -B` |
| `retentionMB=64, retentionDays=1` | `--node -D <root> -m <root>/unused-maildir -R 64 -A 1` |
| all of the above | `--node -D <root> -m <root>/unused-maildir -i -R 128 -A 7 -o 32 -n 16 -p 9000 -P 127.0.0.1:8444 -L -r 127.0.0.1:9050 -B` |

The order is the order `Session::nodeArguments()` appends: `--node`, `-D`, `-m`, `-i`, `-R`, `-A`, `-o`, `-n`, `-p`, `-P`, `-L`, `-r`, `-B`. Two details a rewrite would get wrong: a proxy turns listening off, so `-i` appears beside `-r`; and `-p` carries `configuredPort()` (clamped to 1..65535) while the test for it is the raw `listenPort > 0`, so `listenPort=65536` still passes `-p 8444`.

- [ ] **Step 4: Note the pre-slice commit.**

```sh
git rev-parse HEAD
```
Expected: that hash is `SLICE4_BASE`, used by the compat gate in Task 8.

## Task 1: `bmc::NodeStatus` — the status file

**Files:**
- New: `bmclient/include/bmc/node_status.h`, `bmclient/src/node_status.cpp`
- New: `tests/bmclient/node_status_test.cpp`

- [ ] **Step 1: Write the header.** The keys are the ones `relay_bridge.cpp` writes; the "older than ten seconds is unavailable" rule is the one `Session::status()` applies.

```cpp
// The node's status.json, as the app reads it: the counters it shows and the port it ended up
// listening on. A file that is missing, half-written or foreign reads as nothing known.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bmc {
struct NodeStatus {
    int64_t time = 0;               // the "time" the node wrote it
    int peers = 0, pending = 0;
    int64_t objects = 0, objectBytes = 0;
    int establishedOutgoing = 0, pendingOutgoing = 0, establishedIncoming = 0;
    int knownAddresses = 0, eligibleAddresses = 0;
    int64_t attempts = 0, setupTimeouts = 0, rejections = 0;
    int listenPort = 0;             // the port the node actually listens on

    static std::optional<NodeStatus> parse(std::string_view json);       // the "objects" key decides
    static std::optional<NodeStatus> fromFile(const std::filesystem::path &file);
    // The app shows "Node status unavailable" for a status this old.
    bool fresh(int64_t now, int64_t maxAge = 10) const { return time > 0 && now - time <= maxAge; }
};
} // namespace bmc
```

- [ ] **Step 2: Write `src/node_status.cpp`.** `parse` returns nothing when the text is not an object or has no `objects` key (which is what `Session::tick` tests before it believes the rest); every other key defaults to zero, and a key of the wrong type reads as zero rather than throwing. `fromFile` opens the file, reads at most 4096 bytes (Windows refuses the open for the instant the relay replaces the file, which is why the app keeps its last read), and returns nothing on any failure.

- [ ] **Step 3: Add the file to the build** (`src/node_status.cpp` in `bmclient/CMakeLists.txt`, alphabetical).

- [ ] **Step 4: Write `tests/bmclient/node_status_test.cpp`.** Cases:
  1. `the_counters_are_read` — the full object the relay writes, key for key.
  2. `a_status_without_objects_is_not_a_status` — `{}`, `[]`, `"text"`, and an object with only `time` all read as nothing.
  3. `a_key_of_the_wrong_type_reads_as_zero` — `{"objects":"lots","peers":null}`, no throw.
  4. `fresh_is_the_ten_second_rule` — `time == 0` is never fresh; `now - time == 10` is fresh; 11 is not.

- [ ] **Step 5: Build, run, format.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build-core --parallel 4 && (cd build-core && ./tests/bmclient/bmclient_tests | tail -1)
```

- [ ] **Step 6: Commit.**

```
feat(core): read the node's status file in std

bmc::NodeStatus owns the keys the relay writes and the "older than ten
seconds is unavailable" rule Session::status applies, so the app will
read the node's report without a QJsonObject.
```

## Task 2: `bmc::NodeConfig` — the settings and the argv

**Files:**
- New: `bmclient/include/bmc/node_config.h`, `bmclient/src/node_config.cpp`
- New: `tests/bmclient/node_config_test.cpp`
- Change: `tests/core_tests.cpp` (a temporary parity block, kept until Task 7)

- [ ] **Step 1: Write the header.**

```cpp
// What the node is started with: the settings the app keeps in desktop.ini, and the argv they
// make. A port of 0 means the node's own default; a peer or proxy is IP:port or empty.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bmc {
constexpr int kNodePort = 8444; // the node's NTB_PROTO_DEFAULT_PORT

struct NodeConfig {
    std::string root;                                    // -D
    std::string maildir;                                 // -m
    bool listen = true;                                  // -i when false
    int retentionMB = 2048, retentionDays = 90;          // -R, -A (clamped 64..32768, 1..3650)
    int maxOutgoing = 0, maxIncoming = 0;                // -o, -n when above zero
    int port = 0;                                        // -p when above zero
    std::string peer, proxy;                             // -P/-L, -r/-B
    std::string bin;                                     // the executable to run

    int listenPort() const { return port > 0 ? port : kNodePort; }
    void clampRetention();                               // the two clamps, once
    std::vector<std::string> nodeArguments() const;       // with --node first, as the app builds it
};

// An empty string is the "no peer" the app accepts; otherwise IP:port, as -P and -r take it.
bool validEndpoint(std::string_view text);
// The port is opened on the router only when the node listens with no proxy in front of it.
bool wantsPortMapping(bool online, bool listen, bool upnp, std::string_view proxy);
} // namespace bmc
```

- [ ] **Step 2: Write `src/node_config.cpp`.** `nodeArguments` appends, in this order: `--node`, `-D root`, `-m maildir`, `-i` when `!listen`, `-R`, `-A`, `-o`/`-n` when above zero, `-p` when above zero, `-P peer -L` when the peer is set, `-r proxy -B` when the proxy is set. It does not validate: the app validates first (a bad endpoint is never saved) and the node itself refuses a bad `-R`/`-A`.
  `validEndpoint` is `Session::validEndpoint` without `QUrl`: no empty, no `@`, no `/`, no `?`, no `#`, one `:` with a decimal port in 1..65535, and the host either an IPv4 dotted quad or an IPv6 address in brackets; `listen` is refused while a proxy is set by the caller (`Session::nodeArguments`), not here.

  **Careful:** `QUrl("tcp://…")` accepts more than this (it lowercases, it percent-decodes, it takes `1.2.3.4:080`). The parity block in Step 5 is what decides whether the core agrees with Qt on the strings the app can actually be handed; run it before the Qt body is replaced, and if a string differs, match Qt, do not "improve" it.

- [ ] **Step 3: Add the file to the build.**

- [ ] **Step 4: Write `tests/bmclient/node_config_test.cpp`.**
  1. `the_arguments_are_the_ones_the_app_builds` — the frozen table from Task 0 Step 3, one `CHECK_EQ` per configuration (with the root substituted in).
  2. `a_port_of_zero_asks_for_the_node_default` — no `-p`, and `listenPort() == kNodePort`.
  3. `retention_is_clamped_once` — `clampRetention` on 0/99999 and -1/99999 gives 64/32768 and 1/3650.
  4. `endpoints` — the frozen Qt answers below, one `CHECK_EQ` per string. `QUrl("tcp://" + text)` accepts more than a hand-written parser would: a port with leading zeros, an IPv4 octet with leading zeros, and one leading space. Match it, do not "improve" it, except where the table says otherwise.

| text | Qt | note |
| --- | --- | --- |
| `""` | true | the "no peer" the app accepts |
| `127.0.0.1:8444`, `0.0.0.0:8444`, `255.255.255.255:8444`, `1.2.3.4:1`, `1.2.3.4:65535` | true | |
| `[::1]:8444`, `[::1]:80`, `[::ffff:1.2.3.4]:8444` | true | bracketed IPv6 |
| `1.2.3.4:080`, `1.2.3.4:08`, `1.2.3.4:008444` | true | a port of leading zeros reads as the number |
| `010.2.3.4:8444` | true | an octet of leading zeros, as `QHostAddress` reads it |
| `" 1.2.3.4:8444"` | true | Qt trims only the *leading* whitespace |
| `"1.2.3.4:8444 "`, `"1.2.3.4:8444\t"`, `"  "` | false | …so a trailing one fails |
| `1.2.3.4`, `[::1]`, `1.2.3.4:`, `:8444`, `::1`, `::1:8444` | false | no port, or an unbracketed IPv6 |
| `1.2.3.4:0`, `1.2.3.4:-1`, `1.2.3.4:+8444`, `1.2.3.4:65536`, `1.2.3.4:0000065536` | false | the port range is 1..65535 |
| `a b:1`, `localhost:8444`, `999.1.1.1:8444`, `1.2.3.4.5:8444`, `010.2.3.4:8444` (>255 octet), `[1.2.3.4]:8444` | false | the host must be an address |
| `1.2.3.4:8444/x`, `1.2.3.4:8444?x`, `1.2.3.4:8444#x`, `1.2.3.4:8444%201`, `1.2.3.4:8444.`, `1.2.3.4:8444:8444`, `user@1.2.3.4:8444`, `tcp://1.2.3.4:8444` | false | a path, a query, a fragment or user info |

  The two deliberate differences, both harmless because `Session::saveEndpoint` trims the value and the UI has no other caller: (a) the core trims *surrounding* whitespace, so `"1.2.3.4:8444 "` is accepted where Qt refused it — a superset, and the saved value is the trimmed one; (b) `"1.2.3.4:8444\t"` likewise. The parity block in Step 5 lists them as expected differences; every other string must agree.
  5. `port_mapping_needs_a_listening_node_without_a_proxy` — the four-way table of `wantsPortMapping`.

- [ ] **Step 5: Pin the core against Qt while both exist.** `core_tests` cannot see `Session`, so the parity block goes into `tests/widgets_tests.cpp` — the same file Task 0's probe used, again behind `--node-args` so it costs a second to run. Build a `bmc::NodeConfig` from the same temporary `desktop.ini` files, and check `Session(root, true).nodeArguments()` equals `bmc::nodeArguments(config)` element for element, and that `Session::validEndpoint(QString::fromStdString(s))` equals `bmc::validEndpoint(s)` over the strings of Step 4 plus a few dozen more, with the two whitespace differences above listed as expected. Build the Qt tree and run it.

```sh
cmake --build build --parallel 4 --target desktop_tests && QT_QPA_PLATFORM=offscreen ./build/desktop_tests --node-args
```
Expected: `PARITY OK` and no disagreement. This block stays until Task 7, where `Session` delegates to the core and it becomes the veneer's own check; Task 8 removes it.

- [ ] **Step 6: Build, run both suites' relevant parts, format, commit.**

```
feat(core): build the node's argv in std

bmc::NodeConfig holds the settings the app keeps in desktop.ini and
makes the argv bm::Session makes today, so a caller with no Qt can start
a node; a Qt test pins the two against each other until Session hands
the job over.
```

## Task 3: `bmc::NodeWatch` moves to the core

**Files:**
- New: `bmclient/include/bmc/node_watch.h` (delete `src/node_watch.h`)
- New: `tests/bmclient/node_watch_test.cpp`
- Change: `src/session.h`, `src/session.cpp`, `tests/core_tests.cpp` (its `NodeWatch` block moves out)

- [ ] **Step 1: Move the file.** `git mv src/node_watch.h bmclient/include/bmc/node_watch.h`, then: `namespace bm` → `namespace bmc`, `#include <QtGlobal>` out, `qint64` → `int64_t`, `#include <cstdint>`. Nothing else changes: the fields (`started`, `hungAfter`, `lonelyAfter`, `spacing`, `hungGrace`), the `judge`/`restarted` bodies and the comments are verbatim.

- [ ] **Step 2: Point `Session` at it.** `src/session.h`: drop `#include "node_watch.h"`, add `#include <bmc/node_watch.h>`, `NodeWatch nodeWatch_;` → `bmc::NodeWatch nodeWatch_;`. `src/session.cpp`'s tick uses `bmc::NodeWatch::Verdict`.

- [ ] **Step 3: Move the test.** Cut the `NodeWatch` block out of `tests/core_tests.cpp` (from `// The watchdog of a node that runs but does no good` to the closing brace before the final `std::cout`) and port each `require` to a `CHECK` in `tests/bmclient/node_watch_test.cpp`, as five tests:
  1. `a_node_just_started_is_left_alone` — no status yet; fresh status with no peers is not yet lonely.
  2. `a_status_stale_for_long_is_hung` — the "stale for a while, not for one tick" rule, and a fresh status clearing it.
  3. `no_peer_for_long_is_lonely` — including a peer that came starting the count again, and `lonelyAfter == 0` never judging.
  4. `a_restarted_node_gets_the_whole_time_again`.
  5. `a_hung_node_is_not_restarted_twice_within_the_spacing`.
  Every assertion of the Qt block must appear; the numbers in it are the test.

- [ ] **Step 4: Build both, run both.**

```sh
cmake --build build-core --parallel 4 && (cd build-core && ./tests/bmclient/bmclient_tests | tail -1)
cmake --build build --parallel 4 && (cd build && ctest -R "^core$" --output-on-failure | tail -3)
```
Expected: the core count grows by five; `core` still passes.

- [ ] **Step 5: Commit** `refactor(core): the node watchdog, moved out of Qt`.

## Task 4: `bmc::NodeSupervisor` — the node process

**Files:**
- New: `bmclient/include/bmc/node_supervisor.h`, `bmclient/src/node_supervisor_posix.cpp`, `bmclient/src/node_supervisor_windows.cpp`
- New: `tests/bmclient/node_supervisor_test.cpp`

- [ ] **Step 1: Write the header.** The behaviours are `QProcess`'s as `Session` used it: start, notice an exit (with the code and whether it crashed), read what the node complained about, stop (SIGTERM, three seconds, SIGKILL) and restart.

```cpp
// The node process: started from a NodeConfig, in a process group of its own, stopped the way the
// app stops it (SIGTERM, then SIGKILL after three seconds) and polled for what it says. The app
// keeps the timers that decide *when*; this owns how.
#pragma once
#include <bmc/node_config.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bmc {
class NodeSupervisor {
  public:
    explicit NodeSupervisor(NodeConfig config);
    ~NodeSupervisor();                 // stop(), never throwing
    bool start();                      // false, with lastError() set, when the spawn failed
    // Reads what the child wrote since the last poll and, when it has exited, notes the status.
    // Returns true the first time it sees that exit.
    bool poll();
    void stop();                       // SIGTERM, up to three seconds, then SIGKILL; idempotent
    bool restart();                    // stop() then start()
    bool running() const;
    int64_t pid() const;               // 0 when it is not running
    int exitCode() const;              // the last exit's code, or 0
    bool crashed() const;              // the last exit was a signal, not a code
    bool exited() const;               // an exit has been seen since the last start()
    const std::string &lastError() const;  // why the spawn failed or why it stopped, "" when fine
    // The last non-empty line the child wrote to stderr, trimmed and at most 200 bytes: the app
    // shows it as "Node: <it>".
    std::string standardError() const;
    const NodeConfig &config() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bmc
```

- [ ] **Step 2: Write `src/node_supervisor_posix.cpp`.** `posix_spawn` with the file actions that connect the child's stdout and stderr to one pipe (stdout is read and dropped, as the app's first handler does), `POSIX_SPAWN_SETPGROUP` with pgroup 0 (a group of its own), the inherited environment plus `YNOTBIT_PARENT_PID=<getpid()>` (the bridge turns a dead parent into a clean SIGTERM), `config.bin` as the program and `config.nodeArguments()` as its argv. `poll()` reads the pipe with `O_NONBLOCK` into a buffer, keeps a 200-byte tail of the last non-empty line, and calls `waitpid(pid, &status, WNOHANG)` once; on a WIFEXITED/WIFSIGNALED status it notes the code and whether it was a signal, and reaps the child. `stop()` sends `kill(-pid, SIGTERM)` (the group), waits in 50 ms steps for up to three seconds while reaping, then `kill(-pid, SIGKILL)` and waits up to a second; whatever the child is doing after that, `stop()` returns and leaves nothing to wait for. A `start()` after a stop is allowed.
  No `catch`-all: the only failure paths are the spawn (reported) and a read that is interrupted.

- [ ] **Step 3: Write `src/node_supervisor_windows.cpp`.** `CreateProcessW` with `CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW`, the child in a job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (the Windows process group), `YNOTBIT_PARENT_PID` in the environment block, an anonymous pipe for stderr, `TerminateProcess` after three seconds. **This translation unit is compiled by the Windows CI job and cannot be run here**: keep it to the calls above, guard it with `#ifdef _WIN32`, and have the POSIX one be `#ifndef _WIN32`, so neither is compiled on the other platform.

- [ ] **Step 4: Add both to the build** (`bmclient/CMakeLists.txt`, alphabetically; only one of them compiles per platform).

- [ ] **Step 5: Write `tests/bmclient/node_supervisor_test.cpp`** (POSIX only, `#ifndef _WIN32` around the whole file, with the rest of the suite untouched). The children are `/bin/sh -c …`, so nothing depends on the project's own binaries:
  1. `a_child_that_exits_is_noticed` — `exit 3`: `poll()` becomes true, `exited()`, `exitCode() == 3`, `!crashed()`, `!running()`.
  2. `a_child_that_crashes_is_noticed` — `kill -9 $$`: `crashed()`.
  3. `what_it_wrote_is_kept` — two lines to stderr, then sleeping: `standardError()` is the last one, trimmed.
  4. `a_child_that_ignores_sigterm_is_killed` — `trap "" TERM; sleep 30`: `stop()` returns in under five seconds, `!running()`, and `kill(pid, 0)` fails.
  5. `its_children_die_with_it` — `sleep 30 & echo $! > f; wait`: after `stop()`, the recorded grandchild is gone (retry `kill(pid, 0)` for a moment; a grandchild re-parented to init needs a beat).
  6. `the_child_knows_its_parent` — `sh -c 'echo $YNOTBIT_PARENT_PID'` through a file: it is our `getpid()`.
  7. `stopping_what_is_not_running_is_harmless` — `stop()` twice, and `stop()` before any `start()`.
  8. `a_spawn_that_cannot_happen_is_reported` — `bin = "/nonexistent/…"`: `start()` false, `lastError()` not empty, `!running()`.

- [ ] **Step 6: Build, run, format, commit** `feat(core): start, watch and stop the node process in std`.

## Task 5: The `std` relay bridge and `bmnode`

**Files:**
- New: `bmclient/src/relay_api.h` (a copy of `src/relay_api.h`), `bmclient/src/relay_bridge.cpp`
- New: `bmnode/main.cpp`
- Change: `CMakeLists.txt` (the target)

- [ ] **Step 1: Copy the notbit entry points.** `cp src/relay_api.h bmclient/src/relay_api.h`. It is C, it is Qt-free, and the Qt bridge keeps its own copy until slice 5 deletes it; the core's copy includes `ntb-peer-stats.h` from `third_party/notbit/src`.

- [ ] **Step 2: Write the bridge.** `bmclient/src/relay_bridge.cpp` is `src/relay_bridge.cpp` with:

| Qt | std |
| --- | --- |
| `QDateTime::currentSecsSinceEpoch()` | `bmc::now()` |
| `QFile::decodeName(directory)` | the `const char *` as it is (a path is UTF-8 here) |
| `QDir().mkpath` | `std::filesystem::create_directories(path, ec)` |
| `QJsonDocument(QJsonObject{…}).toJson()` | a hand-built object through `bmc::json::quote` for the string values, in one `bmc::writeFileAtomic` |
| `QDir(...).entryInfoList({"*.object"})` | `std::filesystem::directory_iterator`, sorted by name (`std::sort` on the file names) so the eight-object cap picks the same eight |
| `QRegularExpression("^[a-f0-9-]{36}$")` | the same test in `std` |
| `entry.isSymLink()` | `std::filesystem::symlink_status(...).type() == file_type::symlink` |
| `QFile::remove` | `std::filesystem::remove(path, ec)` |
| `bm::Protocol::inventoryHash`, `bm::ProofOfWork::valid` | `bmc::Protocol::inventoryHash`, `bmc::ProofOfWork::valid` |

  The parent check stays as it is (`getppid() != pid_t(YNOTBIT_PARENT_PID)` → `std::raise(SIGTERM)`, and the Windows branch with `OpenProcess`), the once-a-second gate stays, the `try`/`catch (...)` stays, and **the key names, the states and the order of the writes stay**: `status.json` is written before the spool is walked, an accepted object is offered only when peers exist, and the spool file is removed only when the receipt was written and the object was either refused or offered.

- [ ] **Step 3: Write `bmnode/main.cpp`.**

```cpp
// The node on its own, without Qt: the notbit daemon and the std relay bridge (relay_bridge.cpp).
// Session starts the app itself with --node, so this accepts the same argv, --node included,
// which lets one argument list and one test flag serve both binaries.
#include <cstring>
extern "C" int ntb_daemon(int argc, char **argv);
int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--node") == 0) {
        --argc;
        ++argv;
    }
    return ntb_daemon(argc, argv);
}
```

- [ ] **Step 4: Add the target** to the Qt-free part of `CMakeLists.txt`, beside the other Qt-free targets so `-DYNOTBIT_QT=OFF` builds it:

```cmake
add_executable(bmnode bmnode/main.cpp bmclient/src/relay_bridge.cpp)
target_include_directories(bmnode PRIVATE ${NTB}/src bmclient/src)
target_link_libraries(bmnode PRIVATE bmclient notbit_engine)
```
`relay_bridge.cpp` is not in the `bmclient` library: it defines a symbol the engine calls, only binaries that run a node need it, and putting it in the library would make every consumer link the engine.

- [ ] **Step 5: Build both ways and smoke it by hand.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build-core --parallel 4 --target bmnode
./build-core/bmnode -D /tmp/bmnode-smoke -m /tmp/bmnode-smoke/mail -i -R 64 -A 1 &
sleep 3; cat /tmp/bmnode-smoke/status.json; ls /tmp/bmnode-smoke; kill %1
```
Expected: a `status.json` with the keys of Task 1, `publish/` and `receipts/` created, and a clean exit on the SIGTERM. Use a temporary directory; the smoke run must not touch `~/…/node`.

- [ ] **Step 6: Commit** `feat(core): bmnode, the node without Qt`.

## Task 6: `bmnode` runs the node tests

**Files:**
- Change: `CMakeLists.txt` (the Qt-free tests), `cmake/qt-app.cmake` (the two retention tests against `bmnode`)

- [ ] **Step 1: Register the Python node tests against `bmnode`, in the Qt-free part.** `find_package(Python3 COMPONENTS Interpreter)` is inside the `YNOTBIT_QT` block today; find it in the Qt-free part too (it is optional: when Python is missing, these tests simply do not exist) and add, with the same timeouts the Qt side uses:

```cmake
if(Python3_Interpreter_FOUND)
 add_test(NAME relay-bmnode COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_test.py $<TARGET_FILE:bmnode>)
 add_test(NAME relay-verack-first-bmnode COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_test.py $<TARGET_FILE:bmnode> verack-first)
 add_test(NAME relay-download-bmnode COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_download_test.py $<TARGET_FILE:bmnode>)
 add_test(NAME listen-fallback-bmnode COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/listen_fallback_test.py $<TARGET_FILE:bmnode>)
 add_test(NAME peer-connectivity-bmnode COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/peer_connectivity_test.py $<TARGET_FILE:bmnode>)
 set_tests_properties(relay-bmnode relay-verack-first-bmnode PROPERTIES TIMEOUT 60)
 ...
endif()
```
The scripts take the binary as `argv[1]` and the node flag from `YNOTBIT_TEST_NODE_FLAG`, defaulting to `--node`, which `bmnode` accepts: nothing in the scripts changes. `tls-interop` is not repeated — TLS is the engine's, with no bridge in it.

- [ ] **Step 2: The two retention tests, against `bmnode`.** They run `ynotbit --node -R 0` and expect failure. Add the same two against `$<TARGET_FILE:bmnode>` in `cmake/qt-app.cmake` beside them (the argv is the same, `--node` and all), and keep the app's pair: the app's own node path is still real.

- [ ] **Step 3: Run them.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure | tail -3)
```
Expected: the previous five entries plus the new ones, all passing. `peer-connectivity-bmnode` takes about 70 s.

- [ ] **Step 4: Commit** `test(core): the node tests run against bmnode too`.

## Task 7: `Session` drives the node through the core

**Files:**
- Change: `src/session.h`, `src/session.cpp`, `tests/core_tests.cpp`

- [ ] **Step 1: The members.** In `src/session.h`: `#include <bmc/node_config.h>`, `<bmc/node_status.h>`, `<bmc/node_supervisor.h>`; drop `#include <QProcess>` and `#include "node_watch.h"`; `QProcess node_` → `std::unique_ptr<bmc::NodeSupervisor> node_;`; `QJsonObject lastNodeStatus_` → `std::optional<bmc::NodeStatus> lastNodeStatus_;`; `NodeWatch nodeWatch_` → `bmc::NodeWatch nodeWatch_;`; delete `void startNode();`'s QProcess-ness, keep the name; add `bmc::NodeConfig nodeConfig() const;` and `void pollNode();`.

- [ ] **Step 2: The config.** `Session::nodeConfig()` reads the same `desktop.ini` keys and returns a `bmc::NodeConfig` with `root = root_`, `maildir = root_ + "/unused-maildir"`, `bin = QCoreApplication::applicationFilePath().toStdString()`, and `listen = listenEnabled() && proxy.isEmpty()` — the one place listening and the proxy meet, exactly as `nodeArguments` had it. `nodeArguments()` becomes `QStringList` built from `nodeConfig().nodeArguments()`. `configuredPort()` becomes `nodeConfig().listenPort()`; `validEndpoint` and `wantsPortMapping` delegate to the core.

- [ ] **Step 3: Lifecycle.** The constructor: `node_ = std::make_unique<bmc::NodeSupervisor>(nodeConfig())`, then `startNode()`. The three `QProcess` connections go; `pollNode()` in `tick()` does what they did: reads `node_->standardError()` and, when it is new and not empty, sets `nodeError_` and `error_ = tr("Node: %1")`; on `poll()` returning true (an exit it has not seen), if `nodeError_` is empty and (`exitCode() != 0` or `crashed()`), set `nodeError_` to `tr("crashed")` / `tr("exit code %1")`, and start the same 30 s `QTimer::singleShot` that calls `startNode()` when online and not running. `startNode()`: `updatePortMapping()`, return when offline, `nodeWatch_.restarted(now)`, remove `status.json`, clear `lastNodeStatus_` and `nodeError_`, `node_->start()` (and on failure `error_ = tr("Node: %1").arg(QString::fromStdString(node_->lastError()))`). `restartNode()`: `++nodeRestarts_`, `attempt([&] { node_->stop(); startNode(); })` — not `node_->restart()`, which would start a node while `offline_` and would skip what `startNode()` does (remove `status.json`, clear the last status and the last stderr line, tell the watchdog it was restarted). `~Session`: `node_->stop()` (idempotent, no `stoppingNode_` flag needed).

- [ ] **Step 4: `status()` and `tick()`.** `lastNodeStatus_` is refreshed by `bmc::NodeStatus::fromFile(root_ + "/status.json")` (the Windows "keep the last read" comment moves with it: only a successful read replaces it), `status()` compares `bmc::now() - status->time > 10` → the same `tr("Node status unavailable")`, and `tick()` takes `objects`, `object_bytes`, `established_incoming` and `listen_port` from it. The `QProcess::state() != Running` tests become `!node_->running()`.

- [ ] **Step 5: The parity block** in `tests/widgets_tests.cpp` (behind `--node-args`) stays; it now compares the veneer with the core, which is still a check worth having. It goes in Task 8.

- [ ] **Step 6: Build and run everything the app owns.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build --parallel 4 && (cd build && ctest --output-on-failure | tail -3)
```
Expected: 29/29 — including `peer-connectivity`, `relay`, `listen-fallback`, `desktop` and `lifecycle`, which spawn the app's node through the new supervisor. If `desktop` HANGs with low CPU, the machine is swapping: rerun it alone.

- [ ] **Step 7: Commit** `refactor(qt): Session drives the node through the core`.

## Task 8: Gates, README and the notes

**Files:**
- Change: `src/i18n/error_catalog.cpp` (only if it moves), `bmclient/README.md`, the design spec ("Notes after slice 4", "Notes for slice 5").

- [ ] **Step 1: The catalogue and the translations.** No `tr()` string changes in this slice and no new core message is thrown, so both must be unchanged:

```sh
python3 scripts/update-error-catalog.py --check && echo CATALOGUE-OK
python3 scripts/check-translations.py
```
Expected: `CATALOGUE-OK` and `PASS: 6 translations complete`. Do not run `update_translations`.

- [ ] **Step 2: The compat gate** (no on-disk format changed, so it must pass unchanged):

```sh
SLICE4_BASE=<the hash from Task 0 Step 4> scripts/compat-check.sh
```
Expected: `PASS`, the previous release reads this code's vault and mailbox and the other way round.

- [ ] **Step 3: The Qt-free container gate.** It now also builds `bmnode` and runs the supervisor's tests on glibc — the first time the POSIX process code runs on Linux:

```sh
scripts/check-qt-free.sh big1
```
Expected: `QT-FREE BUILD OK`. (The container installs no Python, so the Python-backed `bmnode` tests are not registered there.) `check-qt-free.sh`'s `inner` is a single-quoted bash string: an apostrophe in a comment breaks it silently.

- [ ] **Step 4: Both `ctest` runs, in sequence, not together.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build --parallel 4 && (cd build && ctest -j2 --output-on-failure | tail -3)
cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure | tail -3)
```
Expected: 29 Qt entries and the core entries, the new `bmnode` ones included.

- [ ] **Step 5: The Qt app on Linux** (the gate slices 2 and 3 used), in an `ubuntu:25.04` container on big1 with Qt 6.8.3 and the dependencies from `scripts/build-dependencies.sh`. Expected: the same 29/29 as macOS. This is the only place the app's `--node` path runs on Linux with the new supervisor.

- [ ] **Step 6: The notes.** Add "Notes after slice 4" to the design spec: that the desktop still starts itself with `--node` and why; that `bmnode` accepts a leading `--node` so one argv serves both; that the supervisor's stop is the app's SIGTERM-then-three-seconds-then-SIGKILL over a process group of its own, that the timers which decide *when* stayed in `Session`, and that the Windows unit is build-verified only; that the bridge exists twice until slice 5; and what is left uncovered (the Windows unit, a spawn that fails mid-life, a child that dies while `stop()` is waiting). Then "Notes for slice 5": the tools that still re-exec themselves (`announce_release`, `post_digest`, `chat_responder`), the Qt bridge and `src/relay_api.h` to delete with them, `bmc::scanMailbox` still having no production caller, and `bm::updates::defaultSubscriptions()` still in the veneer.

- [ ] **Step 7: Remove the probe and write the README.** Delete the `--node-args` block from `tests/widgets_tests.cpp`: the frozen tables in `tests/bmclient/node_config_test.cpp` are the coverage that must live on, and the app's own suites (which start a real node with the real argv) cover the veneer. Rebuild `desktop_tests`. Then `bmclient/README.md`: the node half is in the core (`NodeConfig`, `NodeStatus`, `NodeWatch`, `NodeSupervisor`), `bmnode` is the node without Qt, and the relay bridge is in `bmclient/src/relay_bridge.cpp`.

- [ ] **Step 8: Commit** `docs(core): note the node slice`.

## Gates at the end of the slice

- Qt `ctest`: 29/29, unchanged.
- Core `ctest`: the five entries of slice 3 plus the new `bmnode`-backed ones, all passing.
- `scripts/check-qt-free.sh big1`: `QT-FREE BUILD OK`.
- `SLICE4_BASE=<Task 0> scripts/compat-check.sh`: PASS.
- `python3 scripts/update-error-catalog.py --check` and `scripts/check-translations.py`: unchanged and passing.
- The desktop's node: started from the same argv, the same strings, the same restart rules; `ps` while the app runs shows the node command line `Task 0` recorded.

The numbers these gates actually produced are in **Results**, below.

## Results

Recorded after execution, on the tree of `98c9ddb`, with `17883bf` (`SLICE4_BASE`) as the pre-slice commit.

- **macOS, Qt `ctest`:** `36/36` in 202 s (`-j2`). Thirty-six entries are the 29 of slice 3 plus the seven `bmnode` ones Task 6 registers. `desktop`, `lifecycle`, `peer-connectivity`, `relay` and `listen-fallback` all pass, which is `Session` starting a real node through `bmc::NodeSupervisor`.
- **macOS, core `ctest`:** `10/10` — the five of slice 3, plus `relay-bmnode`, `relay-verack-first-bmnode`, `relay-download-bmnode`, `listen-fallback-bmnode` and `peer-connectivity-bmnode`. `bmclient_tests` alone is `160 run, 0 failed`.
- **`scripts/check-qt-free.sh big1`:** `QT-FREE BUILD OK`, 5/5, with `bmnode` built and the supervisor's POSIX code compiled and run on glibc for the first time.
- **Linux, Qt `ctest`** (Ubuntu 25.04, Qt 6.8.3, `QT_QPA_PLATFORM=offscreen`, serial): `36/36` in 519 s. This is the whole app, its suites and the node tests on Linux, and the app target had never built there — see the fix below.
- **`SLICE2_BASE=17883bf scripts/compat-check.sh`:** PASS. The previous release and the core read each other's vault and mailbox, and both read the four golden files. (The script's variable is `SLICE2_BASE`; the "SLICE4_BASE" of the steps above is that hash.)
- **`python3 scripts/update-error-catalog.py --check`:** `CATALOGUE-OK`; **`scripts/check-translations.py`:** `PASS: 6 translations complete`. Neither file changed, and `update_translations` was not run.
- **The desktop's node argv, by `ps`:** the app ran with a temporary data directory and a settings file asking for everything (`listen=false`, `proxy=127.0.0.1:9`, `peer=127.0.0.1:9`, `maxOutgoing=32`, `maxIncoming=16`, `listenPort=9000`). The child was the app itself with the frozen Task 0 "everything" line, exactly — `--node -D <root> -m <root>/unused-maildir -i -R 2048 -A 90 -o 32 -n 16 -p 9000 -P 127.0.0.1:9 -L -r 127.0.0.1:9 -B` — and it went with the app on SIGTERM. The peer and the proxy deliberately point at port 9: nothing listens, nothing is contacted, and no port is bound (`-i`).
- **One pre-existing break, found by the Linux gate and fixed in its own commit (`b7fe50b`):** `Session::unreadChats` handed a core `int64_t` to `QVariant`, which has six integer constructors — exact for `long long` (macOS), a conversion from `long` (Linux), so GCC calls it ambiguous. The app target, `desktop_tests` and `chatbot_tests` therefore did not build on Linux at all; the Linux gates of slices 2 and 3 ran subsets that included no target compiling `session.cpp`, which is why it went unseen (the line landed with `Session::unreadChats`, which also took `session.cpp` into `chatbot_tests`). One explicit `qint64(...)`. The plan's Linux step said "the same 29/29 as macOS"; the tree builds there now, so the gate runs in full.
- **Deviations from the steps, recorded where they happened:** Task 0's probe went into `desktop_tests` rather than `core_tests` (a `bmc::Client` lives in `bmcore`, `Session` does not); Task 7's `restartNode()` calls `stop()` then `startNode()` rather than the supervisor's `restart()`; Task 8 Step 5's expected count is 36, not 29, since Task 6 adds the `bmnode` entries the same Step 4 lists.
- **The review round.** A fresh reviewer read the branch against this plan and found two things worth fixing, both in the relay bridge's and the supervisor's first version. **First: the node's log arrived as its errors.** One pipe carried the child's stdout and stderr together, and `ntb-daemon.c` sends the node's log to stdout, so `standardError()` — and, through it, `error_ = tr("Node: %1")` in the window — became the log ("Loading saved object store"), text the app never showed while `QProcess` kept the channels apart. Two pipes now, stdout drained and dropped, with two tests that fail on the old code (`what_it_logs_is_not_what_it_says`, `what_it_logs_is_drained_even_though_it_is_dropped`); the Windows unit had the same defect and sends its stdout to the NUL device. The drain also had to cover `stop()`'s waits: a node still writing its log while it shuts down blocks on the pipe and is killed by the same `stop()` that is waiting for it, so the waits read as they go — as `QProcess::waitForFinished()` did — and `a_child_that_logs_its_way_out_is_not_killed_for_it` spends the whole three seconds and ends in a SIGKILL when they do not. **Second: `status.json` and the receipts were compact JSON** where `QJsonDocument::toJson()` writes the indented form with a trailing newline; `jsonObject()` now writes Qt's bytes, verified with a Qt probe that re-emits both files byte for byte. Smaller fixes from the same round: `start()` forgets what the last child said (a test pins it), the Windows unit's `crashed()` is an NTSTATUS exit code rather than "we stopped it", and `lastError()`'s doc says what the code does. Three differences are recorded rather than changed: the 200/400 cut is in bytes where the app cut QChars, a repeated identical line is not re-shown, and an unparsable `status.json` reads as no news. The review also found that Review Focus #2's spool id filter and symlink refusal were not covered by any test, so `tests/relay_test.py` now spools a name of a job id's length in the wrong alphabet and a symlink pointing at a real object, waits two ticks, and finds neither a receipt nor a removal — and the app's own bridge, which runs the same case, passes it too. Gates re-run after the fixes: the Qt `ctest` 36/36, the core `ctest` 10/10 (`164 run, 0 failed`), the Linux Qt `ctest` 36/36, and the container `QT-FREE BUILD OK`.
