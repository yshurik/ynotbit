#pragma once
#include <QByteArray>
#include <atomic>
#include <thread>
namespace bm {
class ProofOfWork {
    std::atomic_bool cancel_{false}, done_{false};
    std::thread worker_;
    QByteArray result_;

  public:
    ~ProofOfWork() {
        stop();
    }
    void start(QByteArray object, quint64 trials = 1000, quint64 extra = 1000);
    void stop();
    bool done() const {
        return done_;
    }
    QByteArray take();
    static bool valid(const QByteArray &, qint64 now, quint64 trials = 1000, quint64 extra = 1000);
};
} // namespace bm
