#pragma once
#include <QByteArray>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
namespace bm {
class ProofOfWork {
    std::atomic_bool cancel_{false}, done_{false};
    std::vector<std::thread> workers_;
    std::mutex resultMutex_;
    QByteArray result_;
    void join();

  public:
    ~ProofOfWork() {
        stop();
    }
    // Searches on workerCount() threads; done() turns true when one finds a nonce.
    void start(QByteArray object, quint64 trials = 1000, quint64 extra = 1000);
    void stop();
    bool done() const {
        return done_;
    }
    QByteArray take();
    // Every core but one, so the window stays responsive while a letter is prepared.
    static unsigned workerCount();
    static bool valid(const QByteArray &, qint64 now, quint64 trials = 1000, quint64 extra = 1000);
};
} // namespace bm
