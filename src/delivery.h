#pragma once
#include "cache.h"
#include "pow.h"
#include "storage.h"
#include <bmc/delivery.h>
#include <memory>
namespace bm {
// The delivery engine bmc::Delivery runs, with the Qt types in front: the proof of work, the
// outbox states and the receipts are the core's, the time comes from bmc::now() here.
class Delivery {
    std::shared_ptr<bmc::Delivery> core_;

  public:
    explicit Delivery(QString root);
    ~Delivery() = default;
    Delivery(const Delivery &) = delete;
    Delivery &operator=(const Delivery &) = delete;
    bmc::Delivery &core() const {
        return *core_;
    }
    const std::shared_ptr<bmc::Delivery> &corePtr() const {
        return core_;
    }
    void stop();
    void cancel(Mailbox &, const QString &);
    void retry(Mailbox &, const QString &);
    // Up to limit objects; with a budget, stops once that many milliseconds
    // have gone (after at least one object).
    int scan(Cache &, Mailbox &, const Vault &, int limit = 8, int budgetMs = 0);
    // Reads the kept broadcasts and public keys again (cheap: no message is
    // tried against the identities), e.g. for a recipient's key already here.
    static void rereadKept(Mailbox &);
    void tick(Mailbox &, const Vault &, bool online);
    bool working() const;
};
} // namespace bm
