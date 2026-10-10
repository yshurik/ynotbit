#pragma once
// Keys for a new identity: from a secret phrase, as PyBitmessage derives a
// deterministic address, and the search for shorter addresses (more zero bytes
// at the start of the RIPE hash), as tools/bm-shortaddr does.
#include "storage.h"
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <memory>
#include <optional>
class QTimer;
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
} // namespace bm
