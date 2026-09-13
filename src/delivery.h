#pragma once
#include "cache.h"
#include "pow.h"
#include "storage.h"
namespace bm {
class Delivery {
    QString root_, mining_;
    ProofOfWork pow_;
    void plan(Mailbox &, const Vault &, qint64);
    void processJobs(Mailbox &, bool);

  public:
    explicit Delivery(QString root) : root_(std::move(root)) {}
    void stop() {
        pow_.stop();
        mining_.clear();
    }
    void cancel(Mailbox &, const QString &);
    void retry(Mailbox &, const QString &);
    int scan(Cache &, Mailbox &, const Vault &, int limit = 8);
    void tick(Mailbox &, const Vault &, bool online);
    bool working() const {
        return !mining_.isEmpty();
    }
};
} // namespace bm
