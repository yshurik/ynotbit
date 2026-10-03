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
    int catchUpBroadcasts(Cache &, Mailbox &, const QString &publisher);

  public:
    explicit Delivery(QString root) : root_(std::move(root)) {}
    void stop() {
        pow_.stop();
        mining_.clear();
    }
    void cancel(Mailbox &, const QString &);
    void retry(Mailbox &, const QString &);
    // Up to limit objects; with a budget, stops once that many milliseconds
    // have gone (after at least one object).
    int scan(Cache &, Mailbox &, const Vault &, int limit = 8, int budgetMs = 0);
    // Reads the kept broadcasts and public keys again (cheap: no message is
    // tried against the identities), e.g. for a recipient's key already here.
    static void rereadKept(Mailbox &);
    void tick(Mailbox &, const Vault &, bool online);
    bool working() const {
        return !mining_.isEmpty();
    }
};
} // namespace bm
