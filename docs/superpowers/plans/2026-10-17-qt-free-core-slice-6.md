# Qt-free core, slice 6: bot platform (bmbot C ABI) — Implementation Plan

> Rewritten on 2026-10-18, after the slice was executed: the plan committed as 8da6d81 described a library that was a stub (`bmbot_open`
> returned 1), listed tasks "already done" that were not, and ended in a "Results" section that no run had produced. This is the plan
> as it was carried out, and its real results.

**Goal:** a C ABI library `libbmbot` over `bmclient` that a program in any language can run a Bitmessage bot on, `chat_responder` rewritten
without Qt as its reference consumer, a container image of them, and a responder running in it.

**Spec:** `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md` (§ The bot library, § chat_responder and the container,
§ Notes after slice 6 for the deviations).

**Global constraints** (as in the other slices): nothing in `bmclient/`, `bmbot/` or the responder includes or links Qt
(`scripts/check-qt-free.sh` is the gate); `NodeConfig`'s argv rows stay byte-identical (the only addition, `linkOnly`, is appended last and
off by default); no user-visible string changes in the Qt app (`scripts/update-error-catalog.py --check` and
`scripts/check-translations.py` pass unchanged); `clang-format` only on the files this slice wrote; nothing is committed, pushed, tagged or
released until the user asks.

## Tasks (all done; none committed)

- [x] **Task 1: `NodeConfig::linkOnly`.** `-b -B -e` after every other row for a bot that links to its peer alone
  (`bmclient/include/bmc/node_config.h`, `src/node_config.cpp`, one row in `tests/bmclient/node_config_test.cpp`).
- [x] **Task 2: the runtime.** `bmbot/bot.{h,cpp}`: `Options::fromJson` (config checked, a phrase file read), the data directory lock, the
  vault and the mailbox made or opened (the first responder's folders open as they were), `NodeSupervisor` + `NodeWatch` + a driver thread
  (`Session::tick` without the timer), events (notes, then the oldest unread chat or letter), `done`, `sendChat`, `sendLetter`, `state`,
  `status`, `close`, and the hang watch (`std::abort()` when the driver stalls, injectable for the test).
- [x] **Task 3: the ABI.** `bmbot/bmbot.h` (C-callable; the draft had everything inside `#ifdef __cplusplus`) and `bmbot/bmbot.cpp` (codes,
  ownership of strings). `bmbot/CMakeLists.txt`: the objects once, a shared and a static library, `cmake --install` of the shared one with
  `bmbot.h` and a pkg-config file.
- [x] **Task 4: the chat bot and the log, in std.** `tools/chat_bot.{h,cpp}` over a `ChatHost` interface; `tools/rotating_log.{h,cpp}`.
- [x] **Task 5: `chat_responder`, without Qt, on the C ABI.** The old flags, plus `--hang`, `--retention-mb`, `--retention-days`, `--node-bin`
  and `--check`; the Qt tool, its engine's Qt tests and its CMake lines are gone from `cmake/qt-app.cmake` and `tests/`.
- [x] **Task 6: tests.** `tests/bmbot/`: `bmbot_tests` (30 cases), `responder_e2e_test.py`, `ctypes_test.py`; `bmbot/examples/echo.py` is a
  working echo bot.
- [x] **Task 7: the image.** `docker/Dockerfile` (the recipe of `scripts/check-qt-free.sh`, then a slim runtime under `tini`),
  `docker/Dockerfile.dockerignore`, `docker/compose.yaml`, `scripts/build-image.sh`.
- [x] **Task 8: docs.** `docs/chat-responder.md`, `bmbot/README.md`, the spec's notes after slice 6, this plan.
- [x] **Task 9: a responder with a vanity address, running.** See "Deployment" below.

## Gates, on the tree as it was at the end

| Gate | Result |
|------|--------|
| macOS, Qt-free tree (`build-core`), `ctest` | 13 of 13 (231 s), including `bmbot` (30 cases), `responder-e2e` and `bmbot-ctypes` |
| macOS, Qt tree (`build`), `ctest` | 38 of 38 on the first full run (362 s): the 36 of before, less `chatbot`, plus `bmbot`, `responder-e2e` and `bmbot-ctypes`. On the last full run 37 of 38: `responder-e2e` read the first responder's status file before its ten-second refresh (a race in the test, which now waits for the counters); the three bmbot tests then passed in both trees, and six runs of the end-to-end test in a row |
| Linux, Debian bookworm container with no Qt (`scripts/check-qt-free.sh big1`) | 13 of 13 (251 s), `QT-FREE BUILD OK`; the first run of it found that `libbmbot.so` did not link (static libraries not position independent) |
| `scripts/update-error-catalog.py --check`, `scripts/check-translations.py` | pass (6 translations complete) |
| `-Wall -Wextra` on the new sources | no warnings |
| Image `docker/Dockerfile` built on big1 (x86_64) | 96.8 MB; runs as uid 10001 with a read-only root; healthy in 75 s on the public network |
| Stop, recreate, crash | `docker stop`: exit 0, "Bye.", no node left, 0.5 s; recreated on the same volume: same address; the responder killed inside: Docker started it again, same address, healthy in 15 s |
| Live | see Deployment |

`scripts/compat-check.sh` (vault and mailbox cross-open with the previous release) was not run: this slice writes and reads vaults and
mailboxes only through `bmc::Vault` and `bmc::Mailbox`, unchanged.

## Deployment (2026-10-18)

The address `BM-87echoAkzstiHuPGSN8PH88m4WPm4attre8` comes from a phrase `<secret>:17507166` found by `bm-vanity`, a copy of big1's
`tools/bm-shortaddr` that searches phrases at pair 0 for a word in the address (`~/w/bmecho-search` on big1; the tool is not in the tree). It
was checked three ways before it ran: the tool gives the same addresses as `bm-shortaddr`, it gives the three reference addresses of
`tests/bmclient/protocol_test.cpp`, and the real `chat_responder` prints the tool's address for a phrase. The image runs on the server `tos` as the
container `bmecho` (`/data/bmecho`: `run.sh`, `README.txt`, `secrets/phrase`, `data/`), `--restart unless-stopped`, 512 MB, 2 CPUs, read-only
root, no capabilities, `memlock` raised, port 8444 published and reachable from outside. A second responder on big1, with its own phrase and the
server as its peer, said hello to the address and got "You said: hello from ..." back in 25 s (getpubkey, pubkey, chat, ack, reply; the
deployed bot logged `calculating ack`, `calculating message`, `awaiting ack`, `acknowledged`). At that point the bot had 7 peers (4 out, 3 in),
0.9% of a CPU and 123 MiB.

## Results

*Written on the tree of 2026-10-18; nothing is committed.*

What stayed undone, and why: the Go example (no Go on the build machines; the ABI has nothing a cgo caller wraps); a CI job for the container
(the script exists); a responder that answers letters (the ABI hands them out; the bot sets them aside); Windows; the 24 h soak (the
deployment's first hours are above; its log and status file are in `/data/bmecho/data`); `announce_release` and `post_digest` on `bmnode`
(slice 5's work, which this slice did not need).
