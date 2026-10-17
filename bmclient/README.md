# bmclient

The core of a Bitmessage client with no Qt: bytes and UTF-8 helpers, a JSON reader, identities and
keys, the objects of the network (`Wire`), the simple message of the engine the app started from
(`Protocol`), keys from a phrase, the chat format, update-notice helpers, proof of work, and the
app's storage: the vault (the identities and mailbox keys in one encrypted file) and the mailbox
(letters, chats, deliveries, jobs, keys, subscriptions, contacts and settings in one SQLCipher
file). The Qt app reaches it through thin wrappers in `src/`; the bot library `bmbot` (a later
slice) is built on it. Design:
`docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md`.

- Headers `include/bmc/*.h` (`#include <bmc/wire.h>`), sources `src/*.cpp`, namespace `bmc`, C++20.
- Depends on std, OpenSSL, libsodium, libsecp256k1, SQLCipher and the notbit C codec
  (`third_party/notbit`). Nothing here includes or links Qt.
- Text is UTF-8 `std::string`, bytes are `bmc::Bytes` / `bmc::ByteView`, a time is `int64_t` seconds
  or milliseconds (`bmc::now()`, `bmc::nowMillis()`), a chat's id is `bmc::uuid()`. Errors are
  `bmc::Error` (a `std::runtime_error` whose `what()` is the English text the translation catalogue
  lists, and which carries a `Code`).
- The bytes on disk are the previous release's: the vault is a `QDataStream` at version `Qt_6_0`
  inside XChaCha20-Poly1305 (written and read by `bmc::qtstream`), and the mailbox is the same
  SQLCipher schema, migrated the same way. `tests/golden/` holds vaults and mailboxes that release
  wrote, which the vault's and the mailbox's tests read; `scripts/compat-check.sh` opens files both
  ways against that release's own code.

Build and test without Qt:

    cmake -S . -B build-core -G Ninja -DYNOTBIT_QT=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build build-core
    ctest --test-dir build-core --output-on-failure

`scripts/check-qt-free.sh [ssh-host]` does it in a Debian container that has no Qt.

Tests are in `tests/bmclient/` and use the small harness in `include/bmc/test.h`. Most are unit
tests; the vault's and the mailbox's also read the frozen files in `tests/golden/`. With Qt,
`tests/bmclient/parity/` runs Qt's own functions beside the core's (text, JSON, chat, updates, and
the `QDataStream` the vault payload is written with). `scripts/compat-check.sh` is the
cross-release gate: it builds the pre-slice commit in a worktree with the Qt app's code and
cross-opens a vault and a mailbox both ways. It stays a script, not a test, because it builds
another tree.
