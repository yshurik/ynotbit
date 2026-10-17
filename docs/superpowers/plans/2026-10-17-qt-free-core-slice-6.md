# Qt-free core, slice 6: bot platform (bmbot C ABI) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a standalone C ABI library `libbmbot` that provides a bot runtime over the Qt-free core (`bmclient`). The library spawns a `bmnode` node process, presents a minimal JSON config, and returns durable inbox events (`chat`, `letter`), delivery states, and status. Python ctypes and Go cgo can call the ABI directly. The library is built as a shared object (`libbmbot.so` / `libbmbot.dylib`) so Python ctypes can `cdll` it, and Go cgo can `#cgo LDFLAGS: -lbmbot`.

**Architecture:** `libbmbot` is a thin but complete C library that:
- Parses a JSON config (`data_dir`, `phrase_file`, `node`, `limits`, `hang_seconds`)
- Spawns `bmnode` as a child process with the std relay bridge
- Provides a durable, at-least-once inbox via the node's `objects.sqlite` + `publish/` / `receipts/`
- Returns events one-shot per run (acked by `bmbot_done`; after crash/restart the undone ones come back)
- `note` events carry problems and watchdog restarts (the library never prints); they have no `id`, are not durable and need no `bmbot_done`
- Delivery states (`bmbot_state`) are the same strings the app shows today
- Status JSON includes peers, objects, uptime, counts taken, answered, limited, failed
- `bmbot_next` blocks up to `timeout_ms`; returns 1 (no event) or 0 (event filled into `event_json`)
- `bmbot_send_chat` / `bmbot_send_letter` enqueue outbox messages (the library does not yet transmit them over the wire — that is slice 7)

**Tech Stack:** C99 / C++20 interop, the notbit C engine, `bmclient` (Vault/Mailbox/Protocol/Wire), OpenSSL, libsodium, SQLCipher, Python 3 (ctypes), Go (cgo). The library has **no Qt** dependency — it is verified by `nm -D` / `ldd` showing no Qt symbols.

**Spec:** `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md`. This is the bot platform slice: `bmbot`, `chat_responder` on the ABI, install rules, examples, Docker, the 24 h soak. It depends on slice 5 (the node duplication ends, the Qt bridge deleted, `NodeConfig` has the private link flags).

**Global Constraints**
- **Nothing in `bmclient/` or `libbmbot` includes or links Qt**, in headers or sources; `scripts/check-qt-free.sh` is the gate.
- **The app's node argv is frozen** by slice 4's table. This slice may only append to it: every existing row stays byte-identical.
- **No user-visible string changes** in the tools; `scripts/update-error-catalog.py --check` and `scripts/check-translations.py` must pass unchanged, and `update_translations` is **not** run in this slice.
- **`clang-format` only on files this slice edits**, never a tree-wide sweep.
- **Commit per task**, Conventional Commits, prose body ending `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- **Never stage**: `docs/superpowers/plans/2026-10-04-sqlite-object-store.md`, `docs/superpowers/plans/2026-10-15-qt-free-core-slice-1.md`, `docs/superpowers/work-state-2026-10-16.md`, `reports/`, `research_notes/`. `git add -A docs/superpowers` staged one of them once; add files by name.
- **Nothing is pushed, tagged or released** until the user asks. Do not touch the user's running responder or their other `~/w/ynotbit` copies.
- **Test isolation**: the Qt suite and the core suite run in sequence, never at the same time; both bind node ports, and so does `scripts/compat-check.sh`.

**Review Focus**
1. **libbmbot builds and links without Qt** — `nm -D libbmbot.dylib` shows no Qt symbols; `ldd` shows only OpenSSL, libsodium, SQLCipher, secp256k1, Threads.
2. **The ABI functions are callable from Python ctypes** — the ctypes test (`tests/bmbot/ctypes_test.py`) passes, including `bmbot_open`, `bmbot_status`, `bmbot_state`, and the library degrades gracefully when `bmnode` is unavailable.
3. **The Docker image builds and runs** — `docker build` succeeds, `docker run` starts `chat_responder` (on the C ABI), `bmnode`, and `libbmbot`; the health check (`chat_responder --check`) returns 0; `SIGTERM` gracefully stops the container.
4. **The Python echo example runs** — `python3 bmbot/examples/echo.py` loads the library and calls `bmbot_abi_version` without crash.
5. **The Go example (when go is present)** compiles `cgo` LDFLAGS `-lbmbot` and links without errors.
6. **Existing app suites remain green** — `ctest` of the app, the core `bmclient` tests, and the Qt-free container build all pass untouched.

**Tasks:**

## Task 0: pre-slice commit and golden freeze

**Files:** none (procedural).

- [ ] **Step 1: Note the pre-slice commit.**

```sh
git rev-parse HEAD
```

Expected: the plan's parent, `67894a8`, unless something else landed first. That hash is `SLICE6_BASE`, used by the compat gate in Task 8.

- [ ] **Step 2: the Qt-free gate still passes** — run the app Qt suite and the core suite in sequence, and run `scripts/check-qt-free.sh` in a container. All must be green.

## Task 1: bmbot C ABI library (already done — see below)

The `libbmbot` shared library is built and provides the functions declared in `bmbot/bmbot.h`. See the built artefacts in `bmbot/build/`.

- [ ] **Step 1: verify the library** — run ` otool -L bmbot/build/libbmbot.dylib` and `nm -D bmbot/build/libbmbot.dylib`; verify no Qt symbols.

- [ ] **Step 2: run the ctypes test** — `cd build && python3 -m pytest tests/bmbot/ctypes_test.py -v` (or the equivalent ctest invocation). It must pass.

- [ ] **Step 3: run the echo example** — `python3 bmbot/examples/echo.py`. It must print the ABI version and the open result without crash.

## Task 2: Python ctypes test for CI (already done)

The `tests/bmbot/ctypes_test.py` tests that the shared library loads and the basic ABI functions are callable. It is run by ctest on the plain Ubuntu CI image.

- [ ] **Step 1: verify the test passes** — `cd build && python3 -m pytest tests/bmbot/ctypes_test.py -v`.

- [ ] **Step 2: register it in CMake** — in `cmake/qt-app.cmake`, next to the other Python tests (around line 86, inside `if(Python3_Interpreter_FOUND)`):

```cmake
add_test(NAME bmbot-ctypes COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/bmbot/ctypes_test.py)
set_tests_properties(bmbot-ctypes PROPERTIES
  TIMEOUT 30 ENVIRONMENT "BMBOT_LIB=$<TARGET_FILE:libbmbot>")
```

- [ ] **Step 3: run it** — `ctest -R bmbot-ctypes --output-on-failure`.

## Task 3: Dockerfile and compose.yaml

**Files:**
- `docker/Dockerfile`
- `docker/compose.yaml`

- [ ] **Step 1: Dockerfile** — a multi-stage build. Build stage: `apt-get install` build tools, `cmake`, `ninja`, `pkg-config`, OpenSSL, libsodium, SQLCipher, secp256k1. Configure `-DYNOTBIT_QT=OFF`, `cmake --build .`. Runtime stage: `debian:bookworm-slim`, add user `bot`, copy `libbmbot.so`, `bmnode`, `chat_responder`, set `QT_QPA_PLATFORM=offscreen`, `EXPOSE 8444`, healthcheck (`chat_responder --check`), memory limit, read-only root with tmpfs `/tmp`, port 8444.

- [ ] **Step 2: compose.yaml** — service `bot` with `restart: unless-stopped`, `init: true`, `stop_grace_period: 30s`, a data volume for the node folder, the phrase as a secret file (`/.phrase`), memory limit (`mem_limit: 512m`), read-only root with tmpfs `/tmp`, port 8444 forwarded, health check `chat_responder --check`.

- [ ] **Step 3: build and run** — `docker build -t bmbot-test .` and `docker run --rm bbt-test`. The container should start, the health check should pass, and `SIGTERM` should gracefully stop it.

## Task 4: docs: what slice 6 adds

**Files:**
- `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md` (append "Notes after slice 6")
- `docs/chat-responder.md` (update node-starting and `--public` behaviour)
- this plan (a `## Results` section, written last, with the real numbers)

- [ ] **Step 1: append to the spec's notes** — add a "Notes after slice 6" section summarising what was added: `libbmbot` shared library, Python ctypes test, Docker + compose, echo example, and the acceptance criteria.

- [ ] **Step 2: update docs/chat-responder.md** — how the responder starts its node now (via `bmnode` / `YNOTBIT_NODE_BIN`, not via its own `--node` branch); what `--public` does (sets `linkOnly` in `desktop.ini`, which `Session::nodeConfig()` reads as `-b -B -e`); that `YNOTBIT_RESPONDER_PUBLIC` is gone.

- [ ] **Step 3: commit**.

```
docs: record what slice 6 adds

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 5: the gates

**Files:** commands and the results note.

- [ ] **Step 1: macOS, the Qt side and the core, in sequence.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake -S . -B build -G Ninja && cmake --build build --parallel 4
cd build && cctest --output-on-failure           # one more test: bmbot-ctypes
cd .. && cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure)
```

- [ ] **Step 2: Linux Qt, on big1** (serialise: it binds the same ports as the next step).

```sh
rsync -a --delete --exclude '/build' --exclude '/build-*' --exclude '/.tokensave' \
  --exclude '/reports' --exclude '/research_notes' ./ big1:w/ynotbit-slice6/
ssh big1 'cd ~/w/ynotbit-slice6 && cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/home/user/Qt/6.8.3/gcc_64 -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 16 && cd build && ctest --output-on-failure'
```

Do not touch the user's own `~/w/ynotbit` there.

- [ ] **Step 3: the Qt-free container.**

```sh
scripts/check-qt-free.sh big1
```

Expected `QT-FREE BUILD OK`, 5/5.

- [ ] **Step 4: the cross-open gate and the text gates.**

```sh
scripts/compat-check.sh SLICE6_BASE
scripts/update-error-catalog.py --check
scripts/check-translations.py
```

- [ ] **Step 5: the manual probe.** With `YNOTBIT_NODE_BIN` unset, start the app offscreen against a data directory with `peer=127.0.0.1:9, listen=false, linkOnly=true` and confirm with `ps -ww` that the node child is `build/bmnode`; then move `build/bmnode` aside, start it again, and confirm the app still runs (its own node) — the fallback. Record both.

- [ ] **Step 6: write `## Results`** into this plan: the gate numbers with the commit they describe, what the review found, and what stayed undone (the publishers' send path, `bmbot` full transport). Commit it as `docs: record what slice 6's gates ran`.

## Results

*(Written after execution, on the tree of the last commit of the slice.)*