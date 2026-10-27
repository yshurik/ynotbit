#include <bmc/chat_format.h>
#include <bmc/client.h>
#include <bmc/error.h>
#include <bmc/identity.h>
#include <bmc/text.h>
#include <bmc/wire.h>
#include <set>

namespace bmc {
Client::Client(std::shared_ptr<Vault> vault, std::shared_ptr<Mailbox> mailbox, std::string root)
    : vault_(std::move(vault)), mailbox_(std::move(mailbox)), cache_(std::make_shared<Cache>(root)),
      delivery_(std::make_shared<Delivery>(std::move(root))) {}

void Client::bindMailboxToCache() {
    boundCacheId_ = cache_->id();
    mailbox_->bindCache(boundCacheId_);
}

StepReport Client::step(int64_t now, bool online) {
    StepReport report;
    if (!mailbox_->isOpen())
        return report;
    report.open = true;
    // A new id means the node created or emptied its store: read it from the start.
    if (cache_->id() != boundCacheId_)
        bindMailboxToCache();
    if (cache_->firstSequence() > mailbox_->checkpoint() + 1)
        prunedUnread_ = true;
    // A time budget, not a count: catching up (a new identity, first sync) goes as fast as the
    // window can spare, ~150 ms per tick.
    delivery_->scan(*cache_, *mailbox_, *vault_, now, 512, 150);
    delivery_->tick(*mailbox_, *vault_, online, now);
    report.checkpoint = mailbox_->checkpoint();
    report.upToDate = !cache_->hasAfter(report.checkpoint);
    report.working = delivery_->working();
    report.pruned = prunedUnread_;
    return report;
}

namespace {
// The checks of a chat and its queueing; a chan may send only if the caller says so.
std::string queueChatChecked(Mailbox &mailbox, const Vault &vault, const std::string &from,
                             const std::string &peer, const std::string &body, int64_t expires,
                             bool chanMaySend) {
    require(mailbox.isOpen(), "Open a mailbox first");
    bool own = false;
    for (const auto &identity : vault.identities())
        if ((chanMaySend || !identity.chan) && identity.address == from)
            own = true;
    require(own, "Choose a sender from this vault");
    require(Wire::validAddress(peer) && peer != from, "Enter a valid Bitmessage recipient address");
    require(!text::trim(body).empty() && body.size() <= size_t(kMaxChatBodyBytes),
            "Chat message must be between 1 and 8192 bytes");
    const auto id = mailbox.queueChat(from, peer, body, expires);
    Delivery::rereadKept(mailbox); // the peer's key may already be kept
    return id;
}
} // namespace

std::string Client::sendChat(const std::string &from, const std::string &peer,
                             const std::string &body, int64_t now) {
    const auto id =
        queueChatChecked(*mailbox_, *vault_, from, peer, body, now + kChatTtlSeconds, false);
    watchPerson(peer);
    return id;
}

std::string Client::sendServiceChat(const std::string &from, const std::string &peer,
                                    const std::string &body, int64_t ttlSeconds, int64_t now) {
    // The life is checked first, with the mailbox's own words for it, so that a caller cannot get a
    // chat that outlives a day by way of this door.
    require(ttlSeconds > 0 && ttlSeconds <= kChatTtlSeconds, "Invalid chat message or expiry");
    return queueChatChecked(*mailbox_, *vault_, from, peer, body, now + ttlSeconds, true);
}

std::string Client::chatState(const std::string &id) const {
    if (!mailbox_->isOpen() || id.empty())
        return {};
    try {
        return mailbox_->chatState(id);
    } catch (const std::exception &) {
        return {};
    }
}

std::vector<ChatMessage> Client::unreadChats(int limit) const {
    if (!mailbox_->isOpen())
        return {};
    try {
        return mailbox_->unreadChats(limit);
    } catch (const std::exception &) {
        return {};
    }
}

bool Client::markChatMessageRead(const std::string &id) {
    if (!mailbox_->isOpen() || id.empty())
        return false;
    try {
        return mailbox_->markChatMessageRead(id);
    } catch (const std::exception &) {
        return false;
    }
}

void Client::sendLetter(const std::string &id, int64_t now) {
    require(mailbox_->isOpen(), "Open a mailbox first");
    const auto m = mailbox_->message(id);
    bool own = false;
    for (const auto &i : vault_->identities())
        if (i.address == m.from)
            own = true;
    require(own, "Choose a sender from this vault");
    const auto kind = mailbox_->setting("draftkind:" + id, "direct");
    require(kind == "broadcast" || Wire::validAddress(m.to),
            "Enter a valid Bitmessage recipient address");
    require(m.subject.find_first_of("\r\n") == std::string::npos, "Subject must be one line");
    require(!text::trim(m.body).empty(), "Write a message before sending");
    // The caller has its own, friendlier wording for an oversized letter; this is the backstop.
    require(letterTextBytes(m.subject, m.body) <= kMaxLetterText, "Letter is too large");
    mailbox_->queueDraft(id, kind, now + 4 * 86400);
    if (kind != "broadcast")
        watchPerson(m.to);
    Delivery::rereadKept(*mailbox_); // the recipient's key may already be kept
}

bool Client::watchPerson(const std::string &address) {
    try {
        return mailbox_->isOpen() && Delivery::watchPerson(*mailbox_, *vault_, address);
    } catch (const std::exception &) {
        return false;
    }
}

int Client::watchKnownPeople() {
    require(mailbox_->isOpen(), "Open a mailbox first");
    std::set<std::string> people;
    for (const auto &c : mailbox_->contacts())
        people.insert(c.address);
    for (const char *folder : {"Inbox", "Archive"})
        for (auto &address : mailbox_->senders(folder))
            people.insert(std::move(address));
    for (auto &address : mailbox_->recipients("Sent"))
        people.insert(std::move(address));
    for (auto &address : mailbox_->chatPeers())
        people.insert(std::move(address));
    int made = 0;
    Mailbox::Batch batch(*mailbox_); // one commit, however many are made
    for (const auto &address : people)
        if (Delivery::watchPerson(*mailbox_, *vault_, address))
            ++made;
    batch.commit();
    return made;
}
} // namespace bmc
