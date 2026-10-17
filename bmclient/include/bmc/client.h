// The client loop: a vault, a mailbox, the node's object store and the delivery engine, with the
// rules the desktop Session had, in std. The Qt veneer (src/session.cpp) keeps the QTimer, the
// QProcess and the signals and calls this.
#pragma once
#include <bmc/cache.h>
#include <bmc/delivery.h>
#include <bmc/mailbox.h>
#include <bmc/records.h>
#include <bmc/vault.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bmc {
// What one step did, for the caller's status line.
struct StepReport {
    bool open = false;     // the mailbox is open
    bool upToDate = false; // no cached object is after the checkpoint
    bool working = false;  // proof of work is being prepared
    bool pruned = false;   // retention removed objects this mailbox never read
    int64_t checkpoint = 0;
};

class Client {
    std::shared_ptr<Vault> vault_;
    std::shared_ptr<Mailbox> mailbox_;
    std::shared_ptr<Cache> cache_;
    std::shared_ptr<Delivery> delivery_;
    std::string boundCacheId_;
    bool prunedUnread_ = false;

  public:
    Client(std::shared_ptr<Vault> vault, std::shared_ptr<Mailbox> mailbox, std::string root);
    Vault &vault() const {
        return *vault_;
    }
    Mailbox &mailbox() const {
        return *mailbox_;
    }
    Cache &cache() const {
        return *cache_;
    }
    Delivery &delivery() const {
        return *delivery_;
    }
    const std::shared_ptr<Vault> &vaultPtr() const {
        return vault_;
    }
    const std::shared_ptr<Mailbox> &mailboxPtr() const {
        return mailbox_;
    }
    // The mailbox records the cache it is bound to; a new id (the node recreated its store) binds
    // it again, so the scan starts over.
    void bindMailboxToCache();
    // One turn: bind, note pruned objects, scan with a time budget (512 objects, 150 ms), tick the
    // delivery engine. online false: nothing is offered to the relay.
    StepReport step(int64_t now, bool online);
    // The chat and letter services, with the rules Session had. Each throws bmc::Error with the
    // English text the catalogue lists; the caller translates. sendChat returns the new id.
    std::string sendChat(const std::string &from, const std::string &peer, const std::string &body,
                         int64_t now);
    std::string chatState(const std::string &id) const;
    std::vector<ChatMessage> unreadChats(int limit) const;
    bool markChatMessageRead(const std::string &id);
    // Queues a draft as an outgoing letter; the recipient's key may already be kept, so the kept
    // broadcasts and keys are read again.
    void sendLetter(const std::string &id, int64_t now);
};
} // namespace bmc
