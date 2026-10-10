# New Vault Dialog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One dialog creates a vault with its first identity: the address (random, or from a secret phrase, optionally searched shorter) is shown with a coolname nickname before the vault file is written.

**Architecture:** `src/key_search.{h,cpp}` (in `bmcore`) derives PyBitmessage's phrase keys and runs the multi-threaded shorter-address search on libsecp256k1, as `tools/bm-shortaddr` on big1 does. `Vault::addIdentity(const Identity &)` and `Session::createVault(path, password, repeated, identity)` save a vault around keys made beforehand. `src/new_vault_dialog.{h,cpp}` is the UI; `DesktopWindow::newVault()` opens it from File → Create vault… and the lock screen, and the lock screen's inline create mode goes.

**Tech Stack:** C++20, Qt 6.8 Widgets, libsecp256k1 v0.6.0 (already linked), OpenSSL SHA-512, libsodium `randombytes_buf`, the repo's `require()` tests run by ctest.

**Spec:** `docs/superpowers/specs/2026-10-10-new-vault-dialog-design.md`

## Global Constraints

- Phrase keys are PyBitmessage's: signing key = SHA-512(phrase + varint(2·pair))[:32], encryption key = SHA-512(phrase + varint(2·pair + 1))[:32], phrase as UTF-8; version 4, stream 1, not a chan.
- A phrase's standard address is its first pair with ≥ 1 zero byte; the search reports, for a phrase, only the *first* pair at each new level (every lower pair searched), on every core but one.
- Nickname: `addressName()` words, capitalised except "of", "from", "the" after the first. File: `<folder>/<slug>.bmvault`, `-2`, `-3`… past existing files; slug = lower case, letters and digits, other runs one `-`, "vault" if empty.
- Existing vault files are never overwritten. The phrase is never stored. No mailbox is created.
- Tests never write into the real `~/Documents`: the dialog takes its folder as a parameter.
- Every new UI string is translated into ru, uk, ja, ko, zh_CN, zh_TW (`scripts/check-translations.py` must pass).
- Formatting: `/opt/local/bin/clang-format -i --lines=A:B` on changed lines; new files formatted whole.
- Build: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8`; suite: `caffeinate -i ctest --test-dir build --output-on-failure` (22 tests).
- Commits only when the user asks; never stage `docs/superpowers/plans/2026-10-04-sqlite-object-store.md`, `reports/`, `research_notes/`.

## Review Focus

- A phrase with non-ASCII letters gives the same address as `keys-from-phrase.py` (UTF-8 bytes, nothing normalised).
- Cancelling the dialog while "Make it shorter" runs stops the threads at once; nothing hangs or crashes.
- Editing the phrase while the search runs stops it, and the address shown is the new phrase's, never a late find for the old one.
- A nickname in another script (Cyrillic, Japanese) keeps its letters in the file name.
- A vault that cannot be written (a file name too long) leaves the dialog open with the error, the keys intact for another try.

Each is pinned by a test in the task that owns the code (Tasks 1 and 4).

---

### Task 1: Keys from a phrase

**Files:**
- Create: `src/key_search.h`, `src/key_search.cpp`
- Modify: `CMakeLists.txt` (`add_library(bmcore …)`: add `src/key_search.cpp`)
- Test: `tests/core_tests.cpp`

**Interfaces:**
- Produces: `Identity bm::phraseIdentity(const QString &phrase, const QString &label, quint64 pair)`, `std::optional<quint64> bm::firstPair(const QString &phrase, int zeroBytes, quint64 limit = 1000000)`, `int bm::zeroBytes(const QString &address)`; internal helpers `phraseKey`, `pairZeroBytes`, `Context` in `key_search.cpp`'s anonymous namespace (Task 2 adds to the same file).

- [ ] **Step 1: Write the failing test**

In `tests/core_tests.cpp`, add `#include "key_search.h"` after `#include "gpu_pow.h"`, and before the `std::cout << "PASS: vault, password rotation…` line:

```cpp
        {
            // Keys from a phrase, as PyBitmessage and scripts/keys-from-phrase.py
            // (big1) derive them.
            const QString phrase = "ynotbit test phrase";
            require(firstPair(phrase, 1) == 60u &&
                        phraseIdentity(phrase, "x", 60).address ==
                            "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN",
                    "a phrase's standard address is its first key pair with a zero byte");
            require(firstPair(phrase, 2) == 27122u &&
                        phraseIdentity(phrase, "x", 27122).address ==
                            "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6",
                    "its shorter address is its first key pair with two");
            require(firstPair("пароль ünïcode фраза", 1) == 252u &&
                        phraseIdentity("пароль ünïcode фраза", "x", 252).address ==
                            "BM-2cUqyPSNiBCvTg51hztE4uWrxET3386Ezo",
                    "a phrase is its UTF-8 bytes, nothing normalised");
            const auto general = firstPair("general", 1);
            require(general.has_value(), "the phrase 'general' has a standard address");
            const auto identity = phraseIdentity("general", "Mine", *general);
            require(identity.address == "BM-2cW67GEKkHGonXKZLCzouLLxnLym3azS8r" && !identity.chan &&
                        identity.label == "Mine" &&
                        identity.address == Protocol::channel("general", "[chan] general").address,
                    "'general' gives [chan] general's address, as an identity of its own");
            require(zeroBytes("BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6") == 2 &&
                        zeroBytes("BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN") == 1 &&
                        zeroBytes("nonsense") == 0,
                    "an address tells how many zero bytes its hash starts with");
        }
```

In `CMakeLists.txt`, in `add_library(bmcore …)`, insert ` src/key_search.cpp` after `src/protocol_wire.cpp`.

- [ ] **Step 2: Run it to see it fail**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target core_tests`
Expected: FAIL: `Cannot find source file: src/key_search.cpp` (or `key_search.h` not found).

- [ ] **Step 3: Write the derivation**

Create `src/key_search.h`:

```cpp
#pragma once
// Keys for a new identity: from a secret phrase, as PyBitmessage derives a
// deterministic address, and the search for shorter addresses (more zero bytes
// at the start of the RIPE hash), as tools/bm-shortaddr does.
#include "storage.h"
#include <QString>
#include <optional>
namespace bm {
// The keys of a phrase's key pair `pair`: signing key = SHA-512(phrase +
// varint(2 pair))[:32], encryption key = SHA-512(phrase + varint(2 pair + 1))[:32];
// a version 4 address in stream 1, not a chan.
Identity phraseIdentity(const QString &phrase, const QString &label, quint64 pair);
// The first of a phrase's key pairs whose RIPE hash starts with at least
// zeroBytes zero bytes, looking at no more than limit pairs. 1 gives the
// phrase's standard address, PyBitmessage's first; 2 its "shorter" one.
std::optional<quint64> firstPair(const QString &phrase, int zeroBytes, quint64 limit = 1000000);
// How many zero bytes an address's RIPE hash starts with; 0 if it is no address.
int zeroBytes(const QString &address);
} // namespace bm
```

Create `src/key_search.cpp`:

```cpp
#include "key_search.h"
#include "protocol.h"
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <secp256k1.h>
extern "C" {
#include "config.h"
#include "ntb-address.h"
}
#include <cstring>
namespace bm {
namespace {
// A Bitmessage variable-length integer.
QByteArray varint(quint64 n) {
    QByteArray v;
    const auto bigEndian = [&](int bytes) {
        for (int shift = 8 * (bytes - 1); shift >= 0; shift -= 8)
            v.append(char(n >> shift));
    };
    if (n < 0xfd) {
        v.append(char(n));
    } else if (n <= 0xffff) {
        v.append(char(0xfd));
        bigEndian(2);
    } else if (n <= 0xffffffffULL) {
        v.append(char(0xfe));
        bigEndian(4);
    } else {
        v.append(char(0xff));
        bigEndian(8);
    }
    return v;
}
// One of a phrase's private keys: SHA-512(phrase + varint(nonce))[:32].
void phraseKey(const QByteArray &phrase, quint64 nonce, unsigned char out[32]) {
    auto input = phrase + varint(nonce);
    unsigned char digest[64];
    SHA512(reinterpret_cast<const unsigned char *>(input.constData()), size_t(input.size()),
           digest);
    std::memcpy(out, digest, 32);
    OPENSSL_cleanse(digest, sizeof digest);
    OPENSSL_cleanse(input.data(), size_t(input.size()));
}
// A libsecp256k1 context of one's own: making public keys needs one.
struct Context {
    secp256k1_context *p = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    ~Context() {
        secp256k1_context_destroy(p);
    }
};
// The zero bytes a key pair's RIPE hash starts with; -1 for a key out of range
// (PyBitmessage would fail on it too).
int pairZeroBytes(const secp256k1_context *ctx, const unsigned char sign[32],
                  const unsigned char enc[32]) {
    unsigned char keys[130];
    secp256k1_pubkey point;
    size_t length = 65;
    if (!secp256k1_ec_pubkey_create(ctx, &point, sign))
        return -1;
    secp256k1_ec_pubkey_serialize(ctx, keys, &length, &point, SECP256K1_EC_UNCOMPRESSED);
    length = 65;
    if (!secp256k1_ec_pubkey_create(ctx, &point, enc))
        return -1;
    secp256k1_ec_pubkey_serialize(ctx, keys + 65, &length, &point, SECP256K1_EC_UNCOMPRESSED);
    ntb_address a;
    ntb_address_from_network_keys(&a, 4, 1, keys + 1, keys + 66);
    int zeros = 0;
    while (zeros < 20 && a.ripe[zeros] == 0)
        ++zeros;
    return zeros;
}
// An identity from its two private keys.
Identity fromKeys(const unsigned char sign[32], const unsigned char enc[32]) {
    QByteArray s(reinterpret_cast<const char *>(sign), 32), e(reinterpret_cast<const char *>(enc), 32);
    auto identity = Protocol::fromPrivate({}, s, e, 4, 1, false);
    OPENSSL_cleanse(s.data(), 32);
    OPENSSL_cleanse(e.data(), 32);
    return identity;
}
} // namespace
Identity phraseIdentity(const QString &phrase, const QString &label, quint64 pair) {
    auto bytes = phrase.toUtf8();
    unsigned char sign[32], enc[32];
    phraseKey(bytes, 2 * pair, sign);
    phraseKey(bytes, 2 * pair + 1, enc);
    auto identity = fromKeys(sign, enc);
    identity.label = label;
    OPENSSL_cleanse(sign, 32);
    OPENSSL_cleanse(enc, 32);
    OPENSSL_cleanse(bytes.data(), size_t(bytes.size()));
    return identity;
}
std::optional<quint64> firstPair(const QString &phrase, int zeroBytes, quint64 limit) {
    auto bytes = phrase.toUtf8();
    Context ctx;
    unsigned char sign[32], enc[32];
    std::optional<quint64> found;
    for (quint64 pair = 0; pair < limit && !found; ++pair) {
        phraseKey(bytes, 2 * pair, sign);
        phraseKey(bytes, 2 * pair + 1, enc);
        if (pairZeroBytes(ctx.p, sign, enc) >= zeroBytes)
            found = pair;
    }
    OPENSSL_cleanse(sign, 32);
    OPENSSL_cleanse(enc, 32);
    OPENSSL_cleanse(bytes.data(), size_t(bytes.size()));
    return found;
}
int zeroBytes(const QString &address) {
    ntb_address a;
    if (!ntb_address_decode(&a, address.toLatin1().constData()))
        return 0;
    int zeros = 0;
    while (zeros < 20 && a.ripe[zeros] == 0)
        ++zeros;
    return zeros;
}
} // namespace bm
```

- [ ] **Step 4: Run it to see it pass**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target core_tests && build/core_tests`
Expected: `PASS: vault, password rotation, …`

- [ ] **Step 5: Commit (when the user asks)**

```bash
git add src/key_search.h src/key_search.cpp tests/core_tests.cpp CMakeLists.txt
git commit -m "feat(keys): a phrase's keys, as PyBitmessage derives them"
```

---

### Task 2: The shorter-address search

**Files:**
- Modify: `src/key_search.h`, `src/key_search.cpp`
- Test: `tests/core_tests.cpp`

**Interfaces:**
- Consumes: `phraseKey`, `pairZeroBytes`, `Context`, `fromKeys` (Task 1, anonymous namespace of `key_search.cpp`).
- Produces: `struct bm::KeyFind { Identity identity; int zeroBytes; qint64 pair; }`; `class bm::KeySearch : QObject` with `explicit KeySearch(int threads = 0, QObject *parent = nullptr)`, `void start(const QString &phrase, int fromZeroBytes)`, `void start(int fromZeroBytes)`, `void stop()`, `bool running() const`, `KeyFind take()`, signals `void found()`, `void progress(double pairsPerSecond)`.

- [ ] **Step 1: Write the failing test**

In `tests/core_tests.cpp`, after Task 1's block (before the `PASS` line):

```cpp
        {
            // The search for shorter addresses: the same first find on any number
            // of threads; random keys that rebuild their address; a prompt stop.
            const QString phrase = "ynotbit test phrase";
            const auto first = [](KeySearch &search) {
                int announced = 0;
                QObject::connect(&search, &KeySearch::found, [&] { ++announced; });
                QElapsedTimer waited;
                waited.start();
                while (announced == 0 && waited.elapsed() < 60000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                search.stop();
                return search.take();
            };
            for (int threads : {1, 4}) {
                KeySearch search(threads);
                search.start(phrase, 2);
                require(search.running(), "a search runs once started");
                const auto find = first(search);
                require(find.pair == 27122 && find.zeroBytes == 2 &&
                            find.identity.address == "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6" &&
                            !search.running(),
                        "a phrase's search finds its first pair with two zero bytes, on any "
                        "number of threads");
                require(search.take().identity.address.isEmpty(), "a find is taken once");
            }
            {
                KeySearch search;
                search.start(2);
                const auto find = first(search);
                const auto keys = reinterpret_cast<const char *>(find.identity.keys.data());
                require(find.pair == -1 && find.zeroBytes >= 2 &&
                            zeroBytes(find.identity.address) == find.zeroBytes &&
                            Protocol::fromPrivate("x", QByteArray(keys, 32),
                                                  QByteArray(keys + 32, 32), 4, 1, false)
                                    .address == find.identity.address,
                        "random keys with two zero bytes rebuild their address");
            }
            {
                KeySearch search;
                search.start(phrase, 6); // about 2.8e14 pairs: never, here
                QThread::msleep(100);
                QElapsedTimer stopping;
                stopping.start();
                search.stop();
                require(!search.running() && stopping.elapsed() < 1000 &&
                            search.take().identity.address.isEmpty(),
                        "a search stops at once, with nothing found");
            }
        }
```

- [ ] **Step 2: Run it to see it fail**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target core_tests`
Expected: FAIL to build: unknown type name `KeySearch`.

- [ ] **Step 3: Write the search**

In `src/key_search.h`, add includes `#include <QElapsedTimer>`, `#include <QObject>`, `#include <memory>`, add `class QTimer;` before `namespace bm {`, and before the closing `} // namespace bm`:

```cpp
// An address the search found: its keys, and its key pair (-1 for random keys).
struct KeyFind {
    Identity identity; // address empty: nothing found
    int zeroBytes = 0;
    qint64 pair = -1;
};
// Searches, on its own threads, for ever shorter addresses: a phrase's key
// pairs from 0 up, or random keys. Each address shorter than the last one found
// is kept and announced by found(); take() hands its keys over.
class KeySearch : public QObject {
    Q_OBJECT
  public:
    // threads: 0 for every core but one.
    explicit KeySearch(int threads = 0, QObject *parent = nullptr);
    ~KeySearch() override;
    // A phrase's key pairs, from 0, for addresses of at least fromZeroBytes zero
    // bytes. A find is announced once every lower pair has been searched, so it
    // is the phrase's first pair with that many, whichever thread found it.
    void start(const QString &phrase, int fromZeroBytes);
    // Random keys (libsodium's randombytes_buf), at least fromZeroBytes.
    void start(int fromZeroBytes);
    void stop();
    bool running() const;
    // The best address found since the last take(); its identity's address is
    // empty if there is none.
    KeyFind take();

  signals:
    void found();
    void progress(double pairsPerSecond);

  private:
    struct Run;
    std::unique_ptr<Run> run_;
    int threads_;
    QTimer *timer_;
    quint64 lastDone_ = 0;
    QElapsedTimer clock_;
    void begin(std::unique_ptr<Run> run);
};
```

In `src/key_search.cpp`, add includes `#include <QThread>`, `#include <QTimer>`, `#include <sodium.h>`, `#include <algorithm>`, `#include <atomic>`, `#include <map>`, `#include <mutex>`, `#include <thread>`, `#include <utility>`, `#include <vector>`, and before the closing `} // namespace bm`:

```cpp
namespace {
constexpr quint64 kChunk = 4096; // pairs a thread takes at a time
} // namespace
struct KeySearch::Run {
    QByteArray phrase; // empty: random keys
    int fromZeroBytes = 1;
    std::atomic<bool> stop{false};
    std::atomic<quint64> nextChunk{0}, pairsDone{0};
    std::mutex lock;
    // A phrase's finished chunks and their hits (pair, zero bytes), until every
    // lower chunk is finished too.
    std::map<quint64, std::vector<std::pair<quint64, int>>> finished;
    quint64 watermark = 0; // every chunk below it is reported
    int reported = 0;      // zero bytes of the best address announced
    KeyFind best;
    std::vector<std::thread> threads;
    ~Run() {
        if (!phrase.isEmpty())
            OPENSSL_cleanse(phrase.data(), size_t(phrase.size()));
    }
};
KeySearch::KeySearch(int threads, QObject *parent)
    : QObject(parent),
      threads_(threads > 0 ? threads : std::max(1, QThread::idealThreadCount() - 1)),
      timer_(new QTimer(this)) {
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (!run_)
            return;
        const auto done = run_->pairsDone.load();
        const auto seconds = std::max<qint64>(1, clock_.restart()) / 1000.0;
        emit progress((done - lastDone_) / seconds);
        lastDone_ = done;
    });
}
KeySearch::~KeySearch() {
    stop();
}
bool KeySearch::running() const {
    return run_ && !run_->threads.empty();
}
void KeySearch::stop() {
    timer_->stop();
    if (!run_)
        return;
    run_->stop = true;
    for (auto &thread : run_->threads)
        thread.join();
    run_->threads.clear();
}
KeyFind KeySearch::take() {
    if (!run_)
        return {};
    std::lock_guard guard(run_->lock);
    return std::exchange(run_->best, KeyFind{});
}
void KeySearch::start(const QString &phrase, int fromZeroBytes) {
    auto run = std::make_unique<Run>();
    run->phrase = phrase.toUtf8();
    run->fromZeroBytes = fromZeroBytes;
    begin(std::move(run));
}
void KeySearch::start(int fromZeroBytes) {
    auto run = std::make_unique<Run>();
    run->fromZeroBytes = fromZeroBytes;
    begin(std::move(run));
}
void KeySearch::begin(std::unique_ptr<Run> next) {
    stop();
    run_ = std::move(next);
    run_->reported = run_->fromZeroBytes - 1;
    auto run = run_.get();
    // found() is emitted on this object's thread, never on a search thread's.
    const auto announce = [this] {
        QMetaObject::invokeMethod(this, [this] { emit found(); }, Qt::QueuedConnection);
    };
    for (int t = 0; t < threads_; ++t)
        run->threads.emplace_back([run, announce] {
            Context ctx;
            unsigned char sign[32], enc[32];
            std::vector<std::pair<quint64, int>> hits;
            while (!run->stop) {
                bool news = false;
                if (run->phrase.isEmpty()) {
                    quint64 searched = 0;
                    for (; searched < kChunk && !run->stop; ++searched) {
                        randombytes_buf(sign, 32);
                        randombytes_buf(enc, 32);
                        const int zeros = pairZeroBytes(ctx.p, sign, enc);
                        if (zeros < run->fromZeroBytes)
                            continue;
                        std::lock_guard guard(run->lock);
                        if (zeros > run->reported) {
                            run->reported = zeros;
                            run->best = {fromKeys(sign, enc), zeros, -1};
                            news = true;
                        }
                    }
                    run->pairsDone += searched;
                } else {
                    const auto chunk = run->nextChunk++;
                    hits.clear();
                    quint64 i = 0;
                    for (; i < kChunk && !run->stop; ++i) {
                        const auto pair = chunk * kChunk + i;
                        phraseKey(run->phrase, 2 * pair, sign);
                        phraseKey(run->phrase, 2 * pair + 1, enc);
                        if (const int zeros = pairZeroBytes(ctx.p, sign, enc);
                            zeros >= run->fromZeroBytes)
                            hits.emplace_back(pair, zeros);
                    }
                    if (i < kChunk) // stopped: an unfinished chunk is not reported
                        break;
                    run->pairsDone += kChunk;
                    std::lock_guard guard(run->lock);
                    run->finished[chunk] = hits;
                    // Report in pair order: the first pair at each new level.
                    for (auto it = run->finished.find(run->watermark); it != run->finished.end();
                         it = run->finished.find(run->watermark)) {
                        for (const auto &[pair, zeros] : it->second)
                            if (zeros > run->reported) {
                                run->reported = zeros;
                                phraseKey(run->phrase, 2 * pair, sign);
                                phraseKey(run->phrase, 2 * pair + 1, enc);
                                run->best = {fromKeys(sign, enc), zeros, qint64(pair)};
                                news = true;
                            }
                        run->finished.erase(it);
                        ++run->watermark;
                    }
                }
                if (news)
                    announce();
            }
            OPENSSL_cleanse(sign, 32);
            OPENSSL_cleanse(enc, 32);
        });
    lastDone_ = 0;
    clock_.start();
    timer_->start();
}
```

- [ ] **Step 4: Run it to see it pass**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target core_tests && build/core_tests`
Expected: `PASS: vault, password rotation, …`

- [ ] **Step 5: Commit (when the user asks)**

```bash
git add src/key_search.h src/key_search.cpp tests/core_tests.cpp
git commit -m "feat(keys): search for shorter addresses on every core but one"
```

---

### Task 3: Saving a vault around keys made beforehand

**Files:**
- Modify: `src/storage.h`, `src/storage.cpp` (`Vault::addIdentity` overload), `src/session.h`, `src/session.cpp` (`Session::createVault` with arguments)
- Modify: `translations/ynotbit_*.ts`
- Test: `tests/core_tests.cpp`, `tests/widgets_tests.cpp`

**Interfaces:**
- Consumes: `bm::phraseIdentity` (Task 1, in tests).
- Produces: `QString bm::Vault::addIdentity(const Identity &identity)`; `bool bm::Session::createVault(const QString &path, QString password, QString repeated, const Identity &identity)` (true on success; `error()` says why not).

- [ ] **Step 1: Write the failing tests**

In `tests/core_tests.cpp`, after Task 2's block:

```cpp
        {
            // A vault keeps an identity made before it, as made.
            Vault fresh;
            fresh.create(d.filePath("given.bmvault"), "long test password");
            const auto made = phraseIdentity("ynotbit test phrase", "Shown first", 60);
            require(fresh.addIdentity(made) == made.address && fresh.identities().size() == 1 &&
                        fresh.identities()[0].label == "Shown first" &&
                        fresh.identities()[0].isDefault &&
                        std::memcmp(fresh.identities()[0].keys.data(), made.keys.data(), 64) == 0,
                    "a vault adds an identity made beforehand, keys and label as they were");
            rejects([&] { fresh.addIdentity(made); });
            auto forged = phraseIdentity("ynotbit test phrase", "x", 61);
            forged.address = phraseIdentity("ynotbit test phrase", "x", 252).address;
            rejects([&] { fresh.addIdentity(forged); });
            require(fresh.identities().size() == 1,
                    "an identity already there, or keys under another address, are refused");
        }
```

(`<cstring>` for `std::memcmp`: add `#include <cstring>` to core_tests.cpp's includes.)

In `tests/widgets_tests.cpp`, add `#include "key_search.h"` after `#include "i18n.h"`, and after the line `"UPnP runs only online, listening, switched on and without a proxy");`:

```cpp
        {
            // A vault made with the identity it starts with.
            bm::Session fresh(temp.filePath("made-vault-node"), true);
            const auto made = bm::phraseIdentity("ynotbit test phrase", "Mysterious Poised Shellfish", 60);
            const auto path = temp.filePath("made.bmvault");
            require(!fresh.createVault(path, "one password", "another one", made) &&
                        fresh.error().contains("match") && !QFile::exists(path),
                    "mismatched passwords make no vault");
            require(!fresh.createVault(path, "", "", made) && !QFile::exists(path),
                    "an empty password makes no vault");
            require(fresh.createVault(path, "long test password", "long test password", made) &&
                        fresh.unlocked() && QFile::exists(path),
                    "a vault is made, and open");
            const auto identities = fresh.identities();
            require(identities.size() == 1 &&
                        identities[0].toMap()["address"].toString() == made.address &&
                        identities[0].toMap()["label"].toString() == "Mysterious Poised Shellfish" &&
                        identities[0].toMap()["default"].toBool(),
                    "its one identity is the one shown, with its nickname, as default");
            require(!fresh.createVault(temp.filePath("second.bmvault"), "pw", "pw", made) &&
                        fresh.error().contains("Lock"),
                    "an open vault is locked before another is made");
            fresh.lock();
            require(!fresh.createVault(path, "pw", "pw", made) &&
                        fresh.error().contains("existing vaults"),
                    "an existing vault file is never overwritten");
        }
```

- [ ] **Step 2: Run them to see them fail**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target core_tests desktop_tests`
Expected: FAIL to build: no matching `addIdentity(const Identity &)`; no matching `createVault(…)`.

- [ ] **Step 3: Write the overload and createVault**

In `src/storage.h`, after `QString addIdentity(const QString &label);`:

```cpp
    // An identity made beforehand (the new vault dialog's): its keys are checked
    // against its address and kept as they are.
    QString addIdentity(const Identity &identity);
```

In `src/storage.cpp`, after `Vault::addIdentity(const QString &label)`:

```cpp
QString Vault::addIdentity(const Identity &identity) {
    check(unlocked_, "Vault is locked");
    const auto keys = reinterpret_cast<const char *>(identity.keys.data());
    auto copy = Protocol::fromPrivate(identity.label, QByteArray(keys, 32), QByteArray(keys + 32, 32),
                                      4, 1, false);
    check(copy.address == identity.address, "Identity keys do not match its address");
    for (const auto &i : identities_)
        check(i.address != copy.address, "Identity already in the vault");
    identities_.push_back(std::move(copy));
    if (identities_.size() == 1)
        identities_.back().isDefault = true;
    try {
        save();
    } catch (...) {
        identities_.pop_back();
        throw;
    }
    return identities_.back().address;
}
```

In `src/session.h`, after `Q_INVOKABLE void createVault();`:

```cpp
    // A new vault at path holding identity, its first and default; true when
    // made, else error() says why.
    bool createVault(const QString &path, QString password, QString repeated,
                     const Identity &identity);
```

In `src/session.cpp`, after `void Session::createVault() { … }`:

```cpp
bool Session::createVault(const QString &path, QString password, QString repeated,
                          const Identity &identity) {
    attempt([&] {
        check(!unlocked(), tr("Lock the current vault first"));
        check(!password.isEmpty(), tr("Password cannot be empty"));
        check(password == repeated, tr("Passwords do not match"));
        check(!QFile::exists(path),
              tr("Choose a new filename; existing vaults are never overwritten"));
        Password secret{password.toUtf8()};
        password.fill(QChar(0));
        repeated.fill(QChar(0));
        acquireVault(path);
        try {
            vault_.create(path, secret.bytes);
            vault_.addIdentity(identity);
        } catch (...) {
            vault_.lock();
            QFile::remove(path); // made by this call, unfinished
            vaultLock_.reset();
            throw;
        }
        vaultPath_ = path;
        mailPath_.clear();
        mailKey_.clear();
        rememberVault(path);
        activity_ = tr("Vault created with %1. Create a mailbox to start.").arg(identity.label);
    });
    return error_.isEmpty();
}
```

- [ ] **Step 4: Run them to see them pass**

Run: the build of Step 2, then `build/core_tests && (cd build && QT_QPA_PLATFORM=offscreen ./desktop_tests 2>&1 | grep -E '^FAIL|^PASS')`
Expected: core `PASS: …`, desktop `PASS Widgets mailbox selection, rendering and lock`

- [ ] **Step 5: Translate the new string**

Run `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --target update_translations`, then set the `bm::Session` translation of "Vault created with %1. Create a mailbox to start." (drop `type="unfinished"`):

| ru | uk | ja | ko | zh_CN | zh_TW |
|---|---|---|---|---|---|
| Хранилище создано с %1. Создайте почтовый ящик, чтобы начать. | Сховище створено з %1. Створіть поштову скриньку, щоб почати. | %1 で保管庫を作成しました。メールボックスを作成して始めましょう。 | %1(으)로 보관함을 만들었습니다. 메일함을 만들어 시작하세요. | 已用 %1 新建保险库。新建一个邮箱即可开始。 | 已用 %1 新建保險庫。新建一個信箱即可開始。 |

Run: `python3 scripts/check-translations.py`
Expected: `PASS: 6 translations complete`

- [ ] **Step 6: Commit (when the user asks)**

```bash
git add src/storage.h src/storage.cpp src/session.h src/session.cpp tests/core_tests.cpp tests/widgets_tests.cpp translations
git commit -m "feat(vault): a new vault saved around the identity it starts with"
```

---

### Task 4: The new vault dialog

**Files:**
- Create: `src/new_vault_dialog.h`, `src/new_vault_dialog.cpp`
- Modify: `CMakeLists.txt` (an `add_coolname()` function before `qt_add_executable(ynotbit …)`; the dialog and coolname in `ynotbit`, `desktop_tests`, `readme_screenshots`, `feed_scroll_bench`; `coolname_tests` through `add_coolname`; lupdate `SOURCES`), `scripts/check-translations.py` (scan `new_vault_dialog.cpp`), `translations/ynotbit_*.ts`
- Test: `tests/widgets_tests.cpp`

**Interfaces:**
- Consumes: `bm::phraseIdentity`, `bm::firstPair`, `bm::zeroBytes`, `bm::KeySearch`, `bm::KeyFind` (Tasks 1–2); `bm::Session::createVault` (Task 3); `bm::addressName` (`coolname.h`); `bm::identiconPixmap`, `bm::materialIcon`, `bm::iconColor` (`letter_render.h`); `bm::addressFont` (`appearance.h`); `bm::Protocol::identity`.
- Produces: `class bm::NewVaultDialog : QDialog` with `NewVaultDialog(Session &session, const QString &folder, bool dark, QWidget *parent = nullptr)`, `static QString nicknameFor(const QString &address)`, `static QString slugFor(const QString &nickname)`; object names `newVaultDialog`, `nicknameField`, `addressLabel`, `copyNewAddressButton`, `randomKeysButton`, `anotherKeysButton`, `phraseKeysButton`, `phraseField`, `showPhraseButton`, `shorterButton`, `searchStatus`, `rebuildNote`, `vaultFileField`, `vaultFileButton`, `vaultFileProblem`, `newVaultPasswordField`, `newVaultRepeatField`, `passwordProblem`, `newVaultError`, `newVaultCancelButton`, `newVaultCreateButton`.

- [ ] **Step 1: Write the failing test**

In `tests/widgets_tests.cpp`, add `#include "new_vault_dialog.h"` after `#include "key_search.h"`, and after Task 3's block:

```cpp
        {
            // The new vault dialog: an address first, then a nickname and a file
            // named after it; the vault around that address.
            const QString documents = temp.filePath("documents");
            QDir().mkpath(documents);
            bm::Session fresh(temp.filePath("dialog-node"), true);
            auto dialog = std::make_unique<bm::NewVaultDialog>(fresh, documents, false);
            dialog->show();
            auto nickname = dialog->findChild<QLineEdit *>("nicknameField");
            auto address = dialog->findChild<QLabel *>("addressLabel");
            auto file = dialog->findChild<QLineEdit *>("vaultFileField");
            auto password = dialog->findChild<QLineEdit *>("newVaultPasswordField");
            auto repeat = dialog->findChild<QLineEdit *>("newVaultRepeatField");
            auto phrase = dialog->findChild<QLineEdit *>("phraseField");
            auto shorter = dialog->findChild<QPushButton *>("shorterButton");
            auto create = dialog->findChild<QPushButton *>("newVaultCreateButton");
            const auto in = [&](const QString &name) { return QDir(documents).filePath(name); };
            const auto first = address->text();
            require(first.startsWith("BM-") && nickname->text() == bm::NewVaultDialog::nicknameFor(first) &&
                        file->text() == in(bm::NewVaultDialog::slugFor(nickname->text()) + ".bmvault"),
                    "the dialog opens with random keys, their nickname, and a file named after it");
            require(bm::NewVaultDialog::nicknameFor("BM-5oSrHVwbsQYPKmePHfqQJZr6eibiqj9") ==
                            "Imperial Conscious Grebe" &&
                        bm::NewVaultDialog::nicknameFor("BM-%1%2") == "Noble Llama of Discourse" &&
                        bm::NewVaultDialog::slugFor("Noble Llama of Discourse") ==
                            "noble-llama-of-discourse" &&
                        bm::NewVaultDialog::slugFor("  Alice's  Work!") == "alice-s-work" &&
                        bm::NewVaultDialog::slugFor("Ёжик в тумане") == "ёжик-в-тумане" &&
                        bm::NewVaultDialog::slugFor("!!!") == "vault",
                    "nicknames are title case; file names are their slug, letters of any script kept");
            dialog->findChild<QPushButton *>("anotherKeysButton")->click();
            require(address->text() != first &&
                        nickname->text() == bm::NewVaultDialog::nicknameFor(address->text()),
                    "Another makes new keys, and renames");
            QFile taken(in("taken.bmvault"));
            require(taken.open(QIODevice::WriteOnly), "make a file to step around");
            taken.close();
            nickname->clear();
            QTest::keyClicks(nickname, "Taken");
            require(file->text() == in("taken-2.bmvault"),
                    "the nickname names the file, numbered past one that exists");
            file->clear();
            QTest::keyClicks(file, in("mine.bmvault"));
            QTest::keyClicks(nickname, " More");
            require(file->text() == in("mine.bmvault"), "a file the user chose stays put");
            QTimer::singleShot(50, dialog.get(), [&] {
                for (auto w : QApplication::topLevelWidgets())
                    if (auto d = qobject_cast<QFileDialog *>(w))
                        chooseFile(d, in("picked"));
            });
            dialog->findChild<QToolButton *>("vaultFileButton")->click();
            require(file->text() == in("picked.bmvault"), "… picks the file, with its suffix");
            require(!create->isEnabled(), "no password, no vault");
            QTest::keyClicks(password, "long test password");
            QTest::keyClicks(repeat, "long test passwor");
            require(!create->isEnabled() &&
                        dialog->findChild<QLabel *>("passwordProblem")->isVisibleTo(dialog.get()),
                    "mismatched passwords are said, and block Create");
            QTest::keyClicks(repeat, "d");
            require(create->isEnabled(), "matching passwords and a new file allow Create");
            file->clear();
            QTest::keyClicks(file, taken.fileName());
            require(!create->isEnabled() &&
                        dialog->findChild<QLabel *>("vaultFileProblem")->isVisibleTo(dialog.get()),
                    "an existing file is refused");
            dialog->findChild<QRadioButton *>("phraseKeysButton")->click();
            QTest::keyClicks(phrase, "ynotbit test phrase");
            QTest::qWait(400);
            require(address->text() == "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN" &&
                        dialog->findChild<QLabel *>("rebuildNote")->text().contains("60"),
                    "a phrase gives its address, and the key pair that rebuilds it");
            shorter->click();
            require(shorter->text() == "Stop", "the search runs");
            QTest::keyClicks(phrase, "!");
            QTest::qWait(400);
            require(shorter->text() == "Make it shorter" &&
                        address->text() == bm::phraseIdentity("ynotbit test phrase!", {},
                                                              *bm::firstPair("ynotbit test phrase!", 1))
                                               .address,
                    "editing the phrase stops the search; the address is the new phrase's");
            QTest::keyClick(phrase, Qt::Key_Backspace);
            QTest::qWait(400);
            shorter->click();
            QTest::qWaitFor(
                [&] { return address->text() == "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6"; }, 60000);
            shorter->click();
            require(address->text() == "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6" &&
                        dialog->findChild<QLabel *>("rebuildNote")->text().contains("27122") &&
                        shorter->text() == "Make it shorter",
                    "Make it shorter finds the shorter address; Stop keeps it");
            nickname->clear();
            QTest::keyClicks(nickname, QString(300, 'n'));
            file->clear();
            QTest::keyClicks(file, in(QString(300, 'n') + ".bmvault"));
            create->click();
            require(dialog->isVisible() &&
                        dialog->findChild<QLabel *>("newVaultError")->isVisibleTo(dialog.get()) &&
                        !fresh.unlocked() && address->text() == "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6",
                    "a vault that cannot be written leaves the dialog open, keys kept, saying why");
            nickname->clear();
            QTest::keyClicks(nickname, "Shorter Me");
            file->clear();
            QTest::keyClicks(file, in("created.bmvault"));
            create->click();
            require(dialog->result() == QDialog::Accepted && fresh.unlocked() &&
                        QFile::exists(in("created.bmvault")),
                    "Create makes the vault, and closes");
            const auto identities = fresh.identities();
            require(identities.size() == 1 &&
                        identities[0].toMap()["address"].toString() ==
                            "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6" &&
                        identities[0].toMap()["label"].toString() == "Shorter Me",
                    "the vault's identity is the address shown, with its nickname");
            require(phrase->text().isEmpty() && password->text().isEmpty() && repeat->text().isEmpty(),
                    "closing clears the phrase and the passwords");
            // Cancelled mid-search: the threads stop at once.
            fresh.lock();
            dialog = std::make_unique<bm::NewVaultDialog>(fresh, documents, false);
            dialog->show();
            dialog->findChild<QPushButton *>("shorterButton")->click();
            QTest::qWait(100);
            QElapsedTimer closing;
            closing.start();
            dialog->reject();
            dialog.reset();
            require(closing.elapsed() < 2000, "cancelling while the search runs stops it at once");
        }
```

- [ ] **Step 2: Run it to see it fail**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target desktop_tests`
Expected: FAIL to build: `new_vault_dialog.h` not found.

- [ ] **Step 3: Wire coolname and the dialog into the GUI targets**

In `CMakeLists.txt`, before `qt_add_executable(ynotbit MACOSX_BUNDLE …`:

```cmake
# Names for addresses: coolname's vocabulary, pinned in third_party/coolname.
set(COOLNAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/coolname)
file(GLOB COOLNAME_DATA CONFIGURE_DEPENDS ${COOLNAME_DIR}/data/*)
# coolname's code with its data: a target naming addresses needs both.
function(add_coolname target)
 target_sources(${target} PRIVATE src/coolname.cpp src/coolname.h)
 qt_add_resources(${target} "coolname_data" PREFIX "/coolname" BASE ${COOLNAME_DIR}
  FILES ${COOLNAME_DATA} ${COOLNAME_DIR}/excluded.txt)
endfunction()
```

Remove the old `set(COOLNAME_DIR …)` and `file(GLOB COOLNAME_DATA …)` lines further down, and make the coolname test block:

```cmake
qt_add_executable(coolname_tests tests/coolname_tests.cpp)
target_include_directories(coolname_tests PRIVATE src)
target_link_libraries(coolname_tests PRIVATE Qt6::Core)
add_coolname(coolname_tests)
add_test(NAME coolname COMMAND coolname_tests ${CMAKE_CURRENT_SOURCE_DIR})
```

In each of `qt_add_executable(ynotbit …`, `qt_add_executable(desktop_tests …`, `qt_add_executable(readme_screenshots …`, `qt_add_executable(feed_scroll_bench …`, add ` src/new_vault_dialog.cpp src/new_vault_dialog.h` after `src/settings_pane.h`, and after each target's `target_include_directories(<target> PRIVATE third_party/qrcodegen)` line add `add_coolname(<target>)`. In `qt_add_lupdate(… SOURCES …)`, add ` src/new_vault_dialog.cpp` after `src/settings_pane.cpp`. In `scripts/check-translations.py`, change the scanned names to `("desktop_window.cpp", "session.cpp", "feed_view.cpp", "updates.cpp", "new_vault_dialog.cpp")`.

- [ ] **Step 4: Write the dialog**

Create `src/new_vault_dialog.h`:

```cpp
#pragma once
// A new vault and its first identity, in one dialog: the address is made, and
// shown, before the vault is.
#include "key_search.h"
#include <QDialog>
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTimer;
class QToolButton;
namespace bm {
class Session;
class NewVaultDialog : public QDialog {
    Q_OBJECT
  public:
    // folder: where the vault file goes unless the user picks another place.
    NewVaultDialog(Session &session, const QString &folder, bool dark, QWidget *parent = nullptr);
    ~NewVaultDialog() override;
    // An address's nickname: its name, each word capitalised but "of", "from"
    // and "the" after the first.
    static QString nicknameFor(const QString &address);
    // A file name from a nickname: lower case, letters and digits, one "-" for
    // every other run; "vault" if nothing is left.
    static QString slugFor(const QString &nickname);

  public slots:
    void done(int result) override;

  private:
    Session &session_;
    QString folder_;
    Identity identity_; // the keys shown; their address empty while there are none
    KeySearch search_;
    bool nicknameEdited_ = false, fileEdited_ = false;
    QLineEdit *nickname_, *phrase_, *file_, *password_, *repeat_;
    QLabel *identicon_, *address_, *searchStatus_, *rebuild_, *fileProblem_, *passwordProblem_,
        *error_;
    QRadioButton *random_, *fromPhrase_;
    QWidget *phraseRow_;
    QPushButton *another_, *shorter_, *create_;
    QToolButton *copy_;
    QTimer *phraseTimer_;
    void setIdentity(Identity identity, qint64 pair);
    void newRandomKeys();
    void phraseChanged();
    void toggleSearch();
    void stopSearch();
    void updateFile();
    void validate();
    void create();
};
} // namespace bm
```

Create `src/new_vault_dialog.cpp`:

```cpp
#include "new_vault_dialog.h"
#include "appearance.h"
#include "coolname.h"
#include "letter_render.h"
#include "protocol.h"
#include "session.h"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
namespace bm {
namespace {
QLabel *note(const QString &text, const QString &name, bool problem = false) {
    auto label = new QLabel(text);
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setStyleSheet(problem ? "color:#d93025;font-size:12px;" : "color:palette(mid);font-size:12px;");
    return label;
}
// About how long, said briefly.
QString duration(double seconds) {
    if (seconds < 60)
        return NewVaultDialog::tr("%1 s").arg(std::max(1, int(seconds)));
    if (seconds < 3600)
        return NewVaultDialog::tr("%1 min").arg(int(seconds / 60));
    if (seconds < 86400)
        return NewVaultDialog::tr("%1 h").arg(int(seconds / 3600));
    return NewVaultDialog::tr("%1 days").arg(int(seconds / 86400));
}
} // namespace
QString NewVaultDialog::nicknameFor(const QString &address) {
    auto words = addressName(address);
    for (qsizetype i = 0; i < words.size(); ++i)
        if (i == 0 || (words[i] != "of" && words[i] != "from" && words[i] != "the"))
            words[i][0] = words[i][0].toUpper();
    return words.join(' ');
}
QString NewVaultDialog::slugFor(const QString &nickname) {
    QString slug;
    for (const auto c : nickname.toLower()) {
        if (c.isLetterOrNumber())
            slug += c;
        else if (!slug.isEmpty() && !slug.endsWith('-'))
            slug += '-';
    }
    while (slug.endsWith('-'))
        slug.chop(1);
    return slug.isEmpty() ? QString("vault") : slug;
}
NewVaultDialog::NewVaultDialog(Session &session, const QString &folder, bool dark, QWidget *parent)
    : QDialog(parent), session_(session), folder_(folder), phraseTimer_(new QTimer(this)) {
    setObjectName("newVaultDialog");
    setWindowTitle(tr("New vault"));
    setMinimumWidth(560);
    auto form = new QFormLayout(this);
    nickname_ = new QLineEdit;
    nickname_->setObjectName("nicknameField");
    form->addRow(tr("Nickname"), nickname_);
    // The address, with its identicon and a copy button.
    identicon_ = new QLabel;
    identicon_->setFixedSize(32, 32);
    address_ = new QLabel;
    address_->setObjectName("addressLabel");
    address_->setFont(addressFont());
    address_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    copy_ = new QToolButton;
    copy_->setObjectName("copyNewAddressButton");
    copy_->setProperty("addressAction", true);
    copy_->setFixedSize(22, 22);
    copy_->setIconSize(QSize(14, 14));
    copy_->setIcon(materialIcon("copy", iconColor(dark)));
    copy_->setToolTip(tr("Copy address"));
    connect(copy_, &QToolButton::clicked, this,
            [this] { QApplication::clipboard()->setText(identity_.address); });
    auto addressRow = new QHBoxLayout;
    addressRow->addWidget(identicon_);
    addressRow->addWidget(address_, 1);
    addressRow->addWidget(copy_);
    form->addRow(tr("Address"), addressRow);
    // Where the keys come from.
    random_ = new QRadioButton(tr("Random, from the system's entropy"));
    random_->setObjectName("randomKeysButton");
    random_->setChecked(true);
    another_ = new QPushButton(tr("Another"));
    another_->setObjectName("anotherKeysButton");
    auto randomRow = new QHBoxLayout;
    randomRow->addWidget(random_, 1);
    randomRow->addWidget(another_);
    fromPhrase_ = new QRadioButton(tr("From a secret phrase"));
    fromPhrase_->setObjectName("phraseKeysButton");
    phrase_ = new QLineEdit;
    phrase_->setObjectName("phraseField");
    phrase_->setEchoMode(QLineEdit::Password);
    phrase_->setPlaceholderText(tr("Secret phrase"));
    auto showPhrase = new QToolButton;
    showPhrase->setObjectName("showPhraseButton");
    showPhrase->setCheckable(true);
    showPhrase->setText(tr("Show"));
    connect(showPhrase, &QToolButton::toggled, this, [this](bool on) {
        phrase_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    phraseRow_ = new QWidget;
    auto phraseColumn = new QVBoxLayout(phraseRow_);
    phraseColumn->setContentsMargins(24, 0, 0, 0);
    auto phraseLine = new QHBoxLayout;
    phraseLine->addWidget(phrase_, 1);
    phraseLine->addWidget(showPhrase);
    phraseColumn->addLayout(phraseLine);
    phraseColumn->addWidget(note(tr("Anyone who knows or guesses this phrase has your keys. Use a "
                                    "long random phrase."),
                                 "phraseWarning"));
    phraseRow_->hide();
    auto keys = new QVBoxLayout;
    keys->addLayout(randomRow);
    keys->addWidget(fromPhrase_);
    keys->addWidget(phraseRow_);
    form->addRow(tr("Keys"), keys);
    // Shorter addresses, searched for until stopped.
    shorter_ = new QPushButton(tr("Make it shorter"));
    shorter_->setObjectName("shorterButton");
    searchStatus_ = note({}, "searchStatus");
    searchStatus_->hide();
    rebuild_ = note({}, "rebuildNote");
    rebuild_->hide();
    auto shorterColumn = new QVBoxLayout;
    shorterColumn->addWidget(shorter_, 0, Qt::AlignLeft);
    shorterColumn->addWidget(searchStatus_);
    shorterColumn->addWidget(rebuild_);
    form->addRow(tr("Shorter"), shorterColumn);
    // The vault file.
    file_ = new QLineEdit;
    file_->setObjectName("vaultFileField");
    auto choose = new QToolButton;
    choose->setObjectName("vaultFileButton");
    choose->setText("…");
    choose->setToolTip(tr("Choose where to save the vault"));
    auto fileRow = new QHBoxLayout;
    fileRow->addWidget(file_, 1);
    fileRow->addWidget(choose);
    fileProblem_ = note(tr("This file exists; existing vaults are never overwritten."),
                        "vaultFileProblem", true);
    fileProblem_->hide();
    auto fileColumn = new QVBoxLayout;
    fileColumn->addLayout(fileRow);
    fileColumn->addWidget(fileProblem_);
    form->addRow(tr("Vault file"), fileColumn);
    // Its password.
    password_ = new QLineEdit;
    password_->setObjectName("newVaultPasswordField");
    password_->setEchoMode(QLineEdit::Password);
    repeat_ = new QLineEdit;
    repeat_->setObjectName("newVaultRepeatField");
    repeat_->setEchoMode(QLineEdit::Password);
    passwordProblem_ = note(tr("The passwords do not match."), "passwordProblem", true);
    passwordProblem_->hide();
    form->addRow(tr("Password"), password_);
    auto repeatColumn = new QVBoxLayout;
    repeatColumn->addWidget(repeat_);
    repeatColumn->addWidget(passwordProblem_);
    form->addRow(tr("Repeat password"), repeatColumn);
    error_ = note({}, "newVaultError", true);
    error_->hide();
    form->addRow(error_);
    auto cancel = new QPushButton(tr("Cancel"));
    cancel->setObjectName("newVaultCancelButton");
    create_ = new QPushButton(tr("Create vault"));
    create_->setObjectName("newVaultCreateButton");
    create_->setDefault(true);
    auto buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(create_);
    form->addRow(buttons);

    connect(nickname_, &QLineEdit::textEdited, this, [this] {
        nicknameEdited_ = true;
        updateFile();
    });
    connect(random_, &QRadioButton::toggled, this, [this](bool on) {
        phraseRow_->setVisible(!on);
        another_->setEnabled(on);
        if (on)
            newRandomKeys();
        else
            phraseChanged();
    });
    connect(another_, &QPushButton::clicked, this, &NewVaultDialog::newRandomKeys);
    phraseTimer_->setSingleShot(true);
    phraseTimer_->setInterval(300);
    connect(phrase_, &QLineEdit::textChanged, this, [this] {
        stopSearch();
        phraseTimer_->start();
    });
    connect(phraseTimer_, &QTimer::timeout, this, &NewVaultDialog::phraseChanged);
    connect(shorter_, &QPushButton::clicked, this, &NewVaultDialog::toggleSearch);
    connect(&search_, &KeySearch::found, this, [this] {
        if (auto find = search_.take(); !find.identity.address.isEmpty())
            setIdentity(std::move(find.identity), find.pair);
    });
    connect(&search_, &KeySearch::progress, this, [this](double rate) {
        const double pairs = std::pow(256.0, zeroBytes(identity_.address) + 1);
        searchStatus_->setText(tr("Searching %1 key pairs a second; the next shorter address takes "
                                  "about %2.")
                                   .arg(QLocale().toString(qRound64(rate)),
                                        duration(pairs / std::max(rate, 1.0))));
        searchStatus_->show();
    });
    connect(file_, &QLineEdit::textEdited, this, [this] {
        fileEdited_ = true;
        validate();
    });
    connect(choose, &QToolButton::clicked, this, [this] {
        auto path = QFileDialog::getSaveFileName(this, tr("Save the vault as"), file_->text(),
                                                 tr("Bitmessage vault (*.bmvault)"));
        if (path.isEmpty())
            return;
        if (!path.endsWith(".bmvault"))
            path += ".bmvault";
        file_->setText(path);
        fileEdited_ = true;
        validate();
    });
    for (auto field : {password_, repeat_})
        connect(field, &QLineEdit::textChanged, this, &NewVaultDialog::validate);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(create_, &QPushButton::clicked, this, &NewVaultDialog::create);
    newRandomKeys();
}
NewVaultDialog::~NewVaultDialog() = default;
void NewVaultDialog::setIdentity(Identity identity, qint64 pair) {
    identity_ = std::move(identity);
    const auto &address = identity_.address;
    address_->setText(address.isEmpty() ? QString("—") : address);
    identicon_->setPixmap(address.isEmpty() ? QPixmap() : identiconPixmap(address, 32));
    copy_->setEnabled(!address.isEmpty());
    shorter_->setEnabled(!address.isEmpty());
    rebuild_->setText(tr("Key pair %1 of this phrase").arg(pair));
    rebuild_->setVisible(pair >= 0);
    if (!nicknameEdited_ && !address.isEmpty())
        nickname_->setText(nicknameFor(address));
    updateFile();
}
void NewVaultDialog::newRandomKeys() {
    stopSearch();
    setIdentity(Protocol::identity({}), -1);
}
void NewVaultDialog::phraseChanged() {
    stopSearch();
    const auto phrase = phrase_->text();
    const auto pair = phrase.isEmpty() ? std::nullopt : firstPair(phrase, 1);
    if (pair)
        setIdentity(phraseIdentity(phrase, {}, *pair), qint64(*pair));
    else
        setIdentity(Identity(), -1);
}
void NewVaultDialog::toggleSearch() {
    if (search_.running()) {
        stopSearch();
        return;
    }
    const int from = zeroBytes(identity_.address) + 1;
    if (fromPhrase_->isChecked())
        search_.start(phrase_->text(), from);
    else
        search_.start(from);
    shorter_->setText(tr("Stop"));
}
void NewVaultDialog::stopSearch() {
    if (!search_.running())
        return;
    search_.stop();
    shorter_->setText(tr("Make it shorter"));
    searchStatus_->hide();
    // A find announced but not yet taken.
    if (auto find = search_.take(); !find.identity.address.isEmpty())
        setIdentity(std::move(find.identity), find.pair);
}
void NewVaultDialog::updateFile() {
    if (!fileEdited_) {
        const auto slug = slugFor(nickname_->text());
        auto path = QDir(folder_).filePath(slug + ".bmvault");
        for (int n = 2; QFileInfo::exists(path); ++n)
            path = QDir(folder_).filePath(QString("%1-%2.bmvault").arg(slug).arg(n));
        file_->setText(path);
    }
    validate();
}
void NewVaultDialog::validate() {
    const auto path = file_->text().trimmed();
    const bool exists = !path.isEmpty() && QFileInfo::exists(path);
    fileProblem_->setVisible(exists);
    passwordProblem_->setVisible(!repeat_->text().isEmpty() && password_->text() != repeat_->text());
    create_->setEnabled(!identity_.address.isEmpty() && !path.isEmpty() && !exists &&
                        !password_->text().isEmpty() && password_->text() == repeat_->text());
}
void NewVaultDialog::create() {
    stopSearch();
    const auto nickname = nickname_->text().trimmed();
    identity_.label = nickname.isEmpty() ? identity_.address : nickname;
    if (session_.createVault(file_->text().trimmed(), password_->text(), repeat_->text(),
                             identity_)) {
        accept();
        return;
    }
    error_->setText(session_.error());
    error_->show();
    validate();
}
void NewVaultDialog::done(int result) {
    search_.stop();
    {
        const QSignalBlocker block(phrase_);
        phrase_->clear();
    }
    password_->clear();
    repeat_->clear();
    QDialog::done(result);
}
} // namespace bm
```

- [ ] **Step 5: Run it to see it pass**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 && (cd build && QT_QPA_PLATFORM=offscreen ./desktop_tests 2>&1 | grep -E '^FAIL|^PASS')`
Expected: `PASS Widgets mailbox selection, rendering and lock`

- [ ] **Step 6: Translate the dialog**

Run `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --target update_translations`, then fill every `bm::NewVaultDialog` message (drop `type="unfinished"`):

| Source | ru | uk | ja | ko | zh_CN | zh_TW |
|---|---|---|---|---|---|---|
| New vault | Новое хранилище | Нове сховище | 新しい保管庫 | 새 보관함 | 新建保险库 | 新建保險庫 |
| Nickname | Псевдоним | Псевдонім | ニックネーム | 별명 | 昵称 | 暱稱 |
| Copy address | Копировать адрес | Копіювати адресу | アドレスをコピー | 주소 복사 | 复制地址 | 複製地址 |
| Address | Адрес | Адреса | アドレス | 주소 | 地址 | 地址 |
| Random, from the system's entropy | Случайные, из энтропии системы | Випадкові, з ентропії системи | ランダム（システムのエントロピーから） | 무작위, 시스템 엔트로피에서 | 随机，来自系统熵 | 隨機，來自系統熵 |
| Another | Другие | Інші | 別の鍵 | 다른 키 | 换一个 | 換一個 |
| From a secret phrase | Из секретной фразы | З секретної фрази | 秘密のフレーズから | 비밀 문구에서 | 来自秘密短语 | 來自秘密短語 |
| Secret phrase | Секретная фраза | Секретна фраза | 秘密のフレーズ | 비밀 문구 | 秘密短语 | 秘密短語 |
| Show | Показать | Показати | 表示 | 보기 | 显示 | 顯示 |
| Anyone who knows or guesses this phrase has your keys. Use a long random phrase. | Любой, кто знает или угадает эту фразу, получит ваши ключи. Используйте длинную случайную фразу. | Будь-хто, хто знає або вгадає цю фразу, отримає ваші ключі. Використовуйте довгу випадкову фразу. | このフレーズを知っている、または推測できる人は誰でもあなたの鍵を手に入れます。長いランダムなフレーズを使ってください。 | 이 문구를 알거나 추측하는 사람은 누구나 키를 갖게 됩니다. 길고 무작위한 문구를 사용하세요. | 任何知道或猜到这个短语的人都能拿到你的密钥。请使用一个长的随机短语。 | 任何知道或猜到這個短語的人都能取得你的金鑰。請使用一個長的隨機短語。 |
| Keys | Ключи | Ключі | 鍵 | 키 | 密钥 | 金鑰 |
| Make it shorter | Сделать короче | Скоротити | 短くする | 더 짧게 | 缩短 | 縮短 |
| Shorter | Короче | Коротша | 短縮 | 짧게 | 更短 | 更短 |
| Choose where to save the vault | Выбрать, где сохранить хранилище | Вибрати, де зберегти сховище | 保管庫の保存先を選択 | 보관함을 저장할 위치 선택 | 选择保险库的保存位置 | 選擇保險庫的儲存位置 |
| This file exists; existing vaults are never overwritten. | Такой файл уже есть; существующие хранилища никогда не перезаписываются. | Такий файл уже є; наявні сховища ніколи не перезаписуються. | このファイルは存在します。既存の保管庫が上書きされることはありません。 | 이 파일이 이미 있습니다. 기존 보관함은 절대 덮어쓰지 않습니다. | 该文件已存在；现有的保险库绝不会被覆盖。 | 此檔案已存在；現有的保險庫絕不會被覆寫。 |
| Vault file | Файл хранилища | Файл сховища | 保管庫ファイル | 보관함 파일 | 保险库文件 | 保險庫檔案 |
| The passwords do not match. | Пароли не совпадают. | Паролі не збігаються. | パスワードが一致しません。 | 암호가 일치하지 않습니다. | 两次输入的密码不一致。 | 兩次輸入的密碼不一致。 |
| Password | Пароль | Пароль | パスワード | 암호 | 密码 | 密碼 |
| Repeat password | Повторите пароль | Повторіть пароль | パスワードを再入力 | 암호 다시 입력 | 再次输入密码 | 再次輸入密碼 |
| Cancel | Отмена | Скасувати | キャンセル | 취소 | 取消 | 取消 |
| Create vault | Создать хранилище | Створити сховище | 保管庫を作成 | 보관함 만들기 | 新建保险库 | 新建保險庫 |
| Save the vault as | Сохранить хранилище как | Зберегти сховище як | 保管庫を保存 | 보관함을 다른 이름으로 저장 | 将保险库另存为 | 將保險庫另存為 |
| Bitmessage vault (*.bmvault) | Хранилище Bitmessage (*.bmvault) | Сховище Bitmessage (*.bmvault) | Bitmessage 保管庫 (*.bmvault) | Bitmessage 보관함 (*.bmvault) | Bitmessage 保险库 (*.bmvault) | Bitmessage 保險庫 (*.bmvault) |
| Searching %1 key pairs a second; the next shorter address takes about %2. | Перебор %1 пар ключей в секунду; следующий, более короткий адрес — примерно через %2. | Перебір %1 пар ключів за секунду; наступна, коротша адреса — приблизно через %2. | 毎秒 %1 組の鍵を探索中。次に短いアドレスまで約 %2。 | 초당 %1개의 키 쌍을 검색 중입니다. 다음으로 짧은 주소까지 약 %2. | 每秒搜索 %1 对密钥；下一个更短的地址大约需要 %2。 | 每秒搜尋 %1 對金鑰；下一個更短的地址大約需要 %2。 |
| Key pair %1 of this phrase | Пара ключей %1 этой фразы | Пара ключів %1 цієї фрази | このフレーズの鍵ペア %1 | 이 문구의 키 쌍 %1 | 该短语的第 %1 对密钥 | 此短語的第 %1 對金鑰 |
| Stop | Стоп | Стоп | 停止 | 중지 | 停止 | 停止 |
| %1 s | %1 с | %1 с | %1 秒 | %1초 | %1 秒 | %1 秒 |
| %1 min | %1 мин | %1 хв | %1 分 | %1분 | %1 分钟 | %1 分鐘 |
| %1 h | %1 ч | %1 год | %1 時間 | %1시간 | %1 小时 | %1 小時 |
| %1 days | %1 дн. | %1 дн. | %1 日 | %1일 | %1 天 | %1 天 |

Run: `python3 scripts/check-translations.py`
Expected: `PASS: 6 translations complete`

- [ ] **Step 7: Commit (when the user asks)**

```bash
git add src/new_vault_dialog.h src/new_vault_dialog.cpp CMakeLists.txt scripts/check-translations.py tests/widgets_tests.cpp translations
git commit -m "feat(vault): the new vault dialog: an address, its nickname, its file"
```

---

### Task 5: Open the dialog; retire the inline create mode

**Files:**
- Modify: `src/desktop_window.h`, `src/desktop_window.cpp` (`newVault()`, menu, lock screen, create mode removed), `src/session.h`, `src/session.cpp` (`createVault()` without arguments and `beginVaultCreate` removed; `vaultPasswordRequired(QString path)`; `submitVaultPassword(QString password)`), `tools/publish_broadcast.cpp`, `translations/ynotbit_*.ts`
- Test: `tests/widgets_tests.cpp`

**Interfaces:**
- Consumes: `bm::NewVaultDialog(Session &, const QString &folder, bool dark, QWidget *)` (Task 4).
- Produces: `void bm::DesktopWindow::newVault()`; the File menu's action object name `createVaultAction`; `void bm::Session::submitVaultPassword(QString password)`; `void bm::Session::vaultPasswordRequired(QString path)`.

- [ ] **Step 1: Write the failing test**

In `tests/widgets_tests.cpp`, in the lock-screen block, after `"locked, the rail's folders and Write are off");`:

```cpp
            require(!window.findChild<QLineEdit *>("vaultRepeatField"),
                    "the lock screen only unlocks: vaults are made in their own dialog");
            for (const auto &open :
                 {std::function<void()>([&] {
                      window.findChild<QPushButton *>("createVaultButton")->click();
                  }),
                  std::function<void()>(
                      [&] { window.findChild<QAction *>("createVaultAction")->trigger(); })}) {
                bool opened = false;
                QTimer::singleShot(30, &window, [&] {
                    if (auto d = window.findChild<QDialog *>("newVaultDialog")) {
                        opened = d->isVisible();
                        d->reject();
                    }
                });
                open();
                require(opened, "Create a new vault… and File > Create vault… open the new vault "
                                "dialog");
            }
```

- [ ] **Step 2: Run it to see it fail**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 --target desktop_tests && (cd build && QT_QPA_PLATFORM=offscreen ./desktop_tests 2>&1 | grep -E '^FAIL|^PASS|HUNG')`
Expected: `FAIL: the lock screen only unlocks: vaults are made in their own dialog`

- [ ] **Step 3: Open the dialog, retire the old paths**

`src/desktop_window.h`: remove `QLineEdit *vaultPasswordField_, *vaultRepeatField_;` → `QLineEdit *vaultPasswordField_;`; remove `bool vaultCreateMode_ = false;`; change `void showVaultPasswordFor(QString path, bool create);` to `void showVaultPasswordFor(QString path);`; add `void newVault(); // the new vault dialog`.

`src/desktop_window.cpp`:
- add `#include "new_vault_dialog.h"` and `#include <QStandardPaths>`;
- delete the five `vaultRepeatField_ = new QLineEdit; … authLayout->addWidget(vaultRepeatField_);` lines;
- the lock screen's create button: `button(tr("Create a new vault…"), lockedCardLayout, [this] { newVault(); })`;
- the unlock button's handler body becomes:
  ```cpp
        session_.choosePendingVault(targetVaultPath_);
        session_.submitVaultPassword(vaultPasswordField_->text());
        vaultPasswordField_->clear();
  ```
- the menu: `file->addAction(tr("Create vault…"), this, &DesktopWindow::newVault)->setObjectName("createVaultAction");`;
- in the `vaultPasswordAccepted` handler, delete `vaultRepeatField_->clear();`;
- `showVaultPasswordFor(QString path)`: delete `vaultCreateMode_ = create;`;
- `updateLockedScreen()`: delete the `vaultRepeatField_->setVisible(…)` line and make the button line `vaultUnlockButton_->setText(tr("Unlock vault"));`;
- in `refreshRecentVaults()`: `showVaultPasswordFor(path);`;
- add:
  ```cpp
  void DesktopWindow::newVault() {
      const auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
      NewVaultDialog dialog(session_, documents.isEmpty() ? QDir::homePath() : documents,
                            appearance_.dark(), this);
      dialog.exec();
  }
  ```

`src/session.h`: delete `Q_INVOKABLE void createVault();` and `Q_INVOKABLE void beginVaultCreate();`; change `Q_INVOKABLE void submitVaultPassword(QString password, QString repeat, bool create);` to `Q_INVOKABLE void submitVaultPassword(QString password);` and the signal `void vaultPasswordRequired(QString path, bool create);` to `void vaultPasswordRequired(QString path);`.

`src/session.cpp`: delete `Session::createVault()` (no arguments) and `Session::beginVaultCreate()`; in `beginVaultOpen` and `beginVaultUnlock`, `emit vaultPasswordRequired(p);` / `emit vaultPasswordRequired(vaultPath_);`; `submitVaultPassword(QString passphrase)` keeps only the unlock path:

```cpp
void Session::submitVaultPassword(QString passphrase) {
    attempt([&] {
        check(!pendingVaultPath_.isEmpty(), tr("Choose a vault first"));
        check(!passphrase.isEmpty(), tr("Password cannot be empty"));
        const auto path = pendingVaultPath_;
        Password secret{passphrase.toUtf8()};
        auto &bytes = secret.bytes;
        passphrase.fill(QChar(0));
        acquireVault(path);
        try {
            vault_.unlock(path, bytes);
            if (vaultPath_ != path) {
                mailPath_.clear();
                mailKey_.clear();
            }
            vaultPath_ = path;
            rememberVault(path);
            activity_ = tr("Vault unlocked. Open a mailbox to inspect cached objects.");
        } catch (...) {
            vaultLock_.reset();
            sodium_memzero(bytes.data(), bytes.size());
            throw;
        }
        sodium_memzero(bytes.data(), bytes.size());
        pendingVaultPath_.clear();
        emit vaultPasswordAccepted();
        if (!mailPath_.isEmpty())
            openMailboxPath(mailPath_);
        emit changed();
    });
}
```

`tools/publish_broadcast.cpp`: `session.submitVaultPassword(QString::fromLatin1(password));`.

- [ ] **Step 4: Run it to see it pass**

Run: `PKG_CONFIG_PATH=/Users/user/w/ynotbit-deps/installed/lib/pkgconfig cmake --build build --parallel 8 && cmake --build build --target publish_broadcast 2>&1 | tail -1 && (cd build && QT_QPA_PLATFORM=offscreen ./desktop_tests 2>&1 | grep -E '^FAIL|^PASS|HUNG')`
Expected: `PASS Widgets mailbox selection, rendering and lock` (and publish_broadcast builds)

- [ ] **Step 5: Translations, format, full suite**

Run `cmake --build build --target update_translations` (with the PKG_CONFIG_PATH prefix); the strings removed with the create mode ("Repeat passphrase", "Vault created. Add an identity and create a mailbox.", the old lock-screen "Create vault") drop out; nothing new needs translating. Then `python3 scripts/check-translations.py` → `PASS: 6 translations complete`. Format the new files whole and changed lines elsewhere (`--lines`), rebuild, and run the suite.
Expected: `100% tests passed, 0 tests failed out of 22`

- [ ] **Step 6: Commit (when the user asks)**

```bash
git add src/desktop_window.h src/desktop_window.cpp src/session.h src/session.cpp tools/publish_broadcast.cpp tests/widgets_tests.cpp translations docs/superpowers/specs/2026-10-10-new-vault-dialog-design.md docs/superpowers/plans/2026-10-10-new-vault-dialog.md
git commit -m "feat(vault): Create vault opens the new vault dialog"
```
