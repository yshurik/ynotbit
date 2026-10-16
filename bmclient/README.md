# bmclient

The core of a Bitmessage client with no Qt: bytes and UTF-8 helpers, a JSON reader, identities and
keys, the objects of the network (`Wire`), the simple message of the engine the app started from
(`Protocol`), keys from a phrase, the chat format, update-notice helpers and proof of work. The
Qt app reaches it through thin wrappers in `src/`; the bot library `bmbot` (a later slice) is built
on it. Design: `docs/superpowers/specs/2026-10-15-qt-free-core-and-bot-platform-design.md`.

- Headers `include/bmc/*.h` (`#include <bmc/wire.h>`), sources `src/*.cpp`, namespace `bmc`, C++20.
- Depends on std, OpenSSL, libsodium, libsecp256k1 and the notbit C codec (`third_party/notbit`).
  Nothing here includes or links Qt.
- Text is UTF-8 `std::string`, bytes are `bmc::Bytes` / `bmc::ByteView`. Errors are `bmc::Error`
  (a `std::runtime_error` whose `what()` is English text and which carries a `Code`).

Build and test without Qt:

    cmake -S . -B build-core -G Ninja -DYNOTBIT_QT=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build build-core
    ctest --test-dir build-core --output-on-failure

`scripts/check-qt-free.sh [ssh-host]` does it in a Debian container that has no Qt.

Tests are in `tests/bmclient/` and use the small harness in `include/bmc/test.h`. With Qt, the tests in
`tests/bmclient/parity/` also run Qt's own functions beside the core.
