#include <algorithm>
#include <bmc/cache.h>
#include <bmc/clock.h>
#include <bmc/delivery.h>
#include <bmc/error.h>
#include <bmc/hash.h>
#include <bmc/json.h>
#include <bmc/mailbox.h>
#include <bmc/path.h>
#include <bmc/protocol.h>
#include <bmc/updates.h>
#include <bmc/vault.h>
#include <bmc/wire.h>
#include <chrono>
#include <filesystem>
#include <ntb-object-db.h>
#include <optional>
#include <sodium.h>
#include <unordered_set>

namespace bmc {
namespace {
bool terminal(const std::string &s) {
    return s == "acknowledged" || s == "published" || s == "cancelled" || s == "failed" ||
           s == "expired";
}
// The vault keeps its identities, so a view of one is enough and nothing is copied.
std::optional<IdentityRef> identity(const Vault &vault, const std::string &address) {
    for (const auto &i : vault.identities())
        if (i.address == address)
            return IdentityRef(i);
    return {};
}
std::string addJob(Mailbox &m, std::string owner, std::string kind, std::string address, Bytes data,
                   uint64_t trials = 1000, uint64_t extra = 1000, std::string state = "queued") {
    for (const auto &j : m.jobs())
        if (j.owner == owner && j.kind == kind && j.address == address)
            return j.id;
    NetworkJob j;
    j.owner = std::move(owner);
    j.kind = std::move(kind);
    j.address = std::move(address);
    j.payload = std::move(data);
    j.state = std::move(state);
    auto h = Wire::header(j.payload);
    if (!h)
        require(false, "Invalid outgoing object");
    j.expires = h->expires;
    j.nonceTrials = std::max<uint64_t>(1000, trials);
    j.extraBytes = std::max<uint64_t>(1000, extra);
    if (j.state == "ready")
        j.hash = Protocol::inventoryHash(j.payload);
    return m.addJob(std::move(j));
}
void saveKey(Mailbox &m, const PublicIdentity &key, int64_t expires) {
    // Ignore extravagant work requests; they must not stall scanning incoming mail.
    if (key.nonceTrials > 1000000 || key.extraBytes > 1000000)
        return;
    m.savePublicKey({key.address, key.signingKey, key.encryptionKey,
                     std::max<uint64_t>(1000, key.nonceTrials),
                     std::max<uint64_t>(1000, key.extraBytes), key.behaviors, expires});
}
// QString::toLongLong(): a number, or zero when the text is not one.
int64_t toNumber(const std::string &text) {
    try {
        size_t used = 0;
        const auto value = std::stoll(text, &used);
        return used == text.size() ? value : 0;
    } catch (const std::exception &) {
        return 0;
    }
}
std::string joined(const std::vector<std::string> &parts, char separator) {
    std::string out;
    for (const auto &part : parts) {
        if (!out.empty())
            out.push_back(separator);
        out += part;
    }
    return out;
}
// The identities and the subscriptions as one comparable fingerprint.
std::string fingerprint(std::vector<std::string> entries) {
    // Every entry is ASCII, so ordering by byte is ordering by QString's code unit.
    std::sort(entries.begin(), entries.end());
    return hex(hash(Digest::Sha256, view(joined(entries, '\n'))));
}
int64_t elapsedMs(std::chrono::steady_clock::time_point began) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 began)
        .count();
}
// The receipt the relay leaves; a missing or unreadable one reads as no document, as the Qt
// code's failed open did.
std::optional<json::Value> readReceipt(const std::string &file) {
    auto *f = openFile(path(file), "rb");
    if (!f)
        return {};
    char buffer[4096];
    const auto read = std::fread(buffer, 1, sizeof(buffer), f);
    std::fclose(f);
    return json::parse(std::string_view(buffer, size_t(read)));
}
// Brings the watch to these addresses: the ones of version 4 and up by their tag, the older ones
// apart. Only the addresses that came or went are worked out, as a new correspondent is a change
// in a set that may hold thousands.
void updateWatch(BroadcastWatch &watch, const std::vector<std::string> &addresses,
                 std::string fingerprint) {
    const std::unordered_set<std::string> now(addresses.begin(), addresses.end());
    for (auto it = watch.tagOf.begin(); it != watch.tagOf.end();) {
        if (now.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second && !it->second->empty())
            watch.byTag.erase(*it->second);
        it = watch.tagOf.erase(it);
    }
    for (const auto &address : addresses) {
        if (watch.tagOf.count(address))
            continue;
        const auto tag = Wire::tagOfAddress(address); // none: no address at all, nothing of it
        std::optional<std::string> mark;              // can be a broadcast
        if (tag)
            mark = std::string(tag->begin(), tag->end());
        if (mark && !mark->empty())
            watch.byTag.emplace(*mark, address);
        watch.tagOf.emplace(address, std::move(mark));
    }
    watch.untagged.clear();
    for (const auto &[address, tag] : watch.tagOf)
        if (tag && tag->empty())
            watch.untagged.push_back(address);
    watch.fingerprint = std::move(fingerprint);
}
// A broadcast from a watched address, ynotbit's release address, or (chans, when given) a chan's
// own Anonymous-mode post. A broadcast of version 5 says in the clear whose it is, so only the
// address with its tag is tried; one of version 4 carries no tag, and is tried against the
// addresses old enough to have such broadcasts.
std::optional<DecodedEnvelope> decodeBroadcast(ByteView data, const BroadcastWatch &watch,
                                               const std::string &publisher, const Vault *chansOf,
                                               bool *chanBroadcast) {
    *chanBroadcast = false;
    if (const auto tag = Wire::tagOfBroadcast(data); !tag.empty()) {
        if (const auto it = watch.byTag.find(std::string(tag.begin(), tag.end()));
            it != watch.byTag.end())
            if (auto decoded = Wire::decodeBroadcast(data, it->second))
                return decoded;
    } else {
        for (const auto &address : watch.untagged)
            if (auto decoded = Wire::decodeBroadcast(data, address))
                return decoded;
    }
    // ynotbit's own release announcements, heard without subscribing.
    if (!publisher.empty())
        if (auto decoded = Wire::decodeBroadcast(data, publisher))
            return decoded;
    // Joining a chan already means "I care about this address's traffic" --
    // members shouldn't also have to manually subscribe to hear a chan's own
    // "Anonymous"/broadcast-mode posts.
    if (chansOf)
        for (const auto &i : chansOf->identities())
            if (i.chan)
                if (auto decoded = Wire::decodeBroadcast(data, i.address)) {
                    *chanBroadcast = true;
                    return decoded;
                }
    return {};
}
// Records a release announcement's version: the broadcast is signed, so only
// the publisher can announce one.
void noteRelease(Mailbox &m, const DecodedEnvelope &d, const std::string &publisher) {
    if (d.broadcast && !publisher.empty() && d.sender.address == publisher)
        if (auto version = updates::announcedVersion(d.message.subject))
            if (updates::compareVersions(*version, m.setting(updates::kLatestSetting, "0")) > 0)
                m.setSetting(updates::kLatestSetting, *version);
}
const std::string kSubscriptionsSetting = "scan:subscriptions"; // the set last scanned for
const std::string kCatchUpFrom = "scan:catchup-from", kCatchUpTo = "scan:catchup-to";
} // namespace

void Delivery::cancel(Mailbox &m, const std::string &id) {
    stop();
    std::error_code ignored;
    for (const auto &j : m.jobs())
        if (j.owner == id) {
            std::filesystem::remove(path(root_ + "/publish/" + j.id + ".object"), ignored);
            std::filesystem::remove(path(root_ + "/receipts/" + j.id + ".json"), ignored);
        }
    m.cancel(id);
}

void Delivery::retry(Mailbox &m, const std::string &id) {
    stop();
    std::error_code ignored;
    for (const auto &j : m.jobs())
        if (j.owner == id) {
            std::filesystem::remove(path(root_ + "/publish/" + j.id + ".object"), ignored);
            std::filesystem::remove(path(root_ + "/receipts/" + j.id + ".json"), ignored);
        }
    m.retry(id, now() + 4 * 86400);
    m.advance(0);
}

void Delivery::plan(Mailbox &m, const Vault &v, int64_t time) {
    for (auto item : m.outbox()) {
        if (terminal(item.state))
            continue;
        if (item.expires <= time) {
            // Finite retries, then an explicit user-action state; never silently discard a letter.
            if (!item.chat && item.attempts < 3) {
                retry(m, item.id);
                continue;
            }
            m.setDelivery(item.id, "expired",
                          item.chat ? "Chat expired after 24 hours."
                                    : "No delivery receipt before expiry. Retry is available.");
            continue;
        }
        if (item.state == "publishing" || item.state == "awaiting_ack" || item.state == "ready")
            continue;
        const auto sender = identity(v, item.message.from);
        if (!sender) {
            m.setDelivery(item.id, "failed", "Sender identity is missing from this vault");
            continue;
        }
        try {
            if (item.kind == "broadcast") {
                addJob(m, item.id, "broadcast", std::string(sender->address),
                       Wire::encodeBroadcast(*sender, item.message.subject, item.message.body,
                                             item.expires));
                if (item.state != "calculating_message")
                    m.setDelivery(item.id, "calculating_message",
                                  "Calculating broadcast proof of work");
                continue;
            }
            std::optional<PublicIdentity> recipient;
            if (auto local = identity(v, item.message.to))
                recipient = Wire::publicIdentity(*local);
            else if (auto saved = m.publicKey(item.message.to, time))
                recipient = PublicIdentity{saved->address,
                                           saved->signingKey,
                                           saved->encryptionKey,
                                           saved->nonceTrials,
                                           saved->extraBytes,
                                           saved->behaviors,
                                           false};
            if (!recipient) {
                if (item.state != "awaiting_pubkey" || item.nextAttempt <= time) {
                    addJob(m, item.id, "getpubkey", item.message.to,
                           Wire::getPubkey(item.message.to, std::min(item.expires, time + 86400)));
                    m.setDelivery(item.id, "awaiting_pubkey", "Requesting recipient encryption key",
                                  time + 3600);
                }
                continue;
            }
            const bool ack = !recipient->chan && (recipient->behaviors & 1);
            m.recordMilestone(item.id, "key_available",
                              "Recipient encryption key available for this delivery (local, "
                              "cached, or received)");
            if (ack && item.ackObject.empty()) {
                if (item.ackToken.empty()) {
                    item.ackToken.resize(32);
                    randombytes_buf(item.ackToken.data(), 32);
                    m.setAck(item.id, item.ackToken);
                }
                addJob(m, item.id, "ack-template", item.message.to,
                       Wire::acknowledgment(item.ackToken, item.expires));
                if (item.state != "calculating_ack")
                    m.setDelivery(item.id, "calculating_ack",
                                  "Preparing a private delivery receipt");
                continue;
            }
            addJob(m, item.id, "message", item.message.to,
                   Wire::encodeMessage(*sender, *recipient, item.message.subject, item.message.body,
                                       item.expires, ack ? item.ackObject : Bytes()),
                   recipient->nonceTrials, recipient->extraBytes);
            if (item.state != "calculating_message")
                m.setDelivery(item.id, "calculating_message",
                              "Encrypting and calculating message proof of work");
        } catch (const std::exception &e) {
            m.setDelivery(item.id, "failed", e.what());
        }
    }
}

void Delivery::processJobs(Mailbox &m, bool online, int64_t time) {
    std::error_code ignored;
    auto jobs = m.jobs();
    if (!mining_.empty()) {
        auto it =
            std::find_if(jobs.begin(), jobs.end(), [&](const auto &j) { return j.id == mining_; });
        if (it == jobs.end())
            stop();
        else if (pow_.done()) {
            auto solved = pow_.take();
            const auto id = mining_;
            mining_.clear();
            m.updateJob(id, "ready", &solved, Protocol::inventoryHash(solved));
            if (it->kind == "message" || it->kind == "broadcast")
                m.recordMilestone(it->owner, "prepared",
                                  "Encrypted object and proof of work completed");
        }
    }
    for (auto j : m.jobs()) {
        if (j.expires <= time) {
            std::filesystem::remove(path(root_ + "/publish/" + j.id + ".object"), ignored);
            std::filesystem::remove(path(root_ + "/receipts/" + j.id + ".json"), ignored);
            m.removeJob(j.id);
            if (j.id == mining_)
                stop();
            continue;
        }
        if (!j.owner.empty()) {
            const auto state = m.outgoing(j.owner).state;
            if (terminal(state)) {
                std::filesystem::remove(path(root_ + "/publish/" + j.id + ".object"), ignored);
                m.removeJob(j.id);
                if (j.id == mining_)
                    stop();
                continue;
            }
        }
        if (j.state == "queued" || j.state == "mining") {
            if (mining_.empty()) {
                pow_.start(j.payload, j.nonceTrials, j.extraBytes);
                mining_ = j.id;
                m.updateJob(j.id, "mining");
            }
            continue;
        }
        if (j.state == "ready" && j.kind == "ack-template") {
            auto item = m.outgoing(j.owner);
            m.setAck(j.owner, item.ackToken, j.payload);
            m.removeJob(j.id);
            continue;
        }
        if (j.state != "ready" && j.state != "submitted")
            continue;
        if (!online)
            continue;
        auto receipt = root_ + "/receipts/" + j.id + ".json",
             spool = root_ + "/publish/" + j.id + ".object";
        json::Value result;
        if (auto parsed = readReceipt(receipt))
            result = std::move(*parsed);
        auto state = result.at("state").toString();
        if (result.at("hash").toString() != j.hash)
            state.clear();
        if (state == "rejected") {
            if (!j.owner.empty())
                m.setDelivery(j.owner, "failed",
                              "Relay rejected the object; retry to regenerate it");
            m.removeJob(j.id);
            std::filesystem::remove(path(receipt), ignored);
            std::filesystem::remove(path(spool), ignored);
            continue;
        }
        if (state == "offered") {
            if (j.kind == "message" || j.kind == "broadcast") {
                auto item = m.outgoing(j.owner);
                m.setObjectHash(j.owner, j.hash);
                m.setDelivery(
                    j.owner, item.ackObject.empty() ? "published" : "awaiting_ack",
                    item.ackObject.empty()
                        ? "Offered to connected peers. No recipient receipt is available."
                        : "Offered to connected peers; awaiting recipient acknowledgment.");
            }
            if (j.kind == "pubkey")
                m.setSetting("pubkey:" + j.address, std::to_string(j.expires));
            m.removeJob(j.id);
            std::filesystem::remove(path(receipt), ignored);
            std::filesystem::remove(path(spool), ignored);
            continue;
        }
        // Recreate an absent job after a relay crash, but never expose ack templates.
        if (!std::filesystem::exists(path(spool), ignored)) {
            std::filesystem::create_directories(path(root_ + "/publish"), ignored);
            try {
                writeFileAtomic(path(spool), j.payload);
            } catch (const Error &) {
                // require(), not a bare throw: the catalogue lists require() text, and the desktop
                // shows this one in the outbox.
                require(false, "Cannot queue encrypted object for relay", Code::Io);
            }
        }
        if (j.state != "submitted") {
            m.updateJob(j.id, "submitted");
            if (j.kind == "message" || j.kind == "broadcast")
                m.setDelivery(j.owner, "publishing",
                              "Waiting for the relay to offer this object to connected peers");
        }
    }
}

void Delivery::tick(Mailbox &m, const Vault &v, bool online, int64_t time) {
    if (!m.isOpen() || !v.unlocked()) {
        stop();
        return;
    }
    plan(m, v, time);
    processJobs(m, online, time);
    m.endChatDeliveries(time);
}

bool Delivery::watchPerson(Mailbox &m, const Vault &v, const std::string &address) {
    if (!v.unlocked() || !Wire::validAddress(address))
        return false;
    for (const auto &i : v.identities())
        if (i.address == address)
            return false;
    return m.watch(address);
}

void Delivery::rereadKept(Mailbox &m) {
    if (m.checkpoint() <= 0)
        return; // the main scan has everything still ahead of it
    m.setSetting(kCatchUpFrom, "0");
    m.setSetting(kCatchUpTo, std::to_string(m.checkpoint()));
}

// A subscription added (or a letter queued) needs only the kept broadcasts
// (or public keys) read again, not every message tried against every
// identity: this pass reads object headers, decodes those two types alone,
// and never moves the checkpoint.
int Delivery::catchUpBroadcasts(Cache &cache, Mailbox &m, const std::string &publisher,
                                int64_t time) {
    const auto to = toNumber(m.setting(kCatchUpTo));
    auto from = toNumber(m.setting(kCatchUpFrom));
    if (to <= 0 || from >= to)
        return 0;
    int count = 0;
    const auto budget = std::chrono::steady_clock::now();
    while (from < to && elapsedMs(budget) < 50) {
        const auto batch = cache.after(from, 256);
        if (batch.empty()) {
            from = to;
            break;
        }
        for (const auto &o : batch) {
            if (o.sequence > to) {
                from = to;
                break;
            }
            from = o.sequence;
            if (o.payload.size() > NTB_OBJECT_DB_MAX_OBJECT_SIZE)
                continue;
            const auto data = ByteView(o.payload);
            const auto h = Wire::header(data);
            if (h && h->type == 1 && h->expires > time) {
                for (const auto &item : m.outbox())
                    if (!terminal(item.state))
                        if (auto key = Wire::decodePubkey(data, item.message.to))
                            saveKey(m, *key, h->expires);
                continue;
            }
            if (!h || h->type != 3)
                continue;
            bool chanBroadcast = false;
            if (auto decoded = decodeBroadcast(data, watch_, publisher, nullptr, &chanBroadcast)) {
                const auto &d = *decoded;
                saveKey(m, d.sender, time + 28 * 86400);
                noteRelease(m, d, publisher);
                m.reveal(d.sender.address); // a post: a pending subscription is listed now
                m.store(o.hash, d.message.from, d.message.to, d.message.subject, d.message.body, -1,
                        "Broadcasts");
                ++count;
            }
        }
    }
    if (from >= to) {
        m.setSetting(kCatchUpTo, {});
        m.setSetting(kCatchUpFrom, {});
    } else {
        m.setSetting(kCatchUpFrom, std::to_string(from));
    }
    return count;
}

int Delivery::scan(Cache &cache, Mailbox &m, const Vault &v, int64_t time, int limit,
                   int budgetMs) {
    if (!v.unlocked() || !m.isOpen())
        return 0;
    std::vector<std::string> identities, subscriptions;
    for (const auto &i : v.identities())
        identities.push_back(i.address);
    // The subscriptions and the pending ones: all of them are listened for.
    const auto watched = m.watched();
    for (const auto &address : watched)
        subscriptions.push_back("subscription:" + address);
    const auto publisher =
        m.setting(updates::kNotifySetting) == "off" ? std::string() : updates::publisherAddress();
    if (!publisher.empty())
        subscriptions.push_back("updates:" + publisher);
    // Mailboxes scanned before identities and subscriptions were told apart
    // carry one fingerprint of both: still the same set, so no rescan.
    const auto identitiesNow = fingerprint(identities);
    std::vector<std::string> both = identities;
    both.insert(both.end(), subscriptions.begin(), subscriptions.end());
    if (m.boundIdentities() == fingerprint(both))
        m.bindIdentities(identitiesNow, true);
    const bool identitiesChanged = m.boundIdentities() != identitiesNow;
    m.bindIdentities(identitiesNow); // a different set starts over from the first object
    const auto subscriptionsNow = fingerprint(subscriptions);
    if (watch_.fingerprint != subscriptionsNow)
        updateWatch(watch_, watched, subscriptionsNow);
    if (m.setting(kSubscriptionsSetting) != subscriptionsNow) {
        // A full rescan reads everything anyway; otherwise only broadcasts
        // already passed need reading again.
        if (!identitiesChanged)
            rereadKept(m);
        m.setSetting(kSubscriptionsSetting, subscriptionsNow);
    }
    if (identitiesChanged) {
        m.setSetting(kCatchUpTo, {});
        m.setSetting(kCatchUpFrom, {});
    }
    // One commit per call: the letters stored and the checkpoint reached
    // land together, and a failure re-inspects the batch (harmlessly).
    Mailbox::Batch batch(m);
    int count = 0;
    // Objects that are not for us only move the checkpoint, and that is written
    // once per batch, not per object: each write is a synchronous commit, and
    // nine in ten objects are someone else's. A crash re-inspects at most one
    // batch, which is harmless (storing a letter twice is ignored).
    int64_t passed = 0;
    const auto spent = std::chrono::steady_clock::now();
    for (const auto &o : cache.after(m.checkpoint(), limit)) {
        if (budgetMs > 0 && elapsedMs(spent) >= budgetMs)
            break;
        if (o.payload.size() > NTB_OBJECT_DB_MAX_OBJECT_SIZE) {
            passed = o.sequence; // larger than the protocol allows: not a letter
            continue;
        }
        const auto data = ByteView(o.payload);
        auto h = Wire::header(data);
        if (!h) {
            passed = o.sequence;
            continue;
        }
        auto token = Wire::acknowledgmentToken(data);
        if (!token.empty()) {
            if (m.acknowledge(token))
                ++count;
        }
        if (h->type == 0 && h->expires > time) {
            for (const auto &i : v.identities())
                if (!i.chan && Wire::requestsIdentity(data, i) &&
                    toNumber(m.setting("pubkey:" + i.address)) < time + 86400)
                    addJob(m, {}, "pubkey", i.address, Wire::encodePubkey(i, time + 28 * 86400));
        }
        if (h->type == 1 && h->expires > time) {
            for (const auto &item : m.outbox())
                if (!terminal(item.state))
                    if (auto key = Wire::decodePubkey(data, item.message.to))
                        saveKey(m, *key, h->expires);
        }
        std::optional<DecodedEnvelope> decoded;
        bool chanBroadcast = false;
        if (h->type == 2 && token.empty())
            for (const auto &i : v.identities()) {
                decoded = Wire::decodeMessage(data, i);
                if (decoded)
                    break;
            }
        if (h->type == 3)
            decoded = decodeBroadcast(data, watch_, publisher, &v, &chanBroadcast);
        if (decoded) {
            auto &d = *decoded;
            auto ackToken = Wire::acknowledgmentToken(d.acknowledgment);
            Bytes material = bytesOf(d.message.from + "\n" + d.message.to + "\n");
            append(material, ackToken);
            const auto id = ackToken.empty() ? o.hash : hex(hash(Digest::Sha256, material));
            saveKey(m, d.sender, time + 28 * 86400);
            // Persist ACK job first: retrying this object after a crash remains idempotent.
            if (!d.acknowledgment.empty() && ProofOfWork::valid(d.acknowledgment, time))
                addJob(m, {}, "incoming-ack", Protocol::inventoryHash(d.acknowledgment),
                       d.acknowledgment, 1000, 1000, "ready");
            noteRelease(m, d, publisher);
            if (d.broadcast && !chanBroadcast)
                m.reveal(d.sender.address); // a post: a pending subscription is listed now
            const bool service =
                !d.broadcast && !chanBroadcast && serviceAddresses_.count(d.message.to);
            const std::string folder =
                service ? "Service"
                        : (chanBroadcast ? "Channels"
                                         : (d.broadcast ? "Broadcasts" : d.message.folder));
            m.storeRouted(id, d.message.from, d.message.to, d.message.subject, d.message.body,
                          o.sequence, folder, h->expires, o.hash);
            // A letter or a chat from a person, not a chan's post, a service's or a broadcast: the
            // sender is dealt with, so their broadcasts are listened for.
            if (folder == "Inbox")
                watchPerson(m, v, d.message.from);
            ++count;
            passed = 0; // store() moved the checkpoint past this object
        } else
            passed = o.sequence;
    }
    if (passed)
        m.advance(passed);
    count += catchUpBroadcasts(cache, m, publisher, time);
    batch.commit();
    return count;
}
} // namespace bmc
