// The delivery engine: what the outbox still needs, one mined object at a time, the objects
// offered to the relay and the receipts read back, and the incoming scan. The rules, the state
// names and the receipts are src/delivery.cpp's, character for character.
#pragma once
#include <bmc/pow.h>
#include <cstdint>
#include <string>

namespace bmc {
class Cache;
class Mailbox;
class Vault;

class Delivery {
    std::string root_, mining_;
    ProofOfWork pow_;
    void plan(Mailbox &, const Vault &, int64_t time);
    void processJobs(Mailbox &, bool online, int64_t time);
    int catchUpBroadcasts(Cache &, Mailbox &, const std::string &publisher, int64_t time);

  public:
    explicit Delivery(std::string root) : root_(std::move(root)) {}
    void stop() {
        pow_.stop();
        mining_.clear();
    }
    void cancel(Mailbox &, const std::string &id);
    void retry(Mailbox &, const std::string &id);
    // Up to limit objects; with a budget, stops once that many milliseconds have gone (after at
    // least one object).
    int scan(Cache &, Mailbox &, const Vault &, int64_t now, int limit = 8, int budgetMs = 0);
    // Reads the kept broadcasts and public keys again (cheap: no message is tried against the
    // identities), e.g. for a recipient's key already here.
    static void rereadKept(Mailbox &);
    void tick(Mailbox &, const Vault &, bool online, int64_t now);
    bool working() const {
        return !mining_.empty();
    }
};
} // namespace bmc
