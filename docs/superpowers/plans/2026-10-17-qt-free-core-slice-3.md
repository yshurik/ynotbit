# Qt-free core, slice 3: delivery — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `bmclient` holds the delivery layer — `bmc::Cache` (the node's object store, read), `bmc::Delivery` (plan, proof of work, spool, receipts, the incoming scan), `bmc::scanMailbox` and `bmc::Client` (which owns a `Vault`, a `Mailbox`, a `Cache` and a `Delivery` through shared pointers and holds the letter and chat rules). The Qt `bm::Cache`, `bm::Delivery`, `bm::scanMailbox` and `Session::tick` become thin wrappers over them. A letter and a chat travel between two Qt-free clients that exchange objects through two `objects.sqlite` files, with no node and no Qt.

**Architecture:** The same method as slices 1 and 2. This slice is mostly a **verbatim move** of `src/cache.cpp`, `src/scanner.cpp` and `src/delivery.cpp` (491 lines) into `bmclient/`, with `QString`→`std::string`, `QByteArray`→`Bytes`, `QVector`→`std::vector`, `QDateTime::currentSecsSinceEpoch`→`bmc::now` (or the `now` the caller passes), `QSaveFile`→`bmc::writeFileAtomic`, `QJsonDocument`→`bmc::json`, `QCryptographicHash`→`bmc::hash`, `QElapsedTimer`→`std::chrono::steady_clock`. What is **new** is `bmc::Client`: the scan loop and the chat and letter services of `Session`, in std, so `bmbot` can use them without Qt. The Qt veneer keeps every name and Qt signature; `Session` stops owning a `Cache` and a `Delivery` and drives a `bmc::Client` instead.

**Tech Stack:** C++20, std only, plus OpenSSL, libsodium, SQLCipher, libsecp256k1 and the notbit C code; the Qt 6.8 app (`bmcore`) links `bmclient`.

**Spec:** `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md`, "Order of work" item 3 and the sections "The Qt veneer" and "The client loop". Slice 2's plan ends with "Notes for slice 3"; this plan is the execution of those notes.

## Global Constraints

Copied from the spec; every task includes them.

- Nothing in `bmclient/` or `bmbot/` includes or links Qt; checked by `scripts/check-qt-free.sh` in a Qt-less container.
- The core is C++20, std only, plus OpenSSL, libsodium, SQLCipher, libsecp256k1 and the notbit C code.
- Bytes and text: `using Bytes = std::vector<uint8_t>`; `using ByteView = std::span<const uint8_t>`; text is UTF-8 `std::string` / `std::string_view`; addresses are `std::string`; integers are `int64_t` / `uint64_t`.
- Errors: `bmc::Error : std::runtime_error` with the exact English `what()` of today; no localized text in the core; `Session::errorText` translates `what()` in the `bm::Errors` context; `scripts/update-error-catalog.py` reads `src/*.cpp` (except `session.cpp`, `desktop_window.cpp`, `main.cpp`) and `bmclient/src/*.cpp`.
- Time: seconds since the epoch as `int64_t`. Code that can be handed the time takes `now`; the rest calls `bmc::now()`. Tests may install a clock with `bmc::setClockForTesting` (`bmc/clock.h`).
- **On-disk formats do not change.** The vault file, the mailbox `.bmmail`, the node's `objects.sqlite` (`NTB_OBJECT_DB_VERSION` 1, written only by the node), the spool `publish/<job>.object` and the relay's `receipts/<job>.json` keep their bytes and their names. The previous release opens what the new code writes and the other way round.
- The core never prints; no POSIX-only call in `bmclient` (it compiles on Linux, macOS and Windows/MSVC).
- The veneer keeps the names, the namespace (`bm`) and the Qt signatures of today's `src/cache.h`, `src/scanner.h`, `src/delivery.h`, `src/storage.h`; the app and every `ctest` suite compile against the same headers.
- Work happens on `feature/qt-free-core`; core tests live in `tests/bmclient/`.
- Nothing is committed or pushed unless the user has said so.

## Review Focus

The inputs the spec implies but a plain port could get wrong. Each line has a test in the task named in brackets.

1. **The object store is the node's, and a second reader must not disturb it.** `bm::Cache` opened `objects.sqlite` `SQLITE_OPEN_READWRITE` and never `CREATE` (only the node makes the file), removed the pre-0.5.2 `cache.sqlite` on construction, and refused a store whose `PRAGMA user_version` is not `NTB_OBJECT_DB_VERSION` (zero while the node is creating or emptying it). [Task 1: `a_missing_store_reads_as_empty`, `a_foreign_store_reads_as_empty`, `the_old_index_is_removed`]
2. **One commit per scan.** The scan stores letters and the checkpoint together: `Mailbox::Batch` wraps the whole pass and `commit()` runs once at the end; a crash re-inspects one batch, which is harmless because storing a letter twice is ignored. Objects that are not ours move only the checkpoint. `mailbox.advance(0)` starts the scan over. [Task 2: `an_oversized_object_is_passed_over`, `a_new_cache_or_identity_set_restarts_the_scan`; the batch itself: `a_scan_batch_commits_what_it_stored_or_leaves_it_out`]
3. **The delivery state machine, character for character.** The outbox states (`queued`, `awaiting_pubkey`, `awaiting_ack`, `calculating_message`, `calculating_ack`, `published`, `acknowledged`, `cancelled`, `failed`, `expired`), their English detail lines, and the job kinds (`getpubkey`, `pubkey`, `message`, `broadcast`, `ack-template`, `incoming-ack`) are stored in the mailbox and shown by the app; `tests/delivery_tests.cpp` pins them against the veneer. [Task 2: `an_unknown_recipient_asks_for_its_key`, `a_letter_reaches_the_other_side_and_its_acknowledgment_comes_back`; Task 7: the unchanged `delivery` ctest]
4. **The chat and letter rules move to the core, with their English text.** `sendChat` refuses a sender that is not a non-chan identity of the vault, a recipient that is not a valid address or is the sender, and a body that trims to empty or is over 8192 bytes, and queues with a 24-hour expiry; `sendLetter` refuses a sender that is not in the vault, a bad recipient for a direct letter, a subject with a line break, an empty body and a letter over `kMaxLetterText`. The messages become `bmc::require` text in `bmclient/src/client.cpp`, in the catalogue. [Task 3: `a_chat_needs_an_own_sender_and_a_real_recipient`, `a_letter_is_checked_before_it_is_queued`; Task 7: `update-error-catalog.py`]
5. **The revision hook and the batch do not move.** `Mailbox::messageRevision_` is bumped by the `sqlite3_update_hook` on exactly `messages, outbox, chat_messages, chat_outbox, settings, delivery_events` and by `close()`; the desktop's `Session::refresh` compares it to skip a reload. Slice 3 must leave that list and that hook alone while `Cache` and `Delivery` start driving the mailbox. [Task 3: `a_step_reports_the_mailbox_changing_under_it` pins the revision after a delivery write]

## Execution rules

- Run every command from `/Users/user/w/ynotbit`. In each shell first: `export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig`.
- Build directories: `build` (the existing Qt build, Release, Ninja) and `build-core` (Qt-free).
- Qt-free configure on this Mac: `cmake -S . -B build-core -G Ninja -DYNOTBIT_QT=OFF -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=/opt/local/libexec/openssl3`.
- The Mac stalls under swap pressure: keep tool calls short; build with `--parallel 4`; a `HUNG` desktop test with low CPU means a stalled machine, rerun it in the foreground when calm.
- Every new or changed C++ file is formatted with `clang-format -i <file>`. Code blocks are close to that style, not exact.
- **Commit steps run only if the user has allowed commits on this branch.** Commit messages are Conventional Commits with a prose body and end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- Never stage or commit `docs/superpowers/plans/2026-10-04-sqlite-object-store.md`, `docs/superpowers/plans/2026-10-15-qt-free-core-slice-1.md`, `docs/superpowers/work-state-2026-10-16.md`, `reports/` or `research_notes/`.
- A plan test that fails: fix the core, not the test, unless this plan says the test was wrong.
- **Do not run the Mac Qt `ctest -j2` and `scripts/compat-check.sh` at the same time**; the load times out `peer-connectivity` (70 s) and `desktop` (57 s).

## What is a move, what is new

`src/cache.cpp` (98 lines), `src/scanner.cpp` (45) and `src/delivery.cpp` (491) move verbatim except for the type substitutions named in the Architecture paragraph. Every SQL string, every state name, every detail line, the spool and receipt paths and the `NTB_OBJECT_DB_*` macros stay identical.

New code, printed in full below: the three core headers (`bmc/cache.h`, `bmc/delivery.h`, `bmc/client.h`), `bmc::Client`'s body, and the test scaffolding.

## File structure

New in `bmclient/`:

- `include/bmc/cache.h`, `src/cache.cpp` — `CachedObject`, `bmc::Cache`, `bmc::scanMailbox`.
- `include/bmc/delivery.h`, `src/delivery.cpp` — `bmc::Delivery`.
- `include/bmc/client.h`, `src/client.cpp` — `StepReport`, `bmc::Client`.

Changed in `bmclient/`:

- `include/bmc/updates.h` — gains `kNotifySetting` and `kLatestSetting` (the setting keys `Delivery` reads; the translated labels and the default subscriptions stay in `src/updates.cpp`).
- `CMakeLists.txt` — adds `src/cache.cpp src/delivery.cpp src/client.cpp`.

Changed in `src/`:

- `cache.h`, `cache.cpp` — `bm::Cache` holds a `std::shared_ptr<bmc::Cache>`; the Qt `CachedObject` struct stays.
- `scanner.h`, `scanner.cpp` — `bm::scanMailbox` converts and calls `bmc::scanMailbox`.
- `delivery.h`, `delivery.cpp` — `bm::Delivery` holds a `std::shared_ptr<bmc::Delivery>` and converts every argument.
- `updates.h` — the two setting keys derive from `bmc::updates`, one source of truth.
- `storage.h`, `storage.cpp` — `bm::Vault` holds a `std::shared_ptr<bmc::Vault>` (as `bm::Mailbox` already holds its core) and hands the same one to the `Client`.
- `session.h`, `session.cpp` — a `std::shared_ptr<bmc::Client>` replaces `cache_` and `delivery_`; `Session::tick`, `sendChat`, `chatState`, `unreadChats`, `markChatMessageRead` and `sendLetter` call it; `prunedUnread_` moves into the `Client`.

New in `tests/`:

- `tests/bmclient/cache_test.cpp`, `delivery_test.cpp`, `client_test.cpp`, `scenario_test.cpp`, `fake_node.h`.
- `tests/bmclient/CMakeLists.txt` — the four sources and `ntb_object_db` in the link line.

## Task 0: Preconditions and baseline

**Files:** none changed.

- [ ] **Step 1: Confirm the branch and a clean tree.**

Run:
```sh
cd /Users/user/w/ynotbit
git rev-parse --abbrev-ref HEAD        # feature/qt-free-core
git status --short                     # only untracked docs/, reports/, research_notes/
```
Expected: on `feature/qt-free-core`; the only entries are the untracked plan/work-state files and `reports/`, `research_notes/`. If a source file is modified, stop and ask.

- [ ] **Step 2: Record the baseline.** Slice 2 left 113 core tests and 29 Qt tests.

Run:
```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure | tail -3)
cmake --build build --parallel 4 && (cd build && ctest -j2 --output-on-failure | tail -3)
```
Expected: `0 tests failed out of 5` for `build-core` (the `bmclient` entry runs all 113 core cases; the other four are the notbit C suites) and `0 tests failed out of 29` for `build`.

- [ ] **Step 3: Note the pre-slice commit** (the compat gate builds it):

Run:
```sh
git rev-parse HEAD          # record as $SLICE3_BASE
```
Expected: a `0284f2d`-descended commit; write the hash down for Task 7.

## Task 1: `bmc::Cache` and `bmc::scanMailbox` — the node's object store

**Files:**
- Create: `bmclient/include/bmc/cache.h`, `bmclient/src/cache.cpp`
- Change: `bmclient/CMakeLists.txt`
- Create: `tests/bmclient/cache_test.cpp`
- Change: `tests/bmclient/CMakeLists.txt`

- [ ] **Step 1: Write the header.**

```cpp
// The node's object store (objects.sqlite), read in std. Only the node writes it; the desktop
// app and any bot read the same file with their own connection, which is why the file name, the
// version and the layout live in third_party/notbit/src/ntb-object-db.h.
#pragma once
#include <bmc/bytes.h>
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
namespace bmc {
class Mailbox;
class Vault;

struct CachedObject {
    int64_t sequence = 0;
    std::string hash; // lowercase hex, as Protocol::inventoryHash() spells it
    Bytes payload;
};

// Reads the node's object store (objects.sqlite). Only the node writes it.
class Cache {
    sqlite3 *db_ = nullptr;
    std::string root_;
    bool ready();

  public:
    explicit Cache(std::string root);
    ~Cache();
    Cache(const Cache &) = delete;
    Cache &operator=(const Cache &) = delete;
    std::vector<CachedObject> after(int64_t sequence, int limit = 32);
    bool hasAfter(int64_t sequence);
    int64_t firstSequence(); // 0 when nothing is stored
    std::string id();        // empty until the node has created its store
};

// The older, direct-message-only scan of the store. Delivery::scan() is the one the app and the
// bots run; this stays because the Qt suite (tests/lifecycle_tests.cpp) pins it.
int scanMailbox(Cache &cache, Mailbox &mailbox, const Vault &vault, int limit = 8);
} // namespace bmc
```

- [ ] **Step 2: Write `src/cache.cpp` as a move of `src/cache.cpp`.**

Copy `src/cache.cpp` and apply exactly these substitutions; the three SQL strings, the `SQLITE_OPEN_READWRITE` open, the `PRAGMA user_version` gate and the 1000 ms busy timeout do not change.

| `src/cache.cpp` | core |
|---|---|
| `#include "cache.h"` | `#include <bmc/cache.h>` |
| `#include <QFile>` | `#include <bmc/path.h>` and `<filesystem>` |
| `#include <stdexcept>` | `#include <bmc/error.h>` |
| `qint64` / `quint64` | `int64_t` / `uint64_t` |
| `QByteArray`, `QString` | `Bytes`, `std::string` |
| `QVector<CachedObject>` | `std::vector<CachedObject>` |
| `namespace bm` | `namespace bmc` |
| the local `struct Stmt` | `bmc::Statement` from `<bmc/sqlite.h>` (same `number`/`row`/`blob`/`text`, `blob` returns `Bytes`, `text` returns `std::string`) |
| `check(v)` with `"Network cache database error"` | `require(v, "Network cache database error")` — the string is already in the catalogue |
| `QFile::remove(root + "/cache.sqlite")` and the `-journal` sibling | `std::filesystem::remove(bmc::path(root + "/cache.sqlite"))` (and the same for `-journal`), wrapped so a failure is not an error |
| `QFile::exists(path)` | `std::filesystem::exists(bmc::path(path))` |
| `QFile::encodeName(path).constData()` | `path.c_str()` — `Mailbox::connect` opens SQLite the same way (`bmclient/src/mailbox.cpp:80`) |
| `s.num(0)`, `s.num(1, sequence)` | `s.number(0)`, `s.number(1, sequence)` |
| `QString::fromLatin1(s.blob(1).toHex())` | `bmc::hex(s.blob(1))` |
| `QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(p, n)))` | `s.text(0)` |
| `hash` column read as hex | `bmc::hex(s.blob(1))` |

The constructor keeps the `root_` by value (`explicit Cache::Cache(std::string root) : root_(std::move(root))`) and still removes the two pre-0.5.2 files first.

- [ ] **Step 3: Write `bmc::scanMailbox` as a move of `src/scanner.cpp`.**

Same substitutions, plus:
- `QStringList addresses` → `std::vector<std::string> addresses`; `addresses.sort()` → `std::sort(addresses.begin(), addresses.end())`; `addresses.join('\n')` → join with `'\n'` into a `std::string`.
- `QString::fromLatin1(QCryptographicHash::hash(...).toHex())` → `bmc::hex(bmc::hash(Digest::Sha256, bmc::view(joined)))` (include `<bmc/hash.h>`).
- `mailbox.bindIdentities(std::string, bool)`, `cache.after(...)`, `mailbox.advance(...)`, `mailbox.checkpoint()`, `mailbox.storeRouted(...)` are already the core's signatures; `Protocol::decodeMessage` stays (the older `Message`-returning decode the direct-message scan always used, unlike `Delivery::scan`'s `Wire::decodeMessage`); `Wire::header` unchanged.
- `NTB_OBJECT_DB_MAX_OBJECT_SIZE` still comes from `<ntb-object-db.h>`.

- [ ] **Step 4: Add the files to the build.**

In `bmclient/CMakeLists.txt`, add `src/cache.cpp` after `src/bytes.cpp` and keep the list one name per line as it is.

In `tests/bmclient/CMakeLists.txt`, add `cache_test.cpp` to `add_executable(bmclient_tests ...)` and add `ntb_object_db` to the test's link line so a test can write a store the way the node does:

```cmake
target_link_libraries(bmclient_tests PRIVATE bmclient ntb_object_db)
```

- [ ] **Step 5: Write `tests/bmclient/cache_test.cpp`.**

Use `bmc/test.h`'s harness as the other core tests do: `TEST(name) { … }`, `CHECK(cond)`, `CHECK_EQ(a, b)` (it splits on commas, so name a brace-init first) and `CHECK_THROWS(expr)`. There is no `REQUIRE`; a fixture that cannot be built fails through `CHECK`, and `bmc::test::readFile`/`writeFile` are there. Helpers:

```cpp
// Writes an object the way the node does: through its own store.
static void cacheObject(const std::string &root, ByteView payload, int64_t received) {
    std::filesystem::create_directories(bmc::path(root));
    auto *db = ntb_object_db_open(root.c_str());
    CHECK(db);
    const auto hash = bmc::unhex(bmc::Protocol::inventoryHash(payload));
    const bool saved = ntb_object_db_save(db, hash.data(), payload.data(), payload.size(), received);
    ntb_object_db_close(db);
    CHECK(saved);
}
```

Cases:
1. `a_missing_store_reads_as_empty` — a `Cache` over an empty directory: `after(0)`, `hasAfter(0)`, `firstSequence()`, `id()` are empty/0 and none throws.
2. `a_store_reads_back_in_order` — three objects saved with the node's writer; `after(0, 32)` returns them in `sequence` order with the node's hex hash (`Protocol::inventoryHash` of the payload, lower case) and the same payload bytes; `after(<second> )` returns only the third; `hasAfter(first)` is true and `hasAfter(last)` is false; `firstSequence()` is the first; `id()` is the store's `cache_id`.
3. `a_foreign_store_reads_as_empty` — an `objects.sqlite` made with plain `sqlite3` and `PRAGMA user_version = 99`: every read returns empty/0 and none throws.
4. `the_old_index_is_removed` — a directory with `cache.sqlite` and `cache.sqlite-journal`; constructing a `Cache` removes both, and the missing-file path does not throw.
5. `an_oversized_object_is_passed_over` — `scanMailbox` over a store holding one object larger than `NTB_OBJECT_DB_MAX_OBJECT_SIZE` returns 0 and leaves the checkpoint at that object's sequence (this is the store-on-other-people's-objects rule, so the checkpoint keeps moving).

- [ ] **Step 6: Build and run.**

```sh
cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure | tail -3)
```
Expected: `100% tests passed`; the new cases run inside the one `bmclient` entry.

- [ ] **Step 7: Commit.**

```
feat(core): the node's object store, read in std

bm::Cache read objects.sqlite through Qt types. The core now has the
same reader in std: bmc::Cache, and bmc::scanMailbox over it. The SQL,
the version gate and the pre-0.5.2 cleanup are unchanged.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 2: `bmc::Delivery` — the delivery engine

**Files:**
- Create: `bmclient/include/bmc/delivery.h`, `bmclient/src/delivery.cpp`
- Change: `bmclient/include/bmc/updates.h`, `bmclient/CMakeLists.txt`
- Create: `tests/bmclient/delivery_test.cpp`
- Change: `tests/bmclient/CMakeLists.txt`

- [ ] **Step 1: Move the two setting keys into the core.**

In `bmclient/include/bmc/updates.h` add, beside `publisherAddress`:

```cpp
// The setting keys Delivery reads. The translated labels and the default subscriptions stay in
// the Qt veneer (src/updates.cpp), because lupdate reads that file.
inline const std::string kNotifySetting = "updates:notify"; // "off" turns notices off
inline const std::string kLatestSetting = "updates:latest"; // newest announced version
```

- [ ] **Step 2: Write the header.**

```cpp
// The delivery engine: what the outbox still needs, one mined object at a time, the objects
// offered to the relay and the receipts read back, and the incoming scan. The rules, the state
// names and the receipts are src/delivery.cpp's, character for character.
#pragma once
#include <bmc/pow.h>
#include <cstdint>
#include <string>

namespace bmc {
class Cache;
class Mailbox;
class Vault;

class Delivery {
    std::string root_, mining_;
    ProofOfWork pow_;
    void plan(Mailbox &, const Vault &, int64_t time);
    void processJobs(Mailbox &, bool online, int64_t time);
    int catchUpBroadcasts(Cache &, Mailbox &, const std::string &publisher, int64_t time);

  public:
    explicit Delivery(std::string root) : root_(std::move(root)) {}
    void stop() {
        pow_.stop();
        mining_.clear();
    }
    void cancel(Mailbox &, const std::string &id);
    void retry(Mailbox &, const std::string &id);
    // Up to limit objects; with a budget, stops once that many milliseconds have gone (after at
    // least one object).
    int scan(Cache &, Mailbox &, const Vault &, int64_t now, int limit = 8, int budgetMs = 0);
    // Reads the kept broadcasts and public keys again (cheap: no message is tried against the
    // identities), e.g. for a recipient's key already here.
    static void rereadKept(Mailbox &);
    void tick(Mailbox &, const Vault &, bool online, int64_t now);
    bool working() const {
        return !mining_.empty();
    }
};
} // namespace bmc
```

`now` is threaded through `scan`/`tick`/`plan`/`processJobs`/`catchUpBroadcasts` so a test can drive the engine at a fixed time; the Qt veneer passes `bmc::now()`. `retry` keeps using `bmc::now()` (it stamps `now() + 4 * 86400`, exactly as today).

- [ ] **Step 3: Move `src/delivery.cpp`, with the substitutions below.**

Everything named in Task 1's table applies, plus:

| `src/delivery.cpp` | core |
|---|---|
| `static qint64 now()` and its uses | the `now` argument of the enclosing function (`bmc::now()` only in `retry`) |
| `quint64` | `uint64_t` |
| `QString`/`QByteArray`/`QStringList` | `std::string`/`Bytes`/`std::vector<std::string>` |
| `QFile::remove(root_ + "/publish/" + j.id + ".object")` | `std::filesystem::remove(bmc::path(root_ + "/publish/" + j.id + ".object"))` (same for `receipts/*.json`) |
| `QDir().mkpath(root_ + "/publish")` | `std::filesystem::create_directories(bmc::path(root_ + "/publish"))` |
| `QSaveFile out(spool); if (!out.open(...) or `out.write(...)` short or `!out.commit()`) throw std::runtime_error("Cannot queue encrypted object for relay");` | `try { bmc::writeFileAtomic(bmc::path(spool), j.payload); } catch (const Error &) { throw Error(Code::Io, "Cannot queue encrypted object for relay"); }` — the English text and its catalogue line do not change |
| `QFile f(receipt); f.open(ReadOnly); QJsonDocument::fromJson(f.read(4096)).object()` | read the file (`bmc::openFile` or `std::ifstream`) up to 4096 bytes, then `bmc::json::parse(text)`; `result.value("state").toString()` → `doc->at("state").toString()`, `result.value("hash").toString()` → `doc->at("hash").toString()`, both `""` when the document does not parse |
| `QCryptographicHash::hash(entries.join('\n').toUtf8(), Sha256).toHex()` | `bmc::hex(bmc::hash(Digest::Sha256, bmc::view(join(entries, '\n'))))` |
| `QCryptographicHash::hash(d.message.from + '\n' + d.message.to + '\n' + ackToken, Sha256).toHex()` | the same with `bmc::hash`, hashing the `Bytes` of `from + "\n" + to + "\n" + ackToken` |
| `updates::kLatestSetting`, `updates::kNotifySetting` | `updates::kLatestSetting`, `updates::kNotifySetting` from `bmc/updates.h` |
| `updates::publisherAddress()` | `updates::publisherAddress()` (core, returns `std::string`) |
| `QElapsedTimer budget; budget.start(); budget.elapsed() < 50` | `std::chrono::steady_clock::time_point` and `elapsed_ms(...) < 50` (a file-local helper) |
| `QElapsedTimer spent; spent.elapsed() >= budgetMs` | the same helper |
| `randombytes_buf(item.ackToken.data(), 32)` | `#include <sodium.h>` and the same call (libsodium is already a dependency) |
| `QString::number(j.expires)` | `std::to_string(j.expires)` |
| `QString::fromUtf8(e.what())` | `e.what()` |
| `m.setSetting("pubkey:" + j.address, ...)` | the same, `std::string` |
| `for (const auto &sub : m.subscriptions()) if (auto decoded = Wire::decodeBroadcast(data, sub.address))` | unchanged, `std::string` |
| `Mailbox::Batch batch(m); ... batch.commit();` | `Mailbox::Batch batch(m); ... batch.commit();` — unchanged, it is already the core's nested class |
| `ProofOfWork::valid(d.acknowledgment, now())` | `ProofOfWork::valid(d.acknowledgment, time)` |
| the file-local `kSubscriptionsSetting`, `kCatchUpFrom`, `kCatchUpTo` | unchanged, still file-local `const std::string` |
| `namespace bm` | `namespace bmc` |

`plan`, `processJobs` and `catchUpBroadcasts` keep their bodies line for line; only the argument list gains `int64_t` and the types change. `tick` becomes:

```cpp
void Delivery::tick(Mailbox &m, const Vault &v, bool online, int64_t now) {
    if (!m.isOpen() || !v.unlocked()) {
        stop();
        return;
    }
    plan(m, v, now);
    processJobs(m, online, now);
    m.endChatDeliveries(now);
}
```

and `scan` starts `if (!v.unlocked() || !m.isOpen()) return 0;` and uses `time` where it used `now()`.

- [ ] **Step 4: Add the file to the build** (`src/delivery.cpp` in `bmclient/CMakeLists.txt`, alphabetical).

- [ ] **Step 5: Write `tests/bmclient/delivery_test.cpp`.**

Port the delivery half of `tests/delivery_tests.cpp` to the core: same fixtures (a vault with one identity, a mailbox, an object store written with `ntb_object_db_*`, a `transfer()` that reads `publish/*.object`, calls `ProofOfWork::valid`, saves the object into both stores, writes the receipt JSON and removes the spool file), same assertions, `bmc::` types, a fixed `now` instead of `QDateTime::currentSecsSinceEpoch()`. Cases:

1. `an_unknown_recipient_asks_for_its_key` — a queued letter to an address with no key becomes `awaiting_pubkey` with exactly one `getpubkey` job, and `Wire::requestsIdentity(job.payload, identity)` is true.
2. `a_receipt_offers_the_object_and_an_acknowledgment_finishes_it` — the full two-mailbox loop: B's pubkey arrives, A plans the message, the object is mined and offered, the receipt sets `published`/`awaiting_ack` and writes the object hash, B decodes the letter into `Inbox`, B's acknowledgment travels back, A's outbox becomes `acknowledged` and the letter moves to `Sent`.
3. `the_same_object_is_not_stored_twice` — a second scan over the same store leaves one letter.
4. `a_cancelled_letter_loses_its_jobs` and `a_retried_letter_is_queued_again` — `cancel` and `retry` with the job/spool cleanup.
5. `a_subscribed_broadcast_is_stored` — a broadcast from a subscribed address is decoded and stored in `Broadcasts`, and a publication from the publisher address records `updates:latest` when notices are on and not when they are off.
6. `the_catch_up_reads_kept_broadcasts` — the subscription added after the scan is found by `rereadKept` + `catchUpBroadcasts` without moving the checkpoint.
7. `an_oversized_object_moves_the_checkpoint_only` — a payload over `NTB_OBJECT_DB_MAX_OBJECT_SIZE` is passed over and only `advance`s.

Keep each case's proof of work small: the fixtures' public keys carry `nonceTrials`/`extraBytes` of 1000, which is the floor `addJob` clamps to anyway.

- [ ] **Step 6: Build, run, format.**

```sh
cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure | tail -3)
clang-format -i bmclient/include/bmc/delivery.h bmclient/src/delivery.cpp tests/bmclient/delivery_test.cpp
```
Expected: all green. If a case is slow, the proof of work is the cost, not a hang.

- [ ] **Step 7: Commit.**

```
feat(core): the delivery engine, moved out of Qt

src/delivery.cpp's plan, proof of work, spool, receipts and incoming
scan are bmc::Delivery now, in std. The state names, the detail lines,
the job kinds and the spool layout are unchanged; the time is an
argument so a test can drive the engine at a fixed instant.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 3: `bmc::Client` — one vault, one mailbox, one loop

**Files:**
- Create: `bmclient/include/bmc/client.h`, `bmclient/src/client.cpp`
- Change: `bmclient/CMakeLists.txt`
- Create: `tests/bmclient/client_test.cpp`
- Change: `tests/bmclient/CMakeLists.txt`

- [ ] **Step 1: Write the header.**

```cpp
// The client loop: a vault, a mailbox, the node's object store and the delivery engine, with the
// rules the desktop Session had, in std. The Qt veneer (src/session.cpp) keeps the QTimer, the
// QProcess and the signals and calls this.
#pragma once
#include <bmc/cache.h>
#include <bmc/delivery.h>
#include <bmc/mailbox.h>
#include <bmc/records.h>
#include <bmc/vault.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bmc {
// What one step did, for the caller's status line.
struct StepReport {
    bool open = false;     // the mailbox is open
    bool upToDate = false; // no cached object is after the checkpoint
    bool working = false;  // proof of work is being prepared
    bool pruned = false;   // retention removed objects this mailbox never read
    int64_t checkpoint = 0;
};

class Client {
    std::shared_ptr<Vault> vault_;
    std::shared_ptr<Mailbox> mailbox_;
    std::shared_ptr<Cache> cache_;
    std::shared_ptr<Delivery> delivery_;
    std::string boundCacheId_;
    bool prunedUnread_ = false;

  public:
    Client(std::shared_ptr<Vault> vault, std::shared_ptr<Mailbox> mailbox, std::string root);
    Vault &vault() const { return *vault_; }
    Mailbox &mailbox() const { return *mailbox_; }
    Cache &cache() const { return *cache_; }
    Delivery &delivery() const { return *delivery_; }
    const std::shared_ptr<Vault> &vaultPtr() const { return vault_; }
    const std::shared_ptr<Mailbox> &mailboxPtr() const { return mailbox_; }
    // The mailbox records the cache it is bound to; a new id (the node recreated its store)
    // binds it again, so the scan starts over.
    void bindMailboxToCache();
    // One turn: bind, note pruned objects, scan with a time budget (512 objects, 150 ms), tick
    // the delivery engine. online false: nothing is offered to the relay.
    StepReport step(int64_t now, bool online);
    // The chat and letter services, with the rules Session had. Each throws bmc::Error with the
    // English text the catalogue lists; the caller translates. sendChat returns the new id.
    std::string sendChat(const std::string &from, const std::string &peer, const std::string &body,
                         int64_t now);
    std::string chatState(const std::string &id) const;
    std::vector<ChatMessage> unreadChats(int limit) const;
    bool markChatMessageRead(const std::string &id);
    // Queues a draft as an outgoing letter; the recipient's key may already be kept, so the kept
    // broadcasts and keys are read again.
    void sendLetter(const std::string &id, int64_t now);
};
} // namespace bmc
```

- [ ] **Step 2: Write `src/client.cpp`.**

```cpp
Client::Client(std::shared_ptr<Vault> vault, std::shared_ptr<Mailbox> mailbox, std::string root)
    : vault_(std::move(vault)), mailbox_(std::move(mailbox)), cache_(std::make_shared<Cache>(root)),
      delivery_(std::make_shared<Delivery>(std::move(root))) {}

void Client::bindMailboxToCache() {
    boundCacheId_ = cache_->id();
    mailbox_->bindCache(boundCacheId_);
}

StepReport Client::step(int64_t now, bool online) {
    StepReport report;
    if (!mailbox_->isOpen())
        return report;
    report.open = true;
    // A new id means the node created or emptied its store: read it from the start.
    if (cache_->id() != boundCacheId_)
        bindMailboxToCache();
    if (cache_->firstSequence() > mailbox_->checkpoint() + 1)
        prunedUnread_ = true;
    // A time budget, not a count: catching up (a new identity, first sync) goes as fast as the
    // window can spare, ~150 ms per tick.
    delivery_->scan(*cache_, *mailbox_, *vault_, now, 512, 150);
    delivery_->tick(*mailbox_, *vault_, online, now);
    report.checkpoint = mailbox_->checkpoint();
    report.upToDate = !cache_->hasAfter(report.checkpoint);
    report.working = delivery_->working();
    report.pruned = prunedUnread_;
    return report;
}

std::string Client::sendChat(const std::string &from, const std::string &peer,
                             const std::string &body, int64_t now) {
    require(mailbox_->isOpen(), "Open a mailbox first");
    bool own = false;
    for (const auto &identity : vault_->identities())
        if (!identity.chan && identity.address == from)
            own = true;
    require(own, "Choose a sender from this vault");
    require(Wire::validAddress(peer) && peer != from,
            "Enter a valid Bitmessage recipient address");
    require(!trimmed(body).empty() && body.size() <= size_t(kMaxChatBodyBytes),
            "Chat message must be between 1 and 8192 bytes");
    const auto id = mailbox_->queueChat(from, peer, body, now + kChatTtlSeconds);
    Delivery::rereadKept(*mailbox_);
    return id;
}

std::string Client::chatState(const std::string &id) const {
    if (!mailbox_->isOpen() || id.empty())
        return {};
    try {
        return mailbox_->chatState(id);
    } catch (const std::exception &) {
        return {};
    }
}

std::vector<ChatMessage> Client::unreadChats(int limit) const {
    if (!mailbox_->isOpen())
        return {};
    try {
        return mailbox_->unreadChats(limit);
    } catch (const std::exception &) {
        return {};
    }
}

bool Client::markChatMessageRead(const std::string &id) {
    if (!mailbox_->isOpen() || id.empty())
        return false;
    try {
        return mailbox_->markChatMessageRead(id);
    } catch (const std::exception &) {
        return false;
    }
}

void Client::sendLetter(const std::string &id, int64_t now) {
    require(mailbox_->isOpen(), "Open a mailbox first");
    const auto m = mailbox_->message(id);
    bool own = false;
    for (const auto &i : vault_->identities())
        if (i.address == m.from)
            own = true;
    require(own, "Choose a sender from this vault");
    const auto kind = mailbox_->setting("draftkind:" + id, "direct");
    require(kind == "broadcast" || Wire::validAddress(m.to),
            "Enter a valid Bitmessage recipient address");
    require(m.subject.find_first_of("\r\n") == std::string::npos, "Subject must be one line");
    require(!trimmed(m.body).empty(), "Write a message before sending");
    require(letterTextBytes(m.subject, m.body) <= kMaxLetterText, "Letter is too large");
    mailbox_->queueDraft(id, kind, now + 4 * 86400);
    Delivery::rereadKept(*mailbox_); // the recipient's key may already be kept
}
```

Notes the executor needs:

- `trimmed()` is a file-local helper: strip ASCII white space from both ends, then test `empty()` — `QString::trimmed` trims Unicode white space, but the body has already been through `QString::toUtf8` at the veneer, so ASCII trimming on the UTF-8 bytes reproduces the current decision for every input the tests use. If a divergence shows up, use `bmc::text`'s helper instead of hand-rolling.
- `Wire::validAddress` is `bmc::Wire::validAddress`; `letterTextBytes`, `kMaxLetterText` come from `bmc/identity.h`; `kChatTtlSeconds`, `kMaxChatBodyBytes` from `bmc/chat_format.h`; `require` from `bmc/error.h`; `Message`'s fields `from`, `to`, `subject`, `body` are `std::string`.
- `"Letter is too large"` is already in the catalogue (`Mailbox::queueDraft` raises it today); `"Open a mailbox first"`, `"Choose a sender from this vault"`, `"Enter a valid Bitmessage recipient address"`, `"Chat message must be between 1 and 8192 bytes"`, `"Subject must be one line"` and `"Write a message before sending"` move from `src/session.cpp` into the catalogue in Task 7.

- [ ] **Step 3: Add `src/client.cpp` to `bmclient/CMakeLists.txt`** and `client_test.cpp` to `tests/bmclient/CMakeLists.txt`.

- [ ] **Step 4: Write `tests/bmclient/client_test.cpp`.**

A fixture helper builds a vault with one identity and a mailbox (the same `tests/bmclient/storage_fixtures.h` helpers `mailbox_test.cpp` uses), plus a temporary directory for the node's store. Cases:

1. `a_step_with_no_mailbox_reports_nothing` — `step(now, false).open` is false and nothing is touched.
2. `a_step_scans_the_store` — three objects in the store, one a letter to the identity: the first `step` stores it, moves the checkpoint, `upToDate` is true on the second call, and `started` starts over on `mailbox.advance(0)`.
3. `a_step_reports_pruned_objects` — an object store whose `firstSequence()` is past the checkpoint sets `pruned`.
4. `a_step_reports_what_changed` — after a delivery write the `Mailbox::messageRevision()` differs from before the step (the `messages`/`outbox`/`settings` hook still fires through the new path).
5. `a_chat_needs_an_own_sender_and_a_real_recipient` — `sendChat` throws `bmc::Error` with each of the four English texts: no mailbox open, a chan sender, a bad recipient, a body that trims to empty; a good call returns an id and `chatState(id)` is `queued`.
6. `a_chat_is_a_service_a_bot_can_poll` — an incoming chat is read back by `unreadChats(10)` with `local`, `peer`, `body` and a growing `seqno`, and `markChatMessageRead(id)` is true once and false after.
7. `a_letter_is_checked_before_it_is_queued` — a draft whose subject has a newline, one whose body is blank, one whose sender is not in the vault, one with a bad recipient and one over `kMaxLetterText` all throw the English text; a good one is `queued` in the outbox and `Delivery::rereadKept` was called (the kept-key setting moved if anything was kept).

- [ ] **Step 5: Build, run, format.**

```sh
cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure | tail -3)
```

- [ ] **Step 6: Commit.**

```
feat(core): a client that owns the vault, mailbox, cache and delivery

bmc::Client is the desktop client loop without Qt: one step that binds
the mailbox to the node's store, scans with a time budget and ticks the
delivery engine, and the chat and letter services with the rules
Session had. Its English error text joins the catalogue.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 4: The Qt veneer — `Cache`, `scanMailbox`, `Delivery`, the update keys

**Files:**
- Change: `src/cache.h`, `src/cache.cpp`, `src/scanner.h`, `src/scanner.cpp`, `src/delivery.h`, `src/delivery.cpp`, `src/updates.h`
- Change: `src/storage.h`, `src/storage.cpp`

- [ ] **Step 1: `bm::Cache`.** Keep `struct CachedObject` as it is (`qint64`, `QString`, `QByteArray`) — `src/delivery.h` and the tests use it. Change the class to

```cpp
class Cache {
    std::shared_ptr<bmc::Cache> core_;

  public:
    explicit Cache(const QString &root);
    Cache(std::shared_ptr<bmc::Cache> core) : core_(std::move(core)) {}
    ~Cache() = default;
    Cache(const Cache &) = delete;
    Cache &operator=(const Cache &) = delete;
    bmc::Cache &core() const { return *core_; }
    const std::shared_ptr<bmc::Cache> &corePtr() const { return core_; }
    QVector<CachedObject> after(qint64 sequence, int limit = 32);
    bool hasAfter(qint64 sequence);
    qint64 firstSequence();
    QString id();
};
```

and `src/cache.cpp` becomes the four conversions plus `bridge::fromStd`/`bridge::toStd` from `src/qt_bridge.h`. The header keeps `<QByteArray>`, `<QString>` and `<QVector>` for `CachedObject`, drops `struct sqlite3`, and includes `<bmc/cache.h>` instead of `<sqlcipher/sqlite3.h>`.

- [ ] **Step 2: `bm::scanMailbox`.** `src/scanner.cpp` becomes

```cpp
int scanMailbox(Cache &cache, Mailbox &mailbox, const Vault &vault, int limit) {
    return bmc::scanMailbox(*cache.core(), *mailbox.core(), *vault.core(), limit);
}
```

which needs `core()` accessors: add `bmc::Cache &bmc::Cache`-handing members to the two wrappers — `Cache::core()` (private-to-friend is awkward here, so make `core()` public and const) and the same on `Vault`.

- [ ] **Step 3: `bm::Delivery`.**

```cpp
class Delivery {
    std::shared_ptr<bmc::Delivery> core_;

  public:
    explicit Delivery(QString root);
    ~Delivery();
    Delivery(const Delivery &) = delete;
    Delivery &operator=(const Delivery &) = delete;
    void stop();
    void cancel(Mailbox &, const QString &id);
    void retry(Mailbox &, const QString &id);
    int scan(Cache &, Mailbox &, const Vault &, int limit = 8, int budgetMs = 0);
    static void rereadKept(Mailbox &);
    void tick(Mailbox &, const Vault &, bool online);
    bool working() const;
};
```

`Delivery(QString root)` builds `bmc::Delivery(bridge::toStd(root))`; `tick` calls `core_->tick(..., bmc::now())`; `scan` calls `core_->scan(*cache.core(), *mailbox.core(), *vault.core(), bmc::now(), limit, budgetMs)`. Keep `#include "pow.h"` in `src/delivery.h`: `tests/delivery_tests.cpp` uses `ProofOfWork::valid` through it.

- [ ] **Step 4: the update keys.** In `src/updates.h`:

```cpp
inline const QString kNotifySetting = QString::fromStdString(bmc::updates::kNotifySetting);
inline const QString kLatestSetting = QString::fromStdString(bmc::updates::kLatestSetting);
```

with `#include <bmc/updates.h>` at the top. The other Qt keys and `defaultSubscriptions()` stay where they are.

- [ ] **Step 5: `bm::Vault` shares its core.** In `src/storage.h`:

```cpp
class Vault {
    std::shared_ptr<bmc::Vault> core_ = std::make_shared<bmc::Vault>();

  public:
    Vault() = default;
    explicit Vault(std::shared_ptr<bmc::Vault> core) : core_(std::move(core)) {}
    bmc::Vault &core() const { return *core_; }
    const std::shared_ptr<bmc::Vault> &corePtr() const { return core_; }
    ...
};
```

and in `src/storage.cpp` change the 15 `core_.` to `core_->`. `bm::Mailbox` gains `bmc::Mailbox &core() const` and `const std::shared_ptr<bmc::Mailbox> &corePtr() const` beside its existing `core_`.

- [ ] **Step 6: Build the Qt app and run its tests.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build --parallel 4 && (cd build && ctest -j2 --output-on-failure | tail -5)
```
Expected: `0 tests failed out of 29` — `delivery`, `lifecycle`, `outbox` and `wire` are the ones that exercise the veneer.

- [ ] **Step 7: Commit.**

```
refactor(qt): cache, delivery and scanner as wrappers over the core

bm::Cache, bm::Delivery and bm::scanMailbox keep their Qt signatures and
hand every call to bmc; bm::Vault holds its core through a shared
pointer, as bm::Mailbox already did, so a Session and a bmc::Client will
share one object.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 5: `Session` drives a `bmc::Client`

**Files:**
- Change: `src/session.h`, `src/session.cpp`

- [ ] **Step 1: Replace the two members.** In `src/session.h`, drop `std::unique_ptr<Cache> cache_;`, `std::unique_ptr<Delivery> delivery_;`, `QString boundCacheId_;` and `bool prunedUnread_ = false;`, and add, **before** `Vault vault_;`:

```cpp
std::shared_ptr<bmc::Vault> vaultCore_ = std::make_shared<bmc::Vault>();
std::shared_ptr<bmc::Mailbox> mailboxCore_ = std::make_shared<bmc::Mailbox>();
std::shared_ptr<bmc::Client> client_;
```

so the wrappers share the cores:

```cpp
Vault vault_{vaultCore_};
Mailbox mailbox_{mailboxCore_};
```

Include `<bmc/client.h>`.

- [ ] **Step 2: Construct the client in the constructor body** (it needs `root_`, which is a constructor argument):

```cpp
client_ = std::make_shared<bmc::Client>(vaultCore_, mailboxCore_, bridge::toStd(root_));
```

replacing the two `std::make_unique` lines (`src/session.cpp:94-95`). `ProofOfWork::setGpuEnabled(...)` at line 107 stays.

- [ ] **Step 3: Point every use at the client.**

- `bindMailboxToCache()` becomes `client_->bindMailboxToCache();` (its only caller is `tick` and the two open paths).
- `delivery_->stop()` → `client_->delivery().stop()` (five call sites: `createMailbox`, `openMailboxPath`, `lock`, `createVault`'s failure path, `~Session`).
- `delivery_->retry(mailbox_, id)` → `client_->delivery().retry(mailbox_, bridge::toStd(id))`; `cancel` likewise.
- `cache_->id()`, `cache_->firstSequence()`, `cache_->hasAfter(...)` in `tick` are gone: `tick` calls `client_->step(now, !offline_ && node_.state() == QProcess::Running)` and reads the `StepReport`:

```cpp
const auto report = client_->step(bmc::now(), !offline_ && node_.state() == QProcess::Running);
if (report.open) {
    refresh();
    activity_ = report.pruned
                    ? tr("Retention cleanup removed older objects · Some older letters may no "
                         "longer be recoverable")
                    : report.upToDate ? tr("Mailbox up to date with retained objects")
                                      : tr("Inspecting cached objects · checkpoint %1")
                                            .arg(qint64(report.checkpoint));
}
if (report.working)
    activity_ = tr("Preparing outgoing proof of work · locking pauses preparation");
```

The order of the two `activity_` assignments is the one today's code has (`working` wins over the checkpoint line, `pruned` wins over both).

- [ ] **Step 4: Point the services at the client.**

`sendChat`:

```cpp
bool Session::sendChat(QString from, QString peer, QString body, QString *id) {
    bool queued = false;
    attempt([&] {
        const auto queuedId = client_->sendChat(bridge::toStd(from), bridge::toStd(peer),
                                                bridge::toStd(body), bmc::now());
        if (id)
            *id = bridge::fromStd(queuedId);
        refresh();
        queued = true;
    });
    return queued;
}
```

`chatState`, `unreadChats` and `markChatMessageRead` keep their Qt types and their `try`/`catch` and call `client_->…` (`unreadChats` converts each `bmc::ChatMessage` to the same `QVariantMap` with `id`, `local`, `peer`, `body`, `seqno`, `received`).

`sendLetter` keeps the formatted size pre-check it has today (the UI wording `"This letter is %1; Bitmessage carries at most %2…"` with `QLocale().formattedDataSize`), then calls `client_->sendLetter(bridge::toStd(id), bmc::now())` for the rest of the rules and the queueing. The `"Open a mailbox first"` check and the `m.from`/`m.to` reads stay in the wrapper.

- [ ] **Step 5: Build the app and the world.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build --parallel 4 && (cd build && ctest -j2 --output-on-failure | tail -5)
cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure | tail -3)
```
Expected: 29/29 Qt and the core entry green. `desktop` and `peer-connectivity` are the slow ones; run them in the foreground if the machine is loaded.

- [ ] **Step 6: Commit.**

```
refactor(qt): Session drives a bmc::Client

Session no longer owns a Cache and a Delivery: it builds one bmc::Client
from shared cores and calls step() from its timer, and the chat and
letter methods call the client's services. The QTimer, the QProcess and
the signals stay in Session.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 6: Two clients over a fake node

**Files:**
- Create: `tests/bmclient/fake_node.h`, `tests/bmclient/scenario_test.cpp`
- Change: `tests/bmclient/CMakeLists.txt`

- [ ] **Step 1: Write `tests/bmclient/fake_node.h`.**

```cpp
// A fake node: two directories, each with its own objects.sqlite, and the relay's
// work -- what one client spooled into `publish/` is offered to the other side's
// store, and the receipt written, exactly as tests/delivery_tests.cpp's transfer().
#pragma once
#include <bmc/cache.h>
#include <bmc/protocol.h>
#include <bmc/pow.h>
#include <bmc/wire.h>
#include <ntb-object-db.h>
#include <filesystem>
#include <string>

struct FakeNode {
    std::string a, b;
    int64_t now = 0;

    // Writes an object the way the node does: through its own store.
    static void cacheObject(const std::string &root, ByteView payload);
    // Offers every spooled object of `from` to `to`, writes the receipt, removes the spool file.
    int deliver(const std::string &from, const std::string &to);
};
```

`cacheObject` is Task 1's helper (`ntb_object_db_open`/`_save`/`_close`, hash from `Protocol::inventoryHash`); `deliver` walks `from + "/publish"` for `*.object`, requires `ProofOfWork::valid(object, now)`, saves it into `to`'s and `from`'s store, writes `from + "/receipts/<id>.json"` with `{"state":"offered","hash":"<inventoryHash>"}` through `bmc::writeFileAtomic`, and removes the spool file. A header-only helper is fine; if it grows, a `.cpp` is cleaner — either way it joins the test target.

- [ ] **Step 2: Write `tests/bmclient/scenario_test.cpp`.**

The whole slice in one test, with no Qt and no node:

```cpp
// Two clients, two vaults, two mailboxes, two object stores, and the fake relay between them.
// This is the mobile/headless shape the whole refactor exists for.
```

1. `a_letter_crosses_between_two_clients` — A and B each get a vault with one identity and a mailbox bound to their own store; A saves a draft to B and queues it (`bm::Mailbox`-equivalent: `bmc::Mailbox::saveDraft` + `queueDraft`). Then, at a fixed `now` that advances by a minute per round, up to 12 rounds of `deliver(a,b)` → `b.step` → `deliver(b,a)` → `a.step`. Assert: B's inbox holds exactly one letter with A's subject and body, in `Inbox`; A's outbox reaches `acknowledged`; the whole thing needed the pubkey round trip (assert that after the first round A's outbox was `awaiting_pubkey` with a `getpubkey` job, so the test would fail if the key exchange regressed).
2. `a_chat_crosses_and_is_answered` — A `sendChat` to B; the loop above; B's `unreadChats(10)` holds it with A's address; `markChatMessageRead` returns true then false; B `sendChat` back; A's `unreadChats(10)` holds it.
3. `a_broadcast_reaches_a_subscriber` — B subscribes to A's address; A `sendLetter` with `draftkind:broadcast`; the loop; B holds it in `Broadcasts`.
4. `a_second_scan_stores_nothing_twice` — the loop runs again with no new objects: both mailboxes keep one letter each and their checkpoints do not move.

Keep the loop bounded (a `for` with a hard limit and a `CHECK` on the final state) so a regression fails instead of spinning.

- [ ] **Step 3: Add the sources to `tests/bmclient/CMakeLists.txt`** (`scenario_test.cpp`; `fake_node.h` needs no entry unless it becomes a `.cpp`). `ntb_object_db` is already linked from Task 1.

- [ ] **Step 4: Build and run.** The proof of work makes this the slowest core test; a minute is possible on a loaded Mac.

```sh
cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure --timeout 300 | tail -3)
```
Expected: all green.

- [ ] **Step 5: Commit.**

```
test(core): two clients over a fake node

Two vaults, two mailboxes and two objects.sqlite files, with the
relay's work done by the test: a letter goes A to B and its
acknowledgment comes back, a chat is answered, a broadcast reaches a
subscriber. No Qt, no node.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Task 7: Gates, catalogue and notes

**Files:**
- Change: `src/i18n/error_catalog.cpp` (generated), `bmclient/README.md`, `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md` ("Notes after slice 3").

- [ ] **Step 1: Regenerate the error catalogue.**

```sh
python3 scripts/update-error-catalog.py && python3 scripts/update-error-catalog.py --check
git diff --stat src/i18n/error_catalog.cpp
```
Expected: the messages `bmc::Client` and `bmc::Delivery` raise are listed. **Do not run `update_translations`**: the moved strings (`"Choose a sender from this vault"`, `"Enter a valid Bitmessage recipient address"`, …) are still in the `.ts` files under `bm::Session` and are still translated there, so `check-translations.py` passes; running lupdate would mark them vanished and demand new translations for the `bm::Errors` context, which is a release-time job, not this slice's.

- [ ] **Step 2: Translations.**

```sh
python3 scripts/check-translations.py
```
Expected: `PASS: 6 translations complete`.

- [ ] **Step 3: The compat gate** (no on-disk format changed, so it must pass unchanged):

```sh
SLICE3_BASE=<the hash from Task 0 Step 3> scripts/compat-check.sh
```
Expected: PASS, the previous release reads this code's vault and mailbox and the other way round.

- [ ] **Step 4: The Qt-free container gate.**

```sh
scripts/check-qt-free.sh big1
```
Expected: `QT-FREE BUILD OK`. If it fails on a Qt include, a core file grew a Qt dependency — the new files are `bmclient/src/cache.cpp`, `delivery.cpp` and `client.cpp`. Note that `check-qt-free.sh`'s `inner` is a single-quoted bash string: an apostrophe in a comment breaks it silently.

- [ ] **Step 5: Both `ctest` runs, in sequence, not together.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build --parallel 4 && (cd build && ctest -j2 --output-on-failure | tail -3)
cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure | tail -3)
```
Expected: 29/29 and all core tests.

- [ ] **Step 6: Record what is left uncovered and what slice 3 found.** Add a "Notes after slice 3" section to `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md` with: the `now` argument `Delivery` and `Client` take (and that `bmc::now()` is the veneer's source of it); that `bm::Vault` now holds its core through a `shared_ptr`; that the moved error text lives in the `bm::Errors` context and the `.ts` files still carry it under `bm::Session` until the next translation refresh; and a "Notes for slice 4" list for the node work (`bmc::NodeWatch` is already pure; `session.h`'s node members are the next thing to move; `relay_bridge.cpp` and the `--node` re-exec).

- [ ] **Step 7: Commit.**

```
docs(core): note the delivery slice, and catalogue its error text

The catalogue lists what bmc::Client and bmc::Delivery raise. The
translation files are left as they are: the moved strings stay under
bm::Session until the next refresh.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
```

## Notes for slice 4

- **`bm::Cache` and `bm::Delivery` are now one conversion layer each.** `Session` reaches them through `client_`; nothing else constructs them except the tests. If a later slice deletes a veneer class, its test is the only other caller.
- **`bmc::Client` is the seam `bmbot` needs.** `sendChat`, `sendLetter`, `unreadChats`, `markChatMessageRead` and `chatState` are the five calls the bot ABI wraps; they throw `bmc::Error` with a `Code`, which maps straight onto `bmbot`'s negative error codes.
- **The node is slice 4.** `bm::NodeWatch` is already pure in `src/node_watch.h`; `Session`'s node members (`QProcess node_`, `nodeLock_`, `listenPort_`, `lastNodeStatus_`, `restartSoon_`, `nodeWatch_`, `watchdogNote_`, `watchdogRestarts_`, `stoppingNode_`, `nodeRestarts_`), `nodeArguments()`, `validEndpoint`, `configuredPort` and `retentionMB_`/`retentionDays_` are what `bmc::NodeConfig`, `bmc::NodeStatus` and `bmc::NodeSupervisor` take. The status file `status.json` is read in `Session::tick` today; `bmc::NodeStatus` should own that parse.
- **`scanMailbox` has no production caller.** `Delivery::scan` is the production path. It stays only because `tests/lifecycle_tests.cpp` pins it; slice 4 or 5 may retire it with that test.
- **`bmc::nowMillis()` still has no test seam** and `bmc::updates::defaultSubscriptions()` (the translated label) still lives in Qt; both are deliberate.
