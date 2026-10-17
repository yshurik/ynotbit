# Qt-free core, slice 5: the node duplication ends — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** one node, one bridge. `bmnode` is the node the app and its tools run: `Session` starts it, the three tools stop being their own node, and the relay bridge that exists twice becomes one file — `bmclient/src/relay_bridge.cpp` — because the Qt copy (`src/relay_bridge.cpp` and the `src/relay_api.h` it includes) is deleted. `bmc::NodeConfig` learns the three flags a private link needs (`-b -B -e`, which today only `chat_responder`'s own `--node` branch knows about), and which program the node is becomes a pure, tested function of an environment override, the build's `bmnode` and the running executable.

**Architecture:** the same method as slices 1 to 4, with the duplication as the target. Nothing moves into `bmclient` this time: the core gains three `NodeConfig` fields and one function, `Session` calls them, and files are deleted. Three independent pieces, in order:

1. **Which program is the node.** `Session::nodeConfig()` sets `bin` to `bmc::nodeBinary(YNOTBIT_NODE_BIN, <build>/bmnode, thisExecutable())`: an explicit choice wins, then the `bmnode` CMake built, then the executable itself — which is what a copy of the app that has lost `bmnode` still does, because every binary that can be a node carries the **std** bridge. The app keeps its `--node` branch in `src/main.cpp` for exactly that fallback; the tools lose theirs, because they are no longer a node at all.
2. **The private link as a setting.** The three flags `chat_responder` appends to its own `--node` argv today (`-b -B -e`: no default peers, no DNS, only the peers named on the command line) become `NodeConfig::noDefaults/noDns/onlyExplicit`, emitted at the end of the argv, and `chat_responder` writes `linkOnly=true` into its `desktop.ini` where `Session::nodeConfig()` reads the rest of its settings. `YNOTBIT_RESPONDER_PUBLIC` goes away.
3. **The Qt bridge is deleted.** `src/relay_bridge.cpp` and `src/relay_api.h` go; the app's `target_sources` swaps to `bmclient/src/relay_bridge.cpp` (slice 4 proved the std bridge writes the same `status.json` and receipts, byte for byte, and enforces the same spool rules), and `chatbot_tests` drops it along with `notbit_engine`, since it runs only offline sessions.

**Tech Stack:** C++20, Qt 6.8 (the app and the tools), CMake + Ninja, the Qt-free core (`bmclient`, namespace `bmc`), Python 3 for node-level tests (`cryptography` is not needed here), the notbit engine.

**Spec:** `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md`. This is the tool half of the spec's slice 5 ("Bot platform"): the spec's own precondition, quoted in "Notes after slice 4" — *"when they use `bmnode` (or `bmbot`) instead, the Qt bridge and `src/relay_api.h` are deleted"*. `bmbot` itself (the C ABI, `chat_responder` rewritten on it, install rules, examples, Docker, the 24 h soak) is its own subsystem and its own plan: it needs neither the bridge deleted nor the tools moved, and this slice ships without it.

## Global Constraints

- **Nothing in `bmclient/` includes or links Qt**, in headers or sources; `scripts/check-qt-free.sh` is the gate.
- **The app's node argv is frozen** by slice 4's table (Task 0 step 3 below). This slice may only *append* to it: every existing row stays byte-identical, and `-b -B -e` appear after `-L`/`-r <proxy> -B`, at the end, in that order.
- **No user-visible string changes.** The tools print plain literals (not `tr()`); their wording, spacing, exit codes and log lines stay as they are. `scripts/update-error-catalog.py --check` and `scripts/check-translations.py` must pass unchanged, and `update_translations` is **not** run in this slice.
- **`clang-format` only on files this slice edits**, never a tree-wide sweep.
- **Commit per task**, Conventional Commits, prose body ending `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- **Never stage**: `docs/superpowers/plans/2026-10-04-sqlite-object-store.md`, `docs/superpowers/plans/2026-10-15-qt-free-core-slice-1.md`, `docs/superpowers/work-state-2026-10-16.md`, `reports/`, `research_notes/`. `git add -A docs/superpowers` staged one of them once; add files by name.
- **Nothing is pushed, tagged or released** until the user asks. Do not touch the user's running responder or their other `~/w/ynotbit` copies.
- **Test isolation**: the Qt suite and the core suite run in sequence, never at the same time; both bind node ports, and so does `scripts/compat-check.sh`.

## Review Focus

1. **The app's node is still the app's node.** After the swap to the std bridge, `status.json`, the receipts, the spool rules and the parent-death check must be what they were: the `relay`, `relay-verack-first` and `relay-download` tests run `$<TARGET_FILE:ynotbit>` and assert exactly those, so they are the net, and they must pass untouched. `src/main.cpp`'s `--node` branch must still be reachable for a copy of the app that has lost `bmnode`.
2. **The tools stop being a node, and lose nothing else.** The three `--node` branches and `tools::runNode()` are gone; the responder's flags, log lines, status file, answers and exit code are unchanged; the publisher tools' usage messages and dry-run output are unchanged.
3. **The private link still means `-b -B -e`.** The responder's node argv, for both `--public` off and on, is byte-for-byte the argv the child got before this slice (Task 0's rows), and the flags come from `desktop.ini`'s `linkOnly`, read before the node starts. `YNOTBIT_RESPONDER_PUBLIC` is gone from the tree.
4. **The frozen argv table is untouched for every app configuration**; `-b -B -e` are appended only when set. `bmc::nodeBinary` honours the environment first, then a real `bmnode` file, then the executable; an explicit choice that is missing is used anyway (the failure is the caller's to show).
5. **A missing `bmnode` degrades as today.** With `YNOTBIT_NODE_BIN` unset and no `bmnode` beside the build, the app runs the node as itself, exactly as before this slice; only the *default* changes.

## Task 0: the goldens, the net, and the base commit

**Files:** none (probes and notes; the frozen rows are written into Task 3's test and Task 1's script).

- [ ] **Step 1: The responder's node argv today — frozen here.** Build the tools and run the responder against a dead loopback port, then read the child's argv:

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build --parallel 4 --target chat_responder announce_release post_digest
build/chat_responder --data-dir /tmp/slice5-probe --fresh --silent \
  --phrase "slice 5 probe" --peer 127.0.0.1:9 &
sleep 3; ps -ww -o pid,ppid,command -A | grep -E "chat_responder --node|--node -D /tmp/slice5-probe" | grep -v grep
```

The child is the responder itself today, started by `Session` with:

| responder invocation | node argv, with `<root>` for `--data-dir`'s folder |
| --- | --- |
| `--phrase "slice 5 probe" --peer 127.0.0.1:9` | `--node -D <root> -m <root>/unused-maildir -i -R 2048 -A 90 -P 127.0.0.1:9 -L -b -B -e` |
| `--public --peer 127.0.0.1:9` | `--node -D <root> -m <root>/unused-maildir -i -R 2048 -A 90 -o 12 -n 24 -P 127.0.0.1:9 -L` |
| `--public --peer "" --listen --port 9000` | `--node -D <root> -m <root>/unused-maildir -R 2048 -A 90 -o 12 -n 24 -p 9000` |

`<root>` is `--data-dir` (or `QStandardPaths`' app data location) joined with the first eight hex digits of the SHA-256 of the phrase; the probe prints the exact value, and Task 1's wrapper records the exact argv. The order the emitter appends is `--node -D -m -i -R -A -o -n -p -P -L -r -B -b -B -e`. Note the deliberate duplicate when a proxy is set *and* `linkOnly`: `-B` twice, same meaning to the engine's getopt; the flag is not deduplicated, because deduplicating would change an argv this slice does not own.

- [ ] **Step 2: The app's node argv — the slice-4 table, this slice's invariant.** `docs/superpowers/plans/2026-10-17-qt-free-core-slice-4.md` Task 0 step 3 lists the eight rows. They are pinned in `tests/bmclient/node_config_test.cpp`; after Task 3 they must still pass, unchanged.

- [ ] **Step 3: The publishers' contract.** Record what the two publisher tools print and return today:

```sh
build/announce_release; echo "exit=$?"
build/post_digest; echo "exit=$?"
build/chat_responder --help | head -3; echo "exit=$?"
```

Expected: `announce_release: usage: announce_release --keys <keys.dat> --version <X.Y.Z> --notes <file> [--chan <phrase>]... [--send] [--linger <minutes>]` and `post_digest: usage: post_digest --keys <keys.dat> --issue <file.md> [--subject <text>] [--send] [--linger <minutes>]`, both on stderr, both exit 1; `chat_responder --help` prints its usage to stdout and exits 0. Task 6 freezes the first two in a test. The publishers' `--send` path cannot be tested in this tree: the release and digest addresses' private keys are the maintainer's (`bmc::updates::publisherAddress()`, `digestAddress()`), so there is no `keys.dat` a test could hold. Say so in Task 8's notes rather than pretending otherwise.

- [ ] **Step 4: Note the pre-slice commit.**

```sh
git rev-parse HEAD
```

Expected: the plan's parent, `b656324`, unless something else landed first. That hash is `SLICE5_BASE`, used by the compat gate in Task 9.

## Task 1: the responder runs a node and sees a peer — the test it never had

**Files:**
- New: `tests/chat_responder_test.py`
- Change: `cmake/qt-app.cmake` (register the test; `chat_responder` leaves `EXCLUDE_FROM_ALL`)

`chat_responder` has no test at all today; this is the net the rest of the slice is measured against. It is written against the code as it stands (its node is the tool itself, `--node`), and Task 5 extends it with the assertions that fail on today's code.

- [ ] **Step 1: Write the script.** It takes the tool as `sys.argv[1]` and `bmnode` as `sys.argv[2]` (like `tests/relay_test.py` takes the node), listens on `127.0.0.1:0`, and runs the tool against itself as the peer. Copy the `frame`/`receive` helpers from `tests/relay_test.py` (they are top-level code there, not a module) and say so in a comment.

```python
"""The chat responder service: its own node comes up, links to its one peer, and reports.
The node process is checked twice: once as the build starts it (bmnode), once through a wrapper
that records the argv, so the private link (-b -B -e) is pinned rather than assumed."""
```

Steps (each assertion is one the slice must not break):
  1. `tempfile.TemporaryDirectory()`; a listener socket on port 0.
  2. A wrapper at `<tmp>/node-probe` (`#!/bin/sh`, `printf '%s\n' "$@" > <tmp>/node-argv`, `exec <bmnode> "$@"`, `chmod +x`), and a second run with `YNOTBIT_NODE_BIN` unset.
  3. Run `chat_responder --data-dir <tmp>/data --fresh --silent --phrase "slice 5 responder" --peer 127.0.0.1:<port> --log <tmp>/log.txt --status-file <tmp>/status.json`, with `QT_QPA_PLATFORM=offscreen`.
  4. `accept()`; the first frame is `version`; assert its user agent matches `/ynotbit:\d+\.\d+\.\d+/` — the node is the real engine, whoever started it.
  5. Send `verack` then `version` (as `relay_test.py` does); wait up to 20 s for `<tmp>/status.json` to exist and report `"peers": 1` with an `"address"` — the tool's `Session` → `Client` → node chain ran and its health timer fired.
  6. Assert the log has, verbatim and in order: `Chat responder`, `Address:  <that address>`, `Node:     links to 127.0.0.1:<port> only, the public network left alone`, `Data:     <tmp>/data/<8 hex>`, and a `Network:  ` line once the peer shows up.
  7. `SIGTERM`; wait (≤10 s); assert the exit code is 0 and the last log line starts `Bye.`.
  8. Print one `PASS: ...` line naming what was checked.

- [ ] **Step 2: Register it and build it by default.** In `cmake/qt-app.cmake`, next to the other Python tests (line ~86, `if(Python3_Interpreter_FOUND)`):

```cmake
 add_test(NAME chat-responder COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/chat_responder_test.py
  $<TARGET_FILE:chat_responder> $<TARGET_FILE:bmnode>)
 set_tests_properties(chat-responder PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
```

A test that runs a target is worthless if the target is not built: remove `chat_responder` from the `foreach(tool ...)` at line ~163 and give it its own `qt_add_executable(chat_responder ...)` without `EXCLUDE_FROM_ALL` (the two publishers stay excluded: nothing can test them without the maintainer's keys).

- [ ] **Step 3: Run it.**

```sh
cmake -S . -B build -G Ninja && cmake --build build --parallel 4
cd build && ctest -R chat-responder --output-on-failure
```

Expected: PASS, ~10 s. This is the pre-change baseline: it must pass *before* anything in Tasks 3 to 6.

- [ ] **Step 4: Commit.**

```
test(tools): the responder's own node, peer and status file

chat_responder had no test at all. This one starts it against a
scripted peer and checks what the service promises: a real ynotbit node
comes up, it links to the peer it was given alone, status.json reports
the peer, the log says so, and SIGTERM ends it with Bye.

The test is written against today's code, where the tool is its own
node. The next commits move that node to bmnode and extend this test
with the two things that can then be checked from outside: which binary
the node is, and the argv it was started with.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 2: the app's node — the test it never had either

**Files:**
- New: `tests/app_node_test.py`
- Change: `cmake/qt-app.cmake` (register it)

The app's *node* is well covered (`relay`, `relay-download`, `listen-fallback`, `peer-connectivity` all run `ynotbit --node` directly). What nothing covers is the app *starting* that node — which is exactly what this slice changes. Same shape as Task 1, with the GUI in front.

- [ ] **Step 1: Write the script.** Takes the app and `bmnode` as `sys.argv[1]`/`sys.argv[2]`. One listener; the data directory the test passes to `--data-dir` is the session's root, so its settings file is `<tmp>/data/desktop.ini` (the app's own keys; `QSettings` and `Session::nodeConfig()` read the same file):

```ini
vault=<tmp>/data/app.bmvault
mailbox=<tmp>/data/app.bmmail
listen=false
upnp=false
linkOnly=true
peer=127.0.0.1:<port>
retentionMB=64
retentionDays=1
maxOutgoing=16
maxIncoming=4
```

Assertions:
  1. Run `ynotbit --data-dir <tmp>/data` with `QT_QPA_PLATFORM=offscreen` and `YNOTBIT_NODE_BIN=<tmp>/node-probe` (the wrapper from Task 1: records its argv to `<tmp>/node-argv`, then `exec`s `bmnode`). The app has no vault, so it opens its "new vault" dialog behind the node — harmless, and the reason the node starts in `Session`'s constructor.
  2. Within 20 s the wrapper's marker exists (the app started a node at all), it was given `--node -D <tmp>/data -m <tmp>/data/unused-maildir -i -R 64 -A 1 -o 16 -n 4 -P 127.0.0.1:<port> -L -b -B -e` — slice 4's "all of the above" row, minus `-p`, plus this slice's private link — and the node connects to the listener and sends `version` identifying as ynotbit.
  3. Within 20 s, some descendant of the app is `bmnode`: `ps -axo pid,ppid,comm` and match `comm` ending in `bmnode` (macOS prints the path, Linux the 15-character name; match the tail, not equality).
  4. `SIGTERM` the app. The app has no signal handler, so it dies at once; the node must be gone within 15 s on its own, because the relay bridge stops a node whose parent is gone (`YNOTBIT_PARENT_PID`). Assert the node's pid is gone, not the app's exit code.

- [ ] **Step 2: Register it** next to `translation`-style Python tests, `TIMEOUT 120`, `ENVIRONMENT "QT_QPA_PLATFORM=offscreen"`.

- [ ] **Step 3: Run it.** Expect the wrapper marker assertion to fail today (the app ignores `YNOTBIT_NODE_BIN` and runs itself) — record the failing line; steps 1, 3 and 4 of the assertions are still worth having green after Task 5. This is the RED the next commits turn green.

- [ ] **Step 4: Commit.**

```
test(app): the node the app starts, and where it comes from

Session's node belongs to the constructor and was only ever checked
through the app's --node branch, never through the app starting it. The
test gives the app a data directory it can read, a peer to link to, and
a node binary to run, and asks four questions: did a node start, with
which argv, is it bmnode, and does stopping the app stop it.

The YNOTBIT_NODE_BIN assertion fails on today's code, which is the
point.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 3: `bmc::NodeConfig` — the private link

**Files:**
- Change: `bmclient/include/bmc/node_config.h`, `bmclient/src/node_config.cpp`
- Change: `tests/bmclient/node_config_test.cpp`

- [ ] **Step 1: Write the failing test first.** In `tests/bmclient/node_config_test.cpp`, a case per flag and one for the trio (Task 0's rows), e.g.:

```cpp
TEST(the_private_link_is_appended_last) {
    bmc::NodeConfig c;
    c.root = "/r"; c.maildir = "/r/mail"; c.listen = false;
    c.peer = "127.0.0.1:9000";
    c.noDefaults = c.noDns = c.onlyExplicit = true;
    CHECK_EQ(joined(c.nodeArguments()), // the file's own helper, like the rows above
             std::string("--node -D /r -m /r/mail -i -R 2048 -A 90 -P 127.0.0.1:9000 -L -b -B -e"));
}
TEST(each_private_link_flag_stands_alone) { /* -b, -B, -e, one at a time, each at the end */ }
TEST(a_proxy_and_the_private_link_both_say_no_dns) { /* -r p -B ... -B: what the responder sends */ }
```

- [ ] **Step 2: Add the fields.**

```cpp
    bool noDefaults = false;   // -b: don't bootstrap from the default peers
    bool noDns = false;        // -B: don't bootstrap by DNS (a proxy already says this)
    bool onlyExplicit = false; // -e: connect to the peers named on the command line and no others
```

emitted after every other argument, in the order `-b -B -e`, each only when set. Comment the trio once, in the header: these are the three flags `chat_responder` appends for a private link, and the app never sets them.

- [ ] **Step 3: Emit them** at the end of `nodeArguments()` in `bmclient/src/node_config.cpp`.

- [ ] **Step 4: Run.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build-core --parallel 4 && (cd build-core && ./tests/bmclient/bmclient_tests --filter private_link; ./tests/bmclient/bmclient_tests --filter node_config | tail -1)
```

Expected: the new cases pass and slice 4's eight rows are untouched (`node_config` filter still green).

- [ ] **Step 5: Commit.**

```
feat(core): the private link in the node's argv

chat_responder's own --node branch knows three flags the app never
sends: -b, -B and -e, the ones that make a node link to the peer it was
given and nothing else. They become NodeConfig fields, emitted at the
end of the argv in that order, so the responder's node argv can be built
without a branch that only that program takes.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 4: `bmc::nodeBinary` — which program is the node

**Files:**
- Change: `bmclient/include/bmc/node_config.h`, `bmclient/src/node_config.cpp`
- New: `tests/bmclient/node_binary_test.cpp`, added to the explicit source list in `tests/bmclient/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(an_explicit_choice_wins_even_when_it_is_not_there) {
    CHECK_EQ(bmc::nodeBinary("/opt/node", "/build/bmnode", "/app/ynotbit"), "/opt/node");
}
TEST(bmnode_is_used_when_it_is_where_the_build_put_it) {
    const auto file = bmc::test::tempDir() + "/bmnode";
    bmc::test::writeFile(file, "");
    CHECK_EQ(bmc::nodeBinary("", file, "/app/ynotbit"), file);
}
TEST(a_directory_is_not_a_node) { /* a temp *directory* as `built` falls back to self */ }
TEST(a_missing_build_path_falls_back_to_this_executable) {
    CHECK_EQ(bmc::nodeBinary("", "/nonexistent/bmnode", "/app/ynotbit"), "/app/ynotbit");
    CHECK_EQ(bmc::nodeBinary("", "", "/app/ynotbit"), "/app/ynotbit");
}
TEST(an_empty_self_stays_empty) { CHECK_EQ(bmc::nodeBinary("", "", ""), ""); }
```

- [ ] **Step 2: Write the function.**

```cpp
// Which program is the node: an explicit setting wins (YNOTBIT_NODE_BIN here, or the bot library's
// own), then bmnode where the build put it, then this executable — which is its own node, because
// every binary that can start one carries the std bridge. The first two make the node a program of
// its own; the last is what a copy of the app that has lost bmnode still does.
std::string nodeBinary(std::string_view fromEnv, std::string_view built, std::string_view self);
```

Explicit wins even when the file is missing — a setting that silently falls back is a setting that hides a broken deployment. `built` is used only when `std::filesystem::is_regular_file(bmc::path(built))` (with an `error_code`, no throw), so a directory or a dangling path falls back.

- [ ] **Step 3: Build, run, format** (as in Task 3's step 4, with `--filter node_binary`), then `clang-format -i` on the two files this task edited.

- [ ] **Step 4: Commit.**

```
feat(core): which program is the node

An explicit YNOTBIT_NODE_BIN, then bmnode where the build put it, then
this executable, which is its own node. The choice is a pure function so
that it can be tested without a process, and so that Session does not
carry the policy.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 5: `Session` runs `bmnode`, and the responder's link is a setting

**Files:**
- Change: `src/session.cpp`, `src/session.h` (if a declaration is needed)
- Change: `tools/chat_responder.cpp` (write `linkOnly` into `desktop.ini`; delete the `YNOTBIT_RESPONDER_PUBLIC` export)
- Change: `cmake/qt-app.cmake` (the `YNOTBIT_BMNODE` definition)
- Change: `tests/chat_responder_test.py`, `tests/app_node_test.py` (the assertions that fail today)

This is the commit where the app's node stops being the app.

- [ ] **Step 1: `src/session.cpp` — the binary.** Above `nodeConfig()`:

```cpp
// The build's bmnode, when this target was given one: the app and its tools run the Qt-free node by
// default, and fall back to this executable (which carries the std bridge) without it.
#ifndef YNOTBIT_BMNODE
#define YNOTBIT_BMNODE ""
#endif
```

and in `nodeConfig()` replace `node.bin = QCoreApplication::applicationFilePath().toStdString();` with

```cpp
    node.bin = bmc::nodeBinary(bridge::toStd(qEnvironmentVariable("YNOTBIT_NODE_BIN")), YNOTBIT_BMNODE,
                              QCoreApplication::applicationFilePath().toStdString());
```

- [ ] **Step 2: `src/session.cpp` — the private link.** `nodeConfig()` already reads `listen`, `peer`, `listenPort`, `maxOutgoing` and `maxIncoming` out of `root + "/desktop.ini"`; read one more key and set the three flags from it:

```cpp
    // A responder links to the peer it was configured with and nothing else (-b -B -e); a desktop
    // leaves the public network on.
    node.noDefaults = node.noDns = node.onlyExplicit = config.value("linkOnly", false).toBool();
```

- [ ] **Step 3: `tools/chat_responder.cpp` — the key.** In the `QSettings` block that writes `vault`, `mailbox`, `listen`, `upnp`, `peer`, `listenPort`, `maxOutgoing` and `maxIncoming`, add:

```cpp
        // Not joining the public network means linking to the peer alone: no default peers, no
        // DNS, no other peers. Session turns this into -b -B -e.
        config.setValue("linkOnly", !joinPublic);
```

and delete `if (joinPublic) qputenv("YNOTBIT_RESPONDER_PUBLIC", "1");`. Update the comment above the `--node` branch: after Task 6 that branch is gone; until then it is dead for a responder whose node is `bmnode`, which is why the setting, not the branch, is now the truth.

- [ ] **Step 4: `cmake/qt-app.cmake` — the definition.** Every target that compiles `src/session.cpp` needs to know where `bmnode` is. `bmnode` is defined in `CMakeLists.txt` before this file is included (line ~114 vs ~131), so a generator expression is enough:

```cmake
# Where this build put bmnode: Session runs it as the node, and falls back to the executable itself
# when a copy of the app has lost it.
foreach(node_consumer ynotbit desktop_tests readme_screenshots feed_scroll_bench
                       announce_release post_digest chat_responder chatbot_tests)
 target_compile_definitions(${node_consumer} PRIVATE "YNOTBIT_BMNODE=\"$<TARGET_FILE:bmnode>\"")
endforeach()
```

- [ ] **Step 5: The tests that fail on the old code, now green.** Add to `tests/chat_responder_test.py` and `tests/app_node_test.py` the assertions written in Tasks 1 and 2 but not yet expected to pass: the wrapper's recorded argv (Task 0's rows) and the `bmnode` descendant found with `ps`. Run both.

```sh
cmake -S . -B build -G Ninja && cmake --build build --parallel 4
cd build && ctest -R "chat-responder|app-node" --output-on-failure
```

Also re-run the app's node tests, which must be unaffected: `ctest -R "relay|listen-fallback|peer-connectivity"`.

- [ ] **Step 6: Commit.**

```
feat(app): the node is bmnode, and the responder says so in its settings

Session picked its node with QCoreApplication::applicationFilePath():
the app, its tools and its tests were each their own node, and each
carried the Qt relay bridge to be one. The binary is now a choice — an
explicit YNOTBIT_NODE_BIN, then the bmnode this build made, then the
executable itself, which is what a copy of the app without bmnode still
does.

chat_responder's private link stops being an environment variable read
by a --node branch: it writes linkOnly into the desktop.ini Session
already reads for its peer and port, and Session turns it into
-b -B -e. YNOTBIT_RESPONDER_PUBLIC is gone.

Both Python tests now assert what only the new code can: the node is
bmnode, and its argv is the one frozen in the plan.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 6: the tools stop being their own node

**Files:**
- Change: `tools/announce_release.cpp`, `tools/post_digest.cpp`, `tools/chat_responder.cpp`, `tools/publish_broadcast.h`, `tools/publish_broadcast.cpp`
- Change: `cmake/qt-app.cmake`
- New: `tests/tools_usage_test.py`

- [ ] **Step 1: Delete the branches.** `tools/announce_release.cpp:16-18` and `tools/post_digest.cpp:30-32` (`if (argc > 1 && std::string(argv[1]) == "--node") return tools::runNode(...)`) go; `tools/chat_responder.cpp:136-149` goes with them, and with it the `int ntb_daemon(...)` declaration in that file. `tools::runNode` and its `extern "C" int ntb_daemon(int, char **)` go from `tools/publish_broadcast.cpp`, and the declaration plus the "the node is the tool's own executable with --node" sentence go from `tools/publish_broadcast.h`.

- [ ] **Step 2: The bridge and the engine leave the tools.** In `cmake/qt-app.cmake`, drop `src/relay_bridge.cpp` from the `chat_responder` source list and `notbit_engine` from its link line; same for `chatbot_tests` (which runs only offline sessions, so it never needed either). Verify by building: if a target then fails to link for a reason this plan did not foresee, investigate the reason rather than putting the engine back. The app (`ynotbit`) keeps `notbit_engine` and gains `bmclient/src/relay_bridge.cpp` in Task 7 — its `--node` fallback needs both.

- [ ] **Step 3: A test for the publishers' contract** (`tests/tools_usage_test.py`, taking all three tools as argv): each publisher with no arguments prints its usage line on stderr and exits 1 (Task 0's verbatim strings); `chat_responder --help` exits 0 and prints its usage; none of the three starts a node for a usage error (assert no `bmnode` child, and no `desktop.ini` was written in the data directory passed with `--data-dir`). Register it in `cmake/qt-app.cmake` with `TIMEOUT 60`, and extend the gate commands in Task 9 to build `announce_release post_digest` first.

- [ ] **Step 4: Run everything the tools have.**

```sh
cmake --build build --parallel 4 --target announce_release post_digest chat_responder
cd build && ctest -R "chat-responder|tools-usage" --output-on-failure
```

- [ ] **Step 5: Commit.**

```
refactor(tools): the tools are no longer a node

announce_release, post_digest and chat_responder each carried a --node
branch that called ntb_daemon on themselves, plus the bridge and the
engine to be the node that branch started. Their node is bmnode now, so
the branches, tools::runNode and the two libraries go: the tools do one
job, and the node is one program.

A usage error still prints exactly what it printed, and the publishers'
--send path stays untested on purpose: the release and digest keys are
the maintainer's, so no test in this tree can hold a keys.dat for them.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 7: the Qt bridge is deleted

**Files:**
- Delete: `src/relay_bridge.cpp`, `src/relay_api.h`
- Change: `cmake/qt-app.cmake` (line ~69: the app's `target_sources`)

- [ ] **Step 1: The app takes the std bridge.**

```cmake
# The app can still be its own node (src/main.cpp's --node branch, the fallback when bmnode is
# missing), with the same bridge bmnode uses. The Qt copy is gone: one implementation, one set of
# bytes.
target_sources(ynotbit PRIVATE bmclient/src/relay_bridge.cpp)
target_link_libraries(ynotbit PRIVATE notbit_engine)
```

The bridge includes `"relay_api.h"`, which resolves next to the file it is written in — `bmclient/src/`, the copy that stays.

- [ ] **Step 2: Delete the files.**

```sh
git rm src/relay_bridge.cpp src/relay_api.h
grep -rn "relay_bridge\|relay_api" cmake/ CMakeLists.txt src/ tools/ tests/ | grep -v "^bmclient/" | grep -v Binary
```

Expected after the edit: only `bmnode`'s and the app's `target_sources` mention the bridge, and every one of them names `bmclient/src/relay_bridge.cpp`. Nothing includes `src/relay_api.h`.

- [ ] **Step 3: The proof is the existing relay tests**, which run the app's own node path and assert the file bytes, the spool rules and the receipts: `ctest -R "relay|relay-download|listen-fallback"`. Run them, and also the two Qt pools the app's own suite (`desktop`, `chatbot`) — the rename changes no behaviour, so a failure here is a real finding, not a flake.

- [ ] **Step 4: Commit.**

```
refactor(app): one relay bridge

The engine's file boundary existed twice: bmclient/src/relay_bridge.cpp
for bmnode, and a Qt copy beside it for the programs that ran
themselves as the node. Slice 4 proved the std one writes the same
status.json and receipts, byte for byte, and refuses the same spool
entries; the app now uses it too, and the Qt copy and its relay_api.h
are deleted.

src/main.cpp keeps its --node branch: a copy of the app that has lost
bmnode is still its own node, which is the fallback Session's binary
choice ends at.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 8: the documents

**Files:**
- Change: `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md` (a "Notes after slice 5" section)
- Change: `docs/chat-responder.md` (anything this slice makes false)
- Change: this plan (a `## Results` section, written last, with the real numbers)

- [ ] **Step 1: The spec's notes.** Append, in the shape of the earlier ones: what moved to `bmnode` and what was deleted; the `linkOnly` setting and the three flags; that `YNOTBIT_RESPONDER_PUBLIC` is gone; the deliberate differences and the untested publishers; that `bmc::scanMailbox`, `bmc::nowMillis()` and `bm::updates::defaultSubscriptions()` are still as slice 3 left them; and what slice 6 (`bmbot`) starts from.
- [ ] **Step 2: `docs/chat-responder.md`.** Read it and fix what this slice changed: how the node is started, what `--public` does (it is the `linkOnly` setting now), and whether it still points at `build/chat_responder --help` for flags that moved.
- [ ] **Step 3: Commit.**

```
docs: what slice 5 leaves

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 9: the gates

**Files:** none (commands and the results note).

- [ ] **Step 1: macOS, the Qt side and the core, in sequence.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake -S . -B build -G Ninja && cmake --build build --parallel 4 --target announce_release post_digest
cd build && ctest --output-on-failure          # expect one more test than slice 4: chat-responder, app-node, tools-usage
cd .. && cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure)
```

- [ ] **Step 2: Linux Qt, on big1** (serialise: it binds the same ports as the next step).

```sh
rsync -a --delete --exclude '/build' --exclude '/build-*' --exclude '/.tokensave' \
  --exclude '/reports' --exclude '/research_notes' ./ big1:w/ynotbit-slice5/
ssh big1 'cd ~/w/ynotbit-slice5 && cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/home/user/Qt/6.8.3/gcc_64 -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 16 --target announce_release post_digest && cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure'
```

Do not touch the user's own `~/w/ynotbit` there.

- [ ] **Step 3: the Qt-free container.**

```sh
scripts/check-qt-free.sh big1
```

Expected `QT-FREE BUILD OK`, 5/5 (the Python tests are not registered there: the image has no python3).

- [ ] **Step 4: the cross-open gate and the text gates.**

```sh
scripts/compat-check.sh SLICE5_BASE
scripts/update-error-catalog.py --check
scripts/check-translations.py
```

- [ ] **Step 5: the manual probe the tests cannot make.** With `YNOTBIT_NODE_BIN` unset, start the app offscreen against a data directory with `peer=127.0.0.1:9, listen=false, linkOnly=true` and confirm with `ps -ww` that the node child is `build/bmnode`; then move `build/bmnode` aside, start it again, and confirm the app still runs (its own node) — the fallback. Record both.

- [ ] **Step 6: write `## Results`** into this plan: the gate numbers with the commit they describe, what the review found, and what stayed undone (the publishers' send path, `bmbot`). Commit it as `docs: record what slice 5's gates ran`.

## Results

*(Written after execution, on the tree of the last commit of the slice.)*
