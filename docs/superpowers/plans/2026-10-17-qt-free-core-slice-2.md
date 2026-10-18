# Qt-free core, slice 2: storage — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `bmclient` holds the storage layer — `bmc::qtstream`, the record types, `bmc::Vault` and `bmc::Mailbox` (letters and chats) — and the Qt `bm::Vault` and `bm::Mailbox` in `src/` become thin wrappers over them. A vault and a mailbox written by the previous release open in the new code, and the previous release opens what the new code writes.

**Architecture:** The same method as slice 1. The old storage sources are snapshotted into `tests/legacy` under the namespace `legacy` behind plain types (`oracle::`), parity tests run old and new side by side, the old answers are frozen into `tests/golden` (vault files at v1 and v2, mailbox files at `user_version` 1 and 2), and the snapshot is deleted. `src/storage.{h,cpp}`, `src/mailbox_ops.cpp` and `src/chat_store.cpp` become wrappers that convert `QString`/`QByteArray` and call `bmc`. `src/sqlite_helpers.h` stays (Qt-flavoured) for `src/message_search.cpp`, which keeps its own connection; the core gets `bmc/sqlite.h`.

**Tech Stack:** C++20, CMake 3.22+, OpenSSL 3, libsodium (Argon2id, XChaCha20-Poly1305), SQLCipher, the notbit C codec, the in-repo harness `bmc/test.h`; Qt 6.8 only for the veneer and the parity tests.

**Spec:** `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md` (slice 2 of 5 under "Order of work", and "On-disk compatibility"). Slice 1's plan: `docs/superpowers/plans/2026-10-15-qt-free-core-slice-1.md`.

## Global Constraints

Copied from the spec; every task includes them.

- Nothing in `bmclient/` or `bmbot/` includes or links Qt; checked by `scripts/check-qt-free.sh` in a Qt-less container.
- The core is C++20, std only, plus OpenSSL, libsodium, SQLCipher, libsecp256k1 and the notbit C code.
- Bytes and text: `using Bytes = std::vector<uint8_t>`; `using ByteView = std::span<const uint8_t>`; text is UTF-8 `std::string` / `std::string_view`; addresses are `std::string`; integers are `int64_t` / `uint64_t`.
- Errors: `bmc::Error : std::runtime_error` with the exact English `what()` of today; no localized text in the core; `Session::errorText` translates `what()`; `scripts/update-error-catalog.py` reads `bmclient/src/*.cpp` (already true from slice 1).
- Time: seconds since the epoch as `int64_t`; code that needs the time takes `now` or calls `bmc::now()`.
- **On-disk formats do not change.** The vault file (BMVAULT1 and BMVAULT2) and the mailbox `.bmmail` (schema, `user_version` 1 and 2, every SQL statement, the partial index `chat_unread`) are byte-compatible. The previous release opens what the new code writes and the other way round.
- The core never prints; no POSIX-only call in `bmclient` (it compiles on Linux, macOS and Windows/MSVC).
- The veneer keeps the names, the namespace (`bm`) and the Qt signatures of today's `src/storage.h`; the app and every `ctest` suite compile against the same header.
- Work happens on `feature/qt-free-core`; tests live in `tests/bmclient/`.
- Nothing is committed or pushed unless the user has said so.

## Review Focus

The inputs the spec implies but a plain port could get wrong. Each line has a test in the task named in brackets.

1. **The vault bytes.** `QDataStream` is Qt's format: a `quint32` is big-endian; a `QString` is a `quint32` byte length then UTF-16BE, and a null string is `0xFFFFFFFF` (read as empty); a `bool` is one byte. The core must write and read exactly this. [Task 1: `qtstream_writes_the_bytes_qt_wrote` and `qtstream_reads_the_bytes_qt_wrote`; Task 3: the golden v1 and v2 vault files]
2. **A vault that must not be lost.** A read-only vault still opens (the chan rename is best-effort); a failed save leaves the previous file; a save that would exceed 4 MiB, or 1024 identities, or 1024 mailbox keys, is refused with the existing text; `create` reserves the file with mode 0600 and removes it if the first save throws. [Task 3: `a_read_only_vault_still_opens`, `a_failed_save_keeps_the_previous_vault`, `vault_limits_are_refused`]
3. **A mailbox made by the previous release.** `user_version` 1 (no chats, no `unread`) migrates to 2 with the same rows, and the previous release opens the migrated file. [Task 4: the golden v1 and v2 mailbox files; Task 6: `scripts/compat-check.sh`]
4. **The chat rules.** `queueChat` refuses an empty body, a body over `kMaxChatBodyBytes`, an expiry in the past or past `kChatTtlSeconds`; `storeChat` silently drops the same; the seqno is the sender's millisecond clock, always increasing within a conversation, never a count from 1; `unreadChats` is exactly the `chat_unread` partial index in `received,rowid` order. [Task 5: `chats_are_stored_and_read_back`, `a_restored_mailbox_keeps_its_numbers`, `unread_chats_are_the_partial_index`]
5. **Text at the boundary.** A `QString` label with a lone surrogate becomes `?` in UTF-8 (`QString::toUtf8`); a vault label written by Qt and read by the core, and the other way round, are the same text. [Task 3: `a_label_round_trips_through_both_spellings`]

## Execution rules

- Run every command from `/Users/user/w/ynotbit`. In each shell first: `export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig`.
- Build directories: `build` (the existing Qt build, Release, Ninja) and `build-core` (Qt-free).
- Qt-free configure on this Mac: `cmake -S . -B build-core -G Ninja -DYNOTBIT_QT=OFF -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=/opt/local/libexec/openssl3`.
- The Mac stalls under swap pressure: keep tool calls short; build with `--parallel 4`; a `HUNG` desktop test with low CPU means a stalled machine, rerun it in the foreground when calm.
- Every new or changed C++ file is formatted with `clang-format -i <file>`. Code blocks are close to that style, not exact.
- **Commit steps run only if the user has allowed commits on this branch.** Commit messages are Conventional Commits with a prose body and end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- Never stage or commit `docs/superpowers/plans/2026-10-04-sqlite-object-store.md`, `reports/` or `research_notes/`.
- A plan test that fails: fix the core, not the test, unless this plan says the test was wrong.

## Why this plan is shorter than slice 1's

Slice 1 printed every ported file in full because the dry run cut the files out of the plan. Slice 2 is mostly a **verbatim move**: the SQL, the migration order, the state names and the rules do not change, only the types around them (`QString`→`std::string`, `QByteArray`→`Bytes`, `QVector`→`std::vector`, `QUuid`→`bmc::uuid`, `QDateTime::currentSecsSinceEpoch`→`bmc::now`). This plan prints in full only what is **new** (the `qtstream` codec, the `Vault`, the `sqlite` helper, the record header, the golden generator) and, for the mechanical parts, gives the exact source file and the exact transformation, so the executor moves the statements without inventing them. Every SQL string is copied character for character from the file named in the step.

## File structure

New in `bmclient/`:

- `include/bmc/qtstream.h`, `src/qtstream.cpp` — the `QDataStream` subset the vault uses.
- `include/bmc/records.h` — `Identity`, `Message`, `ChatMessage`, `ChatSummary`, `OutboxItem`, `NetworkJob`, `StoredPublicKey`, `Subscription`, `Contact`, `DeliveryEvent`; the constants.
- `include/bmc/sqlite.h` — `Statement`, `Transaction` over SQLCipher, in std.
- `include/bmc/vault.h`, `src/vault.cpp`.
- `include/bmc/mailbox.h`, `src/mailbox.cpp` (connection, schema, migration, meta, letters, outbox, jobs, keys, subscriptions, contacts, settings, backup) and `src/mailbox_chats.cpp` (chats).
- `include/bmc/path.h`, `src/path.cpp` — `bmc::path()` (UTF-8 → `std::filesystem::path`), `bmc::writeFileAtomic()`, `bmc::reserveFile()`, `bmc::uuid()`.

Changed in `src/`:

- `storage.h` — `bm::Secret` already aliases `bmc::Secret` (slice 1). `bm::Vault` and `bm::Mailbox` hold the core objects; the Qt structs stay. New members replace the private data.
- `storage.cpp`, `mailbox_ops.cpp`, `chat_store.cpp` — become wrappers over `bmc`.
- `sqlite_helpers.h` — unchanged; now used only by `src/message_search.cpp`.

New in `tests/`:

- `tests/bmclient/qtstream_test.cpp`, `vault_test.cpp`, `mailbox_test.cpp`, `mailbox_chat_test.cpp`, `storage_fixtures.h`.
- `tests/bmclient/parity/vault_parity_test.cpp`, `mailbox_parity_test.cpp` (Qt only).
- `tests/legacy/` (temporary) — the snapshot of the old `storage.{h,cpp}`, `mailbox_ops.cpp`, `chat_store.cpp`, `sqlite_helpers.h`, plus `oracle.h`, `oracle.cpp`, `vault_gen.cpp`.
- `tests/golden/vault-v1.bmvault`, `vault-v2.bmvault`, `mailbox-v1.bmmail`, `mailbox-v2.bmmail`, `qtstream.txt`.
- `scripts/compat-check.sh`.

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

- [ ] **Step 2: Record the baseline.** The slice-1 exit criteria leave 29 tests on the Mac.

Run:
```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build --parallel 4 && (cd build && ctest -j2 --output-on-failure | tail -3)
```
Expected: `100% tests passed, 0 tests failed out of 29`.

- [ ] **Step 3: Note the pre-slice commit** (the compat gate builds it):

Run:
```sh
git rev-parse HEAD          # record as $SLICE2_BASE
```
Expected: a `9521645`-descended commit; write the hash down for Task 6.

## Task 1: `bmc::qtstream` — the QDataStream subset the vault uses

**Files:** create `bmclient/include/bmc/qtstream.h`, `bmclient/src/qtstream.cpp`, `tests/bmclient/qtstream_test.cpp`; change `bmclient/CMakeLists.txt`, `tests/bmclient/CMakeLists.txt`.

The vault payload is written by `QDataStream` at `Qt_6_0`. Only four things cross it: `quint32`, `bool`, `QString`, raw bytes. `bmc::qtstream` writes and reads exactly those, and nothing else.

**First, measure Qt** (as slice 1 did with the UTF-8 and JSON probes). Write `/tmp/qtstream_probe.cpp`, build it against Qt 6.8.3, and print the bytes for: `quint32(0)`, `quint32(0x01020304)`, `false`, `true`, `QString("")`, `QString("A")`, `QString("€")` (a non-BMP and a BMP case: add `"😀"`), a null `QString()`, and `writeRawData` of `{0,255}`. Record the hex. The expectations below are what Qt 6.8.3 gives; the probe confirms them.

- [ ] **Step 1: Write the failing test.**

```cpp
// tests/bmclient/qtstream_test.cpp
#include <bmc/qtstream.h>
#include <bmc/test.h>
#include <string>

using bmc::qtstream;

TEST(qtstream_writes_the_bytes_qt_wrote) {
    // The bytes a QDataStream at Qt_6_0 writes (confirmed by the probe in Task 1).
    qtstream out;
    out << uint32_t(0x01020304) << false << true;
    out << std::string("A") << std::string("€") << std::string("😀") << std::string();
    out.writeRaw({0, 255});
    CHECK_EQ(bmc::hex(out.bytes()),
             std::string("01020304" "00" "01"
                         "0000000141"
                         "00000003e282ac"
                         "00000004f09f9880"
                         "ffffffff"
                         "00ff"));
}

TEST(qtstream_reads_the_bytes_qt_wrote) {
    qtstream in(bmc::unhex("01020304" "00" "01" "0000000141" "00000003e282ac" "ffffffff"));
    uint32_t n = 0;
    bool f = true, t = false;
    std::string a, empty = "x";
    in >> n >> f >> t >> a >> empty;
    CHECK_EQ(n, uint32_t(0x01020304));
    CHECK(!f && t);
    CHECK_EQ(a, std::string("A"));
    CHECK_EQ(empty, std::string()); // a null QString reads as empty
    CHECK(in.ok() && in.atEnd());
}

TEST(qtstream_refuses_a_truncated_or_oversized_payload) {
    CHECK(!qtstream(bmc::unhex("000000")).ok());           // a quint32 cut short
    CHECK(!qtstream(bmc::unhex("00000005 41")).ok());       // a string longer than what is there
    CHECK(!qtstream(bmc::unhex("00000001 ff")).ok());       // an odd string length
    qtstream in(bmc::unhex("0000000141"));
    std::string a;
    in >> a;
    CHECK(!in.atEnd()); // one byte left over
}
```

- [ ] **Step 2: Run it and watch it fail** (`bmc/qtstream.h` does not exist). Add `qtstream_test.cpp` to `tests/bmclient/CMakeLists.txt`'s source list, then:

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build-core --target bmclient_tests --parallel 4 2>&1 | tail -5
```
Expected: a compile error, no such file `bmc/qtstream.h`.

- [ ] **Step 3: Write `qtstream.h`.**

```cpp
// The QDataStream subset the vault payload uses, and nothing else. A quint32 is big-endian; a
// QString is a quint32 byte length then UTF-16BE, and a null string is 0xFFFFFFFF (read as
// empty); a bool is one byte; raw bytes are as given. See the probe and the golden file.
#pragma once
#include <bmc/bytes.h>
#include <cstdint>
#include <string>

namespace bmc {
class qtstream {
    Bytes bytes_;
    size_t at_ = 0;
    bool ok_ = true;

  public:
    qtstream() = default;
    explicit qtstream(Bytes in) : bytes_(std::move(in)) {}

    qtstream &operator<<(uint32_t v);
    qtstream &operator<<(bool v);
    qtstream &operator<<(const std::string &v);
    void writeRaw(ByteView raw);

    qtstream &operator>>(uint32_t &v);
    qtstream &operator>>(bool &v);
    qtstream &operator>>(std::string &v);

    const Bytes &bytes() const { return bytes_; }
    bool ok() const { return ok_; }
    bool atEnd() const { return at_ == bytes_.size(); }
};
} // namespace bmc
```

- [ ] **Step 4: Write `qtstream.cpp`.** Write the string as a `uint32_t` byte length then the UTF-16BE units; a null (never written by the vault, but Qt's reader accepts it) is `0xFFFFFFFF`. Reading a string: a `0xFFFFFFFF` length is the empty string; an odd length or a length past the end is a failure (`ok_ = false`, leave `at_` where it is).

```cpp
#include <bmc/qtstream.h>

namespace bmc {
namespace {
void put32(Bytes &b, uint32_t v) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
bool get32(ByteView b, size_t &at, uint32_t &v) {
    if (at + 4 > b.size())
        return false;
    v = uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
    at += 4;
    return true;
}
// UTF-8 to UTF-16 code units, the way QString::fromUtf8 then QString::utf16 gives them. A lone
// surrogate is not produced here: the vault text comes from QString::toUtf8 (Task 4 of slice 1).
std::u16string utf16(std::string_view text);
} // namespace
qtstream &qtstream::operator<<(uint32_t v) {
    put32(bytes_, v);
    return *this;
}
qtstream &qtstream::operator<<(bool v) {
    bytes_.push_back(v ? 1 : 0);
    return *this;
}
qtstream &qtstream::operator<<(const std::string &v) {
    const auto units = utf16(v);
    put32(bytes_, uint32_t(units.size() * 2));
    for (char16_t c : units) {
        bytes_.push_back(uint8_t(c >> 8));
        bytes_.push_back(uint8_t(c));
    }
    return *this;
}
void qtstream::writeRaw(ByteView raw) {
    bytes_.insert(bytes_.end(), raw.begin(), raw.end());
}
qtstream &qtstream::operator>>(uint32_t &v) {
    if (!get32(bytes_, at_, v))
        ok_ = false;
    return *this;
}
qtstream &qtstream::operator>>(bool &v) {
    if (at_ + 1 > bytes_.size()) {
        ok_ = false;
        return *this;
    }
    v = bytes_[at_++] != 0;
    return *this;
}
qtstream &qtstream::operator>>(std::string &v) {
    uint32_t n = 0;
    if (!get32(bytes_, at_, n) || n == 0xFFFFFFFF) {
        if (!ok_) return *this;
        v.clear();
        return *this;
    }
    if (n % 2 != 0 || at_ + n > bytes_.size()) {
        ok_ = false;
        return *this;
    }
    std::u16string units;
    for (uint32_t i = 0; i < n; i += 2)
        units.push_back(char16_t(uint16_t(bytes_[at_ + i]) << 8 | bytes_[at_ + i + 1]));
    at_ += n;
    v = utf8(units); // the inverse of utf16(), refusing a lone surrogate as Qt's toUtf8 does
    return *this;
}
} // namespace bmc
```

Implement `utf16`/`utf8` in the same file with the `bmc::text` helpers from slice 1 if they exist, otherwise hand-rolled: `utf16` decodes UTF-8 (an invalid byte becomes U+FFFD, as `QString::fromUtf8`); `utf8` encodes and turns a lone surrogate into `?` (as `QString::toUtf8`). Keep the two small and unit-tested by the round trip in Task 3.

- [ ] **Step 5: Add the source** to `bmclient/CMakeLists.txt`'s `add_library` list and build:

```sh
cmake --build build-core --target bmclient_tests --parallel 4 && (cd build-core && ctest -R bmclient -N)
```
Then run `build-core/tests/bmclient_tests --filter qtstream` (or however slice 1's harness selects a test; see `bmc/test.h`). Expected: three tests pass.

- [ ] **Step 6: Commit** (only if allowed): `feat(core): a QDataStream subset the vault payload uses`.

## Task 2: Records, paths, uuid, and the SQLite helper

**Files:** create `bmclient/include/bmc/records.h`, `bmclient/include/bmc/sqlite.h`, `bmclient/include/bmc/path.h`, `bmclient/src/path.cpp`; change `bmclient/CMakeLists.txt`.

- [ ] **Step 1: `records.h`.** Move the record types from `src/storage.h` (lines 22–58, 232–265) to `bmc`, changing only the types. `Identity`, `Message` already exist in `bmc/identity.h` (slice 1) — do not redefine them; `#include <bmc/identity.h>`. Add:

```cpp
#pragma once
#include <bmc/identity.h>
#include <bmc/secret.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bmc {
constexpr int kMaxDraftBytes = 4 * 1024 * 1024;

struct ChatMessage {
    std::string id, localAddress, peerAddress, body, state;
    int64_t seqno = 0, received = 0, expires = 0;
    bool outgoing = false;
};
struct ChatSummary {
    std::string local, peer, body, state;
    int64_t received = 0;
    bool outgoing = false;
    int unread = 0;
};
struct OutboxItem {
    std::string id, kind, state, error, objectHash;
    Bytes ackToken, ackObject;
    int64_t expires = 0, nextAttempt = 0;
    int attempts = 0;
    Message message;
    bool chat = false;
};
struct NetworkJob {
    std::string id, owner, kind, address, state, hash, error;
    Bytes payload;
    int64_t expires = 0;
    uint64_t nonceTrials = 1000, extraBytes = 1000;
};
struct StoredPublicKey {
    std::string address;
    Bytes signingKey, encryptionKey;
    uint64_t nonceTrials = 1000, extraBytes = 1000;
    uint32_t behaviors = 1;
    int64_t expires = 0;
};
struct Subscription {
    std::string address, label;
};
struct Contact {
    std::string address, label;
    int64_t added = 0;
};
struct DeliveryEvent {
    int64_t timestamp = 0;
    std::string state, detail;
};

// A chan's name as the app shows it: "#<name>". PyBitmessage's default "[chan] <phrase>" and a
// bare "<name>" both become "#<name>". Copied from bm::channelName (src/storage.cpp).
std::string channelName(std::string_view label);
} // namespace bmc
```

Implement `channelName` in `src/vault.cpp` (Task 3), copied from `src/storage.cpp:31-38` with `QString`→`std::string`: `trimmed()` becomes `bmc::text::trim` (slice 1), `startsWith("[chan] ")`/`mid(7)`/`startsWith('#')` stay as the same byte tests.

- [ ] **Step 2: `path.h`/`path.cpp`.** Three helpers, each used by `Vault` and `Mailbox`:

```cpp
#pragma once
#include <bmc/bytes.h>
#include <filesystem>
#include <string>

namespace bmc {
// UTF-8 to a path. On Windows a UTF-8 string becomes a wide path (std::filesystem does this for
// a u8 path); on POSIX it is the bytes as they are (QFile::encodeName did the same).
std::filesystem::path path(const std::string &utf8);
// Write the bytes to a temp file in the same directory, fsync, rename over the target (what
// QSaveFile did). Throws bmc::Error("Cannot write ...") on failure; the target is untouched.
void writeFileAtomic(const std::filesystem::path &target, ByteView bytes, unsigned mode = 0600);
// Create the file if it is not there (mode 0600), or throw bmc::Error("File already exists or
// cannot be created"). What Vault::create and Mailbox::create did with QFile::NewOnly.
void reserveFile(const std::filesystem::path &target);
// A random version-4 UUID, 36 lower-case characters, no braces (QUuid::createUuid()).
std::string uuid();
} // namespace bmc
```

`path()`: `std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()))`.
`writeFileAtomic()`: `fopen(tmp, "wb")` (tmp = target + `.tmp-<uuid>`), write, `fflush`+`fsync(fileno)`, `fclose`, `chmod`/`std::filesystem::permissions(owner_read|owner_write)`, `std::filesystem::rename`; on any failure remove the temp and throw.
`reserveFile()`: `fopen(path, "wbx")` (C11 exclusive create; MSVC and glibc both have it); if it fails, throw; `fclose`; set mode 0600.
`uuid()`: 16 random bytes from `bmc::randomBytes` (slice 1), set version 4 and the variant, format `8-4-4-4-12` lower-case hex.

- [ ] **Step 3: `sqlite.h`.** The std form of `src/sqlite_helpers.h` (read it; it is 88 lines). Same two classes, same behaviour, no Qt:

```cpp
#pragma once
#include <bmc/bytes.h>
#include <bmc/error.h>
#include <cstdint>
#include <sqlcipher/sqlite3.h>
#include <string>
#include <string_view>

namespace bmc {
class Statement {
    sqlite3_stmt *s_ = nullptr;
    void check(int result) const; // throws bmc::Error(sqlite3_errmsg(db))

  public:
    Statement(sqlite3 *db, const char *sql);
    ~Statement();
    Statement(const Statement &) = delete;
    sqlite3_stmt *get() const { return s_; }
    void text(int n, std::string_view t);      // bind_text, SQLITE_TRANSIENT
    void number(int n, int64_t i);
    void blob(int n, ByteView b);
    void reset();
    bool row();
    std::string text(int n) const;             // from_utf8 of the column, empty when NULL
    int64_t number(int n) const;
    Bytes blob(int n) const;
};
// Inside a transaction already open (a scan batch), it joins that one: the outer one commits, or
// rolls everything back if this throws. Copied from bm::detail::Transaction.
class Transaction {
    sqlite3 *db_;
    bool committed_ = false, nested_ = false;

  public:
    explicit Transaction(sqlite3 *db);
    ~Transaction();
    void commit();
};
} // namespace bmc
```

`text(int)` returns the column as UTF-8; the core stores text as UTF-8, so no conversion. Keep the exact error strings: `"Mailbox is closed"`, `sqlite3_errmsg(db)`.

- [ ] **Step 4: Add `path.cpp` to the library**, build `build-core` and the Qt build:

```sh
cmake --build build-core --target bmclient --parallel 4
cmake --build build --target bmcore --parallel 4
```
Expected: both build (nothing uses the new headers yet).

- [ ] **Step 5: Commit**: `feat(core): storage records, paths and a std SQLite helper`.

## Task 3: `bmc::Vault` — the vault bytes

**Files:** create `bmclient/include/bmc/vault.h`, `bmclient/src/vault.cpp`, `tests/bmclient/vault_test.cpp`; change `bmclient/CMakeLists.txt`, `tests/bmclient/CMakeLists.txt`. Add the Qt probe/generator in `tests/legacy` (Task 6 snapshots the old sources; the golden vault files are made here).

`bmc::Vault` is `bm::Vault` (`src/storage.cpp:55-261`, `531-585`) with std types and `bmc::qtstream` in place of `QDataStream`.

- [ ] **Step 1: Write `vault.h`.** The public shape of `bm::Vault` (storage.h:59-96), in `bmc`:

```cpp
#pragma once
#include <bmc/records.h>
#include <bmc/secret.h>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bmc {
class Vault {
    std::string path_;
    Secret wrapping_;
    Bytes salt_;
    bool unlocked_ = false;
    std::vector<Identity> identities_;
    std::map<std::string, Secret> mailboxKeys_;
    void save();

  public:
    ~Vault();
    void create(const std::string &path, const std::string &password);
    void unlock(const std::string &path, const std::string &password);
    void changePassword(const std::string &password);
    void lock();
    bool unlocked() const { return unlocked_; }
    const std::vector<Identity> &identities() const { return identities_; }
    std::string addIdentity(const std::string &label);
    std::string addIdentity(const Identity &identity);
    std::string addChannel(const std::string &phrase, const std::string &label,
                           const std::string &expectedAddress = {});
    void importKeys(const std::string &path);
    std::string addMailboxKey();
    void renameIdentity(const std::string &address, const std::string &label);
    void deleteIdentity(const std::string &address);
    void setDefaultIdentity(const std::string &address);
    std::vector<std::string> mailboxIds() const;
    const Secret &mailboxKey(const std::string &id) const;
    std::string path() const { return path_; }
};
} // namespace bmc
```

`addIdentity`/`addChannel`/`importKeys` call `bmc::Protocol` (slice 1) where `bm::Vault` called `bm::Protocol`: `Protocol::identity(label)`, `Protocol::fromPrivate(label, signing, encryption, 4, 1, false)`, `Protocol::channel(phrase, label, version)`, `Protocol::fromWif`/`importKeys` equivalent — read `src/storage.cpp:203-234` and `src/protocol.h`'s `importKeys`/`addChannel` usage; `addChannel` and `importKeys` live in `src/protocol.cpp` today (they touch the vault) — move their bodies into `bmc::Vault::addChannel`/`importKeys`, calling `bmc::Protocol`. Find them with `tokensave_search` for `addChannel` and `importKeys` before moving.

- [ ] **Step 2: Write the failing tests**, including the golden vault files.

`tests/bmclient/storage_fixtures.h` loads `BMC_GOLDEN_DIR/vault-v1.bmvault` and `vault-v2.bmvault` into `bmc::Bytes` (the same `BMC_GOLDEN_DIR` define slice 1's `golden_test.cpp` uses; see `tests/bmclient/CMakeLists.txt`).

```cpp
// tests/bmclient/vault_test.cpp
#include "storage_fixtures.h"
#include <bmc/test.h>
#include <bmc/vault.h>

using bmc::Vault;

TEST(a_vault_round_trips_through_the_core) {
    const auto path = bmc::test::tempDir() + "/v.bmvault";
    Vault v;
    v.create(path, "correct horse");
    CHECK_EQ(v.addIdentity("me"), v.identities().front().address);
    const auto id = v.addMailboxKey();
    v.lock();
    Vault again;
    again.unlock(path, "correct horse");
    CHECK_EQ(again.identities().size(), size_t(1));
    CHECK_EQ(again.mailboxKey(id).size(), size_t(32));
}

TEST(the_golden_vaults_of_the_previous_release_open) {
    for (const auto &[file, expected] : {std::pair{goldenVault1, size_t(1)},
                                         std::pair{goldenVault2, size_t(2)}}) {
        const auto path = bmc::test::tempDir() + "/golden.bmvault";
        bmc::test::writeFile(path, file);
        Vault v;
        v.unlock(path, "slice2-golden"); // the fixed password the generator used
        CHECK_EQ(v.identities().size(), expected);
        CHECK_EQ(v.identities().front().label, std::string("golden"));
    }
}

TEST(a_read_only_vault_still_opens) {
    // A v2 file whose chan label is the old "[chan] x": the rename is tried and, when the file
    // cannot be written, the names are still fixed in memory.
    const auto path = bmc::test::tempDir() + "/ro.bmvault";
    bmc::test::writeFile(path, goldenVault2OldLabel);
    std::filesystem::permissions(path, std::filesystem::perms::owner_read); // 0400
    Vault v;
    v.unlock(path, "slice2-golden");
    CHECK_EQ(v.identities().front().label, std::string("#x"));
}

TEST(a_failed_save_keeps_the_previous_vault) {
    // Fill the vault past its size bound: the save throws and the file is unchanged.
    const auto path = bmc::test::tempDir() + "/big.bmvault";
    Vault v;
    v.create(path, "pw");
    const auto before = bmc::test::readFile(path);
    CHECK_THROWS(v.addIdentity(std::string(300, 'x'))); // a 300-byte label is fine; force the bound
    // A direct way: add identities until identities_.size() > 1024 is refused.
    CHECK_EQ(bmc::test::readFile(path), before);
}
```

(Adjust the third and fourth tests to the harness's real `tempDir`/`readFile` names in `bmc/test.h`; slice 1 added `bmc::test::fail`, so a temp helper may need adding there. If it does not exist, add `bmc::test::tempDir()` to `bmc/test.h` returning a fresh directory under the system temp path.)

- [ ] **Step 3: Run and watch them fail** (`bmc/vault.h` does not exist). Add the test to the CMake list, build `build-core`, expected: compile error.

- [ ] **Step 4: Write `vault.cpp`.** Move `src/storage.cpp:31-261` and `531-585` into `bmc`, changing:
  - `check(cond, "text")` → `bmc::require(cond, "text")` (slice 1's `bmc/error.h`; confirm the name — `src/storage.cpp` has a local `fail`/`check`; the core uses `require`).
  - `QByteArray` → `Bytes`; `QString` → `std::string`; `QStringList` → `std::vector<std::string>`.
  - `QDataStream` → `bmc::qtstream` (`out << quint32(...)` → `out << uint32_t(...)`; `out.writeRawData(ptr, n)` → `out.writeRaw(ByteView(ptr, n))`; the read side the same).
  - `randombytes_buf(p, n)` → `bmc::randomBytes(p, n)` (slice 1).
  - `crypto_pwhash`/`crypto_aead_xchacha20poly1305_ietf_*` stay (libsodium).
  - `QFile`/`QSaveFile`/`QFileInfo` → `bmc::path`, `bmc::writeFileAtomic`, `bmc::reserveFile` (Task 2). `Vault::save` writes `header + cipher` through `writeFileAtomic(path_, joined, 0600)`.
  - `QFile::remove(path)` → `std::filesystem::remove`.
  - `channelName` stays the free `bmc::channelName`.
  - Keep every literal text identical: `"Enter a vault password"`, `"Insufficient memory to unlock vault"`, `"File already exists or cannot be created"`, `"Cannot protect file permissions"`, `"Lock the current vault first"`, `"Vault is locked"`, `"Vault capacity exceeded; existing vault preserved"`, `"Cannot serialize vault"`, `"Vault is too large; existing vault preserved"`, `"Cannot write vault"`, `"Vault save failed; previous vault preserved"`, `"Cannot open vault"`, `"Invalid vault size"`, `"Unsupported vault format"`, `"Wrong password or damaged vault"`, `"Too many vault identities"`, `"Truncated vault"`, `"Too many mailbox keys"`, `"Truncated vault key"`, `"Duplicate mailbox key"`, `"Invalid vault contents"`, `"Identity keys do not match its address"`, `"Identity already in the vault"`, `"Mailbox key is not in this vault"`, `"Identity label is too long"`, `"Identity not found"`.
  - `Vault::lock()` is called by the destructor; `Vault::~Vault() { lock(); }`.
  - The size bound `plain.bytes.size() + 64 <= 4 * 1024 * 1024` and the 1024 counts stay.
  - `BMVAULT1` reads `chan` then no default flag; `identities.front().isDefault = true`; the reader accepts the file sizes `>= 64 && <= 4 * 1024 * 1024`.

- [ ] **Step 5: Make the golden files.** Write `tests/legacy/vault_gen.cpp` (a small Qt program, built like slice 1's `golden_gen.cpp` — see `tests/bmclient/CMakeLists.txt`'s Qt-only parity target, or add a one-off target in `cmake/qt-app.cmake`). It uses the **old** `bm::Vault` (from the snapshot, namespace `legacy`) to create:
  - `vault-v1.bmvault` — a `BMVAULT1` file: the generator writes a `BMVAULT1` by hand (magic + salt + nonce + the same AEAD over a payload with **no** default flag), with fixed password `slice2-golden`, one identity labelled `golden`;
  - `vault-v2.bmvault` — created through the old `Vault::create` with the same password, one identity and one mailbox key (labels fixed);
  - `vault-v2-old-label.bmvault` — a v2 file with a chan labelled `"[chan] x"`.
  Because the salt and nonce are random, the generator prints nothing; the files are stored as they are and the core only **reads** them. Write the writer's byte-compatibility another way: the parity test (Task 6) has the old `Vault` open a file the core wrote.

- [ ] **Step 6: Run the tests.** `build-core`, `bmclient_tests --filter vault`. Expected: all pass.

- [ ] **Step 7: Make `bm::Vault` a wrapper.** In `src/storage.h`, replace `bm::Vault`'s private data with `std::unique_ptr<bmc::Vault>` (or by value; it is move-only and lives inside `Session`), keep every public signature, and in `src/storage.cpp` replace the bodies with conversions:
  - `void create(const QString &p, const QByteArray &pw) { core_->create(bm::bridge::toStd(p), bm::bridge::toStd(pw)); }`
  - `const std::vector<Identity> &identities()` — the Qt `bm::Identity` (QString label) stays the app's type; keep `bm::Vault` returning `std::vector<bm::Identity>` built from `bmc::Vault::identities()` via `bm::bridge::fromCore` (already in `src/qt_bridge.h:44`). Build the vector once per call (the app reads it rarely) or cache it; do not return a reference into the core.
  - `mailboxKey` returns `const bmc::Secret&` (the alias `bm::Secret = bmc::Secret` makes the signature unchanged).
  - `mailboxIds()` builds a `QStringList` from the std vector.
  - `changePassword(const QByteArray&)` → `toStd`; the password is a secret: wipe the `std::string` after the call (use `bm::bridge::SecretText`, `src/qt_bridge.h:65`).
  - `addIdentity`, `addChannel`, `importKeys`, `renameIdentity`, `deleteIdentity`, `setDefaultIdentity`, `addMailboxKey` return/accept the same Qt types with `toStd`/`fromStd`.
  - The private helpers (`fail`, `check`, `derive`, `reserveFile`, `SensitiveBytes`) move to the core or are deleted.

- [ ] **Step 8: Build the Qt app and run the storage suites.**

```sh
cmake --build build --parallel 4 && (cd build && ctest -R 'core|lifecycle|outbox|chat' --output-on-failure)
```
Expected: pass (these suites use `bm::Vault`).

- [ ] **Step 9: Commit**: `feat(core): the vault, and bm::Vault as a wrapper`.

## Task 4: `bmc::Mailbox` — letters, outbox, jobs, keys, contacts

**Files:** create `bmclient/include/bmc/mailbox.h`, `bmclient/src/mailbox.cpp`; `tests/bmclient/mailbox_test.cpp`; change `bmclient/CMakeLists.txt`, `tests/bmclient/CMakeLists.txt`, `src/storage.h`, `src/storage.cpp`, `src/mailbox_ops.cpp`.

This is a verbatim move of the SQL. Read `src/storage.cpp` (262–592), `src/mailbox_ops.cpp` (all 586 lines) and `src/chat_store.cpp` (chats are Task 5) and move each method into `bmc`, keeping every SQL string character for character.

- [ ] **Step 1: Write `mailbox.h`.** The public shape of `bm::Mailbox` (storage.h:103-229), in `bmc`, with std types:

```cpp
#pragma once
#include <bmc/records.h>
#include <bmc/secret.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
namespace bmc::detail {
class Transaction;
}
namespace bmc {
class Mailbox {
    sqlite3 *db_ = nullptr;
    uint64_t messageRevision_ = 0;
    void connect(const std::string &path, const Secret &key, bool create);
    void sql(const char *q);
    void migrate();
    void deliveryUpdate(const std::string &id, const std::string &state, const std::string &detail,
                        int64_t nextAttempt);

  public:
    ~Mailbox();
    void create(const std::string &path, const std::string &keyId, const Secret &key);
    void open(const std::string &path, const Secret &key);
    void close();
    bool isOpen() const { return db_ != nullptr; }
    std::string keyId() const;
    void bindCache(const std::string &cacheId);
    void bindIdentities(const std::string &fingerprint, bool keepCheckpoint = false);
    std::string boundIdentities() const;
    class Batch {
        std::unique_ptr<detail::Transaction> t_;

      public:
        explicit Batch(Mailbox &m);
        ~Batch();
        void commit();
    };
    // ... store, advance, checkpoint, messages, messageSummaries (three overloads), messageCount,
    // messageHashes, messagePosition, channelAddresses, channelUnread, backup, saveDraft, message,
    // queueDraft, outbox, outgoing, setDelivery, setAck, setObjectHash, acknowledge, retry, cancel,
    // events, recordMilestone, addJob, jobs, updateJob, removeJob, savePublicKey, publicKey,
    // subscriptions, subscribe, unsubscribe, contacts, saveContact, removeContact, moveMessage,
    // restoreMessage, deleteMessage, markRead, unread, setting, setSetting, and the chats (Task 5)
    // — every method of bm::Mailbox, same parameters with std types.
    uint64_t messageRevision() const { return messageRevision_; }
};
} // namespace bmc
```

Every `QString` parameter becomes `const std::string &`, every `QByteArray` a `ByteView`/`Bytes`, `QVector<T>` a `std::vector<T>`, `QStringList` a `std::vector<std::string>`, `qint64` `int64_t`, `quint64` `uint64_t`, `int` stays `int`.

- [ ] **Step 2: Write the failing test** for the connection, the schema, migration and the letter rows.

```cpp
// tests/bmclient/mailbox_test.cpp
#include "storage_fixtures.h"
#include <bmc/mailbox.h>
#include <bmc/test.h>

using bmc::Mailbox;

TEST(a_new_mailbox_has_the_schema_and_a_key_id) {
    const auto path = bmc::test::tempDir() + "/m.bmmail";
    bmc::Secret key(32);
    bmc::randomBytes(key.data(), 32);
    Mailbox m;
    m.create(path, "key-1", key);
    CHECK_EQ(m.keyId(), std::string("key-1"));
    CHECK_EQ(m.checkpoint(), int64_t(0));
    m.close();
}

TEST(the_golden_mailboxes_of_the_previous_release_migrate) {
    for (const auto &file : {goldenMailbox1, goldenMailbox2}) {
        const auto path = bmc::test::tempDir() + "/golden.bmmail";
        bmc::test::writeFile(path, file);
        Mailbox m;
        m.open(path, goldenMailboxKey); // the fixed 32-byte key the generator used
        CHECK(m.isOpen());
        CHECK(!m.messages().empty()); // the generator stored letters
        m.close();
    }
}

TEST(a_letter_round_trips_and_unread_is_tracked) {
    // store(...) then messages()/messageSummaries()/markRead/unread, and the folder moves.
    // Every assertion mirrors a case in tests/core_tests.cpp and tests/outbox_tests.cpp.
}
```

- [ ] **Step 3: Run and watch them fail**, then write `mailbox.cpp`.

Move `src/storage.cpp:262-592` and all of `src/mailbox_ops.cpp` into `bmc`, method by method:
- `Statement`/`Transaction` → `bmc::Statement`/`bmc::Transaction` (`bmc/sqlite.h`, Task 2).
- `sqlite3_open_v2(QFile::encodeName(path).constData(), ...)` → `sqlite3_open_v2(bmc::path(path).string().c_str(), ...)`.
- `QDateTime::currentSecsSinceEpoch()` → `bmc::now()`.
- `QUuid::createUuid().toString(QUuid::WithoutBraces)` → `bmc::uuid()`.
- `QFile::remove(path)` → `std::filesystem::remove`.
- `require(cond, "text")` keeps its text; the messages `"Mailbox is closed"`, `"Invalid mailbox key"`, `"Cannot open mailbox"`, `"Cannot apply mailbox key"`, `"This build lacks SQLCipher"`, `"SQLCipher is required"`, `"Unsupported mailbox format"`, `"Invalid mailbox"`, `"Invalid mailbox metadata"`, `"Invalid checkpoint"`, `"Invalid delivery state"`, `"Delivery no longer exists"`, `"Invalid acknowledgment token"`, `"Invalid network object size"`, `"Recipient proof-of-work requirement exceeds supported limits"`, `"Letter is too large"`, `"Only drafts can be edited"`, `"This letter already has a delivery record; duplicate it as a new draft"`, `"Letter no longer exists"`, `"Invalid delivery type"`, `"Only a draft can be sent"`, `"Choose a sender and recipient"`, `"Message expiry is in the past"`, `"Chats cannot be retried after their 24-hour expiry"`, `"This letter has already been acknowledged"`, `"Delivery is already complete"`, `"No delivery record for this chat"`, `"No delivery record for this letter"`, `"Invalid destination folder"`, `"Cancel active delivery before moving this letter to Trash"`, `"Move the letter to Trash first"`, `"Enter a broadcast address"`, `"A contact needs an address and a name"`, `"Invalid public-key length"` — all identical.
- The `sqlite3_update_hook` lambda uses `qstrcmp(table, "...")`; in std use `std::strcmp`.
- `migrate()` keeps the exact statements and order from `src/mailbox_ops.cpp:29-85`, including `PRAGMA user_version=2`, the `chat_unread` partial index and the `chat_messages`/`chat_sequences`/`chat_outbox` tables.
- `create()` keeps `PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; BEGIN IMMEDIATE; CREATE TABLE meta(...); CREATE TABLE messages(...); PRAGMA user_version=1;` then `INSERT INTO meta(id,checkpoint) VALUES(?,0)` then `migrate()`.
- `backup()` uses `sqlite3_backup_init/step/finish` as now.
- `bindCache`, `bindIdentities`, `Batch` are the small methods at `src/storage.cpp:554-592`.

- [ ] **Step 4: Make the golden mailbox files.** Extend `tests/legacy/vault_gen.cpp` (rename it `tests/legacy/storage_gen.cpp`) to build, with the **old** `bm::Mailbox`:
  - `mailbox-v1.bmmail` — a `user_version` 1 file (the generator opens a raw sqlite connection, creates the v1 schema of `Mailbox::create`, inserts a few letters, `PRAGMA user_version=1`);
  - `mailbox-v2.bmmail` — created through the old `Mailbox::create` (which migrates to 2), with letters, a chat, a subscription, a contact, a draft and a public key;
  - the fixed mailbox key (32 bytes, e.g. `0x00..0x1f`) is written into `tests/bmclient/storage_fixtures.h` as `goldenMailboxKey`.

- [ ] **Step 5: Run the tests** (`bmclient_tests --filter mailbox`), then build the Qt app and run the storage suites. Expected: pass.

- [ ] **Step 6: Make `bm::Mailbox` a wrapper.** In `src/storage.h` replace the private data (`sqlite3 *db_`, `messageRevision_`, the helpers) with `std::shared_ptr<bmc::Mailbox>` and keep every public signature. In `src/storage.cpp`/`mailbox_ops.cpp`, each body converts its arguments (`bm::bridge::toStd`, `toBytes`, `view`) and calls the core, converting results back:
  - `messages()`/`messageSummaries()`/`message()` return `QVector<bm::Message>` built with `bm::bridge::fromCore(bmc::Message)` (already in `src/qt_bridge.h:53`).
  - `outbox()`/`outgoing()`/`jobs()`/`publicKey()`/`subscriptions()`/`contacts()`/`events()` convert the record structs (add the small converters to `src/qt_bridge.h`).
  - `Batch` wraps `bmc::Mailbox::Batch`.
  - `messageRevision()` reads the core's.
  - `isOpen()`, `keyId()`, `checkpoint()`, `advance()`, the settings and the rest are one-line conversions.
  - The Qt-flavoured `detail::Statement`/`Transaction` in `src/sqlite_helpers.h` stay for `src/message_search.cpp`; nothing else includes it after this task.

- [ ] **Step 7: Build and run the suites** (`core`, `lifecycle`, `outbox`, `chat`, `delivery`, `desktop`, `chatbot`). Expected: pass.

- [ ] **Step 8: Commit**: `feat(core): the mailbox, and bm::Mailbox as a wrapper`.

## Task 5: `bmc::Mailbox` — chats

**Files:** create `bmclient/src/mailbox_chats.cpp`, `tests/bmclient/mailbox_chat_test.cpp`; change `bmclient/CMakeLists.txt`, `tests/bmclient/CMakeLists.txt`, `src/storage.h`, `src/chat_store.cpp`.

A verbatim move of `src/chat_store.cpp` (all 149 lines) plus the chat methods that live in `src/mailbox_ops.cpp` (`outbox`'s chat half, `outgoing`'s chat half, `acknowledge`'s chat half) and `src/storage.cpp` (`bindIdentities` is letters). The chat rules are the Review Focus 4.

- [ ] **Step 1: Write the failing tests.** Cover: `queueChat` refusals and success; `storeChat`'s silent drops; `storeRouted` routing a chat vs a letter vs an invalid chat; `chatMessages` (oldest first, the window and `more`); `chatSummaries`; `chatUnread`; `markChatRead`; `unreadChats` order; `chatState`; `markChatMessageRead`; `endChatDeliveries`; and the seqno rule.

```cpp
// tests/bmclient/mailbox_chat_test.cpp
#include <bmc/chat_format.h>
#include <bmc/mailbox.h>
#include <bmc/test.h>

using bmc::Mailbox;

TEST(a_queued_chat_gets_a_millisecond_number_that_keeps_growing) {
    // Two chats to one peer: seqno is bmc::now()'s milliseconds, the second larger; a restored
    // mailbox (reopen) still sends a number past the last one.
}

TEST(chats_that_break_the_rules_are_refused_or_dropped) {
    // queueChat: empty body, body over kMaxChatBodyBytes, expiry in the past, expiry past
    // kChatTtlSeconds -> throws "Invalid chat message or expiry".
    // storeChat: the same cases -> returns silently, no row.
}

TEST(unread_chats_are_the_partial_index_in_received_order) {
    // Three received chats and one outgoing: unreadChats(limit) is the received ones, oldest
    // first, at most limit; markChatMessageRead removes exactly one.
}
```

- [ ] **Step 2: Write `mailbox_chats.cpp`.** Move `src/chat_store.cpp` with the same transformations as Task 4: `QString`→`std::string`, `QByteArray`→`Bytes`, `QDateTime::currentSecsSinceEpoch`→`bmc::now()`, `QDateTime::currentMSecsSinceEpoch`→`bmc::nowMillis()` (add it to `bmc/clock.h` if slice 1 did not), `QUuid`→`bmc::uuid()`, `parseChatMessage`→`bmc::parseChatMessage` (slice 1, `bmc/chat_format.h`), `ChatPayload`→`bmc::ChatPayload`, `kMaxChatBodyBytes`/`kChatTtlSeconds` from `bmc/chat_format.h`. Keep every SQL and the message `"Invalid chat message or expiry"` identical. Move the chat halves of `outbox`, `outgoing`, `acknowledge` and the `chat-` prefix tests (`id.rfind("chat-", 0) == 0`) here.

- [ ] **Step 3: Run the tests**, then the Qt `chat` and `chatbot` suites. Expected: pass.

- [ ] **Step 4: Make the chat half of `bm::Mailbox` a wrapper** (bodies in `src/chat_store.cpp`), converting `QVector<bm::ChatMessage>`/`QVector<bm::ChatSummary>` with the record converters.

- [ ] **Step 5: Commit**: `feat(core): chats in the mailbox`.

## Task 6: The compat gate, the frozen golden files, the snapshot removed

**Files:** create `scripts/compat-check.sh`, `tests/bmclient/parity/vault_parity_test.cpp`, `tests/bmclient/parity/mailbox_parity_test.cpp`; change `cmake/qt-app.cmake`, `tests/golden/`; delete `tests/legacy`.

- [ ] **Step 1: Snapshot the old sources** into `tests/legacy` (the slice-1 script `scripts/legacy-snapshot.sh` lists slice 1's files — change the list to `src/storage.h src/storage.cpp src/mailbox_ops.cpp src/chat_store.cpp src/sqlite_helpers.h`). The snapshot goes under the namespace `legacy` behind `oracle::` (plain types), as in slice 1. Add `tests/legacy/oracle.h`/`oracle.cpp` exposing: `oracle::createVault`, `oracle::openVault` (returns the identities and mailbox key ids), `oracle::createMailbox`, `oracle::openMailbox` (returns the letter rows and the chat rows). Build it as a Qt-only target in `cmake/qt-app.cmake` (the same place slice 1 built the parity executable).

- [ ] **Step 2: Parity tests** (`tests/bmclient/parity/*`, Qt only, run beside the core):
  - the core opens every golden vault and mailbox (already in Tasks 3–4);
  - the **old** `bm::Vault` opens a vault the **core** wrote: the core writes a vault with a fixed password, the oracle opens it and reports the same identities and mailbox keys;
  - the **old** `bm::Mailbox` opens a mailbox the **core** wrote (letters, chats, a subscription, a contact, a public key) and reads the same rows; and the core opens what the old code wrote (the golden files);
  - the core's `qtstream` bytes equal `QDataStream`'s for a grid of strings (empty, ASCII, BMP, non-BMP, a lone surrogate, 255 bytes) and numbers.

- [ ] **Step 3: `scripts/compat-check.sh`.** A manual gate that builds `$SLICE2_BASE` (Task 0 Step 3) in a worktree and cross-opens files, exactly as the spec's Testing section describes. It:
  1. `git worktree add /tmp/slice2-base "$SLICE2_BASE"`;
  2. builds the Qt app there (the `build` recipe) and the current tree's `build-core`;
  3. creates a vault and a mailbox with the current `build-core` test helper (a small Qt-free program `build-core/storage_gen`) into a temp dir;
  4. opens them with the base worktree's app test helper (`oracle`-free: the base has the real `bm::Vault`/`bm::Mailbox`) and checks the identities/rows;
  5. the other way round: creates with the base, opens with the core;
  6. removes the worktree.
  Keep it a script, not a `ctest` test (it builds another tree).

- [ ] **Step 4: Freeze the answers.** The golden vault and mailbox files are already stored (Tasks 3–4). Add `tests/golden/qtstream.txt` (the hex grid the generator wrote) and a `golden_test` case that reads it. Then delete `tests/legacy` and its CMake target. The parity tests that used the oracle are deleted with it; the golden tests remain.

- [ ] **Step 5: Run everything.**

```sh
export PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig
cmake --build build --parallel 4 && (cd build && ctest -j2 --output-on-failure | tail -3)
cmake --build build-core --parallel 4 && (cd build-core && ctest --output-on-failure | tail -3)
```
Expected: the Qt suite passes (29 + the new Qt-free ones); `build-core` passes all `bmclient` tests.

- [ ] **Step 6: Mutation checks** (three, on the risky logic), each: change the core, rebuild `build-core`, see the named tests fail, revert.
  1. In `qtstream.cpp`, write the string length as the number of UTF-16 **units** instead of bytes → `qtstream_writes_the_bytes_qt_wrote` and the golden vault test fail.
  2. In `vault.cpp`'s `unlock`, accept `BMVAULT3` as well → the "Unsupported vault format" path; force it by writing `BMVAULT3` and expecting a refusal (add the assertion to the vault test).
  3. In `mailbox_chats.cpp`, order `unreadChats` by `rowid` instead of `received,rowid` → `unread_chats_are_the_partial_index_in_received_order` fails.

- [ ] **Step 7: Commit**: `test: the previous release's vault and mailbox, frozen; the snapshot removed`.

## Task 7: Verification, the Qt-free container, and docs

**Files:** change `bmclient/README.md`, `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md` (append "Notes after slice 2"), `docs/superpowers/work-state-2026-10-16.md` is not ours to commit.

- [ ] **Step 1: The Qt-free container.** Run `scripts/check-qt-free.sh` (slice 1's; it streams the tree to big1 and builds in a Qt-less Debian container). Expected: `QT-FREE BUILD OK` and the `bmclient` tests pass there. (Use `COPYFILE_DISABLE=1` on the tar, as slice 1 found.)

- [ ] **Step 2: big1, Linux.** Stream the tree to a fresh copy on big1 and run the Qt-free build and the relevant Qt tests (`core`, `lifecycle`, `outbox`, `chat`, `chatbot`, `delivery`). Expected: pass. Remove the copy afterwards; do not touch `~/w/ynotbit`.

- [ ] **Step 3: The Mac full suite.** `cmake --build build --parallel 4 && (cd build && ctest -j2 --output-on-failure | tail -5)`. Expected: all pass (the 29 of the baseline plus the new `bmclient` storage tests).

- [ ] **Step 4: The error catalogue.** Run `scripts/update-error-catalog.py` and `scripts/check-translations.py`. Expected: the catalogue lists the same messages as before plus any new one; the translations are complete.

- [ ] **Step 5: The cross-open gate.** Run `scripts/compat-check.sh` once by hand. Expected: both directions open.

- [ ] **Step 6: Docs.** Add to `bmclient/README.md` what slice 2 added (the storage layer, the golden files, the compat gate). Append "Notes after slice 2" to the spec: the `qtstream` subset and its probe, the golden vault and mailbox files, `bmc/sqlite.h` vs the Qt `src/sqlite_helpers.h` kept for `message_search`, and the record types that stayed Qt in the veneer (`bm::Identity`, `bm::Message`, `bm::ChatMessage`, `bm::ChatSummary` and the small records) while the core has `bmc::` ones.

- [ ] **Step 7: Commit**: `docs(core): what slice 2 settled`.

## Notes for slice 3

- `bmc::Client` owns a `Vault`, a `Mailbox`, a `Cache` and a `Delivery` through shared pointers; `bm::Mailbox` already holds a `std::shared_ptr<bmc::Mailbox>`, so `Session` can hand the same one to a `Client`.
- The `bmc::Mailbox` methods `Delivery` needs (`jobs`, `updateJob`, `setDelivery`, `acknowledge`, `checkpoint`, `advance`, `bindCache`, `bindIdentities`, `endChatDeliveries`, `Batch`) are all in place after Task 4.
- The scan's per-batch `Batch` and the `messageRevision` hook move with `Cache`/`Delivery`; keep the hook's table list identical (`messages`, `outbox`, `chat_messages`, `chat_outbox`, `settings`, `delivery_events`).
