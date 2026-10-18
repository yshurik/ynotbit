// The chats: one row per message, a sequence number per conversation, and the delivery records the
// network works through. This is src/chat_store.cpp of the previous release, character for
// character, plus the chat halves of outbox(), outgoing() and acknowledge() (mailbox.cpp).
#include <algorithm>
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/mailbox.h>
#include <bmc/random.h>
#include <bmc/sqlite.h>
#include <bmc/text.h>
#include <sqlcipher/sqlite3.h>

namespace bmc {
namespace {
// A chat outbox row: the delivery columns first, then the conversation it belongs to.
OutboxItem readChatOutgoing(Statement &s) {
    OutboxItem o;
    o.id = s.text(0);
    o.kind = "direct";
    o.state = s.text(2);
    o.error = s.text(3);
    o.objectHash = s.text(4);
    o.ackToken = s.blob(5);
    o.ackObject = s.blob(6);
    o.expires = s.number(7);
    o.nextAttempt = s.number(8);
    o.attempts = int(s.number(9));
    o.chat = true;
    const auto local = s.text(10), peer = s.text(11);
    o.message = {o.id,    local,       peer, {}, chatMessageBody(s.number(12), s.text(13)),
                 "Chats", s.number(14)};
    return o;
}
const char *kChatOutboxColumns =
    "o.id,'direct',o.state,o.error,o.object_hash,o.ack_token,o.ack_object,"
    "o.expires,o.next_attempt,o.attempts,m.local_address,m.peer_address,m.seqno,"
    "m.body,m.received FROM chat_outbox o JOIN chat_messages m ON m.id=o.id";
} // namespace

std::vector<OutboxItem> Mailbox::outboxChats() const {
    // Chats stay in the mailbox for good, so those whose delivery is over (the terminal states
    // of delivery.cpp) are left out: every tick and every scanned pubkey walks this list.
    Statement chats(db_, (std::string("SELECT ") + kChatOutboxColumns +
                          " WHERE o.state NOT IN "
                          "('acknowledged','published','cancelled','failed','expired') "
                          "ORDER BY m.rowid")
                             .c_str());
    std::vector<OutboxItem> result;
    while (chats.row())
        result.push_back(readChatOutgoing(chats));
    return result;
}

std::optional<OutboxItem> Mailbox::outgoingChat(const std::string &id) const {
    Statement s(db_, (std::string("SELECT ") + kChatOutboxColumns + " WHERE o.id=?").c_str());
    s.text(1, id);
    if (!s.row())
        return {};
    return readChatOutgoing(s);
}

std::vector<std::string> Mailbox::chatAckIds(ByteView token) const {
    Statement s(db_, "SELECT id FROM chat_outbox WHERE ack_token=? AND state NOT IN "
                     "('cancelled','acknowledged')");
    s.blob(1, token);
    std::vector<std::string> ids;
    while (s.row())
        ids.push_back(s.text(0));
    return ids;
}

void Mailbox::storeRouted(const std::string &id, const std::string &sender,
                          const std::string &recipient, const std::string &subject,
                          const std::string &body, int64_t c, const std::string &folder,
                          int64_t expires) {
    ChatPayload payload;
    const auto parsed =
        folder == "Inbox" ? parseChatMessage(subject, body, &payload) : ChatParseResult::Letter;
    if (parsed == ChatParseResult::Chat) {
        storeChat(id, sender, recipient, payload.seqno, payload.body, expires);
        if (c >= 0)
            advance(c);
    } else if (parsed == ChatParseResult::InvalidChat) {
        if (c >= 0)
            advance(c);
    } else {
        store(id, sender, recipient, subject, body, c, folder);
    }
}

std::string Mailbox::queueChat(const std::string &local, const std::string &peer,
                               const std::string &body, int64_t expires) {
    const auto time = now();
    require(!(local.empty() || peer.empty() || text::trim(body).empty() ||
              body.size() > size_t(kMaxChatBodyBytes) || expires <= time ||
              expires > time + kChatTtlSeconds),
            "Invalid chat message or expiry");
    Transaction t(db_);
    // The sender's clock in milliseconds, kept increasing within a conversation. Not a
    // count from 1: a restored mailbox has lost its counters, and the peer still holds
    // the numbers sent before, so a count would send ones it drops as repeats.
    int64_t seq = nowMillis();
    {
        Statement previous(
            db_, "SELECT last_seq FROM chat_sequences WHERE local_address=? AND peer_address=?");
        previous.text(1, local);
        previous.text(2, peer);
        if (previous.row())
            seq = std::max(seq, previous.number(0) + 1);
    }
    const auto id = "chat-" + uuid();
    Statement counter(
        db_, "INSERT INTO chat_sequences(local_address,peer_address,last_seq) VALUES(?,?,?) "
             "ON CONFLICT(local_address,peer_address) DO UPDATE SET last_seq=excluded.last_seq");
    counter.text(1, local);
    counter.text(2, peer);
    counter.number(3, seq);
    counter.row();
    Statement message(
        db_,
        "INSERT INTO "
        "chat_messages(id,local_address,peer_address,direction,seqno,body,received,expires,state) "
        "VALUES(?,?,?,'out',?,?,?,?,'queued')");
    message.text(1, id);
    message.text(2, local);
    message.text(3, peer);
    message.number(4, seq);
    message.text(5, body);
    message.number(6, time);
    message.number(7, expires);
    message.row();
    Statement delivery(db_, "INSERT INTO chat_outbox(id,state,expires) VALUES(?,'queued',?)");
    delivery.text(1, id);
    delivery.number(2, expires);
    delivery.row();
    t.commit();
    return id;
}

void Mailbox::storeChat(const std::string &id, const std::string &sender,
                        const std::string &recipient, int64_t seqno, const std::string &body,
                        int64_t expires, bool outgoing) {
    const auto time = now();
    if (id.empty() || sender.empty() || recipient.empty() || seqno <= 0 ||
        text::trim(body).empty() || body.size() > size_t(kMaxChatBodyBytes) || expires <= time ||
        expires > time + kChatTtlSeconds + 300)
        return;
    Statement s(db_, "INSERT OR IGNORE INTO "
                     "chat_messages(id,local_address,peer_address,direction,seqno,body,received,"
                     "expires,state,unread) "
                     "VALUES(?,?,?, ?,?,?,?,?,'received',?)");
    s.text(1, id);
    s.text(2, outgoing ? sender : recipient);
    s.text(3, outgoing ? recipient : sender);
    s.text(4, outgoing ? "out" : "in");
    s.number(5, seqno);
    s.text(6, body);
    s.number(7, time);
    s.number(8, expires);
    s.number(9, outgoing ? 0 : 1);
    s.row();
    if (!outgoing && sqlite3_changes(db_) > 0)
        ++incomingChats_;
}

std::vector<ChatMessage> Mailbox::chatMessages(const std::string &local, const std::string &peer,
                                               int limit, bool *more) const {
    std::vector<ChatMessage> result;
    // A window is the newest messages, read newest first off the conversation's index, one
    // more than asked for to know whether older ones are left, and turned around.
    Statement s(
        db_,
        limit > 0
            ? "SELECT id,local_address,peer_address,direction,seqno,body,received,expires,state "
              "FROM chat_messages WHERE local_address=? AND peer_address=? "
              "ORDER BY received DESC,rowid DESC LIMIT ?"
            : "SELECT id,local_address,peer_address,direction,seqno,body,received,expires,state "
              "FROM chat_messages WHERE local_address=? AND peer_address=? "
              "ORDER BY received,rowid");
    s.text(1, local);
    s.text(2, peer);
    if (limit > 0)
        s.number(3, int64_t(limit) + 1);
    while (s.row())
        result.push_back(ChatMessage{s.text(0), s.text(1), s.text(2), s.text(5), s.text(8),
                                     s.number(4), s.number(6), s.number(7), s.text(3) == "out"});
    if (more)
        *more = limit > 0 && int(result.size()) > limit;
    if (limit > 0) {
        if (int(result.size()) > limit)
            result.pop_back();
        std::reverse(result.begin(), result.end());
    }
    return result;
}

std::vector<ChatSummary> Mailbox::chatSummaries() const {
    std::vector<ChatSummary> result;
    Statement s(db_, "SELECT m.local_address,m.peer_address,m.body,m.state,m.received,m.direction,"
                     "(SELECT COUNT(*) FROM chat_messages u WHERE u.local_address=m.local_address "
                     "AND u.peer_address=m.peer_address AND u.direction='in' AND u.unread=1) "
                     "FROM chat_messages m WHERE m.rowid=(SELECT l.rowid FROM "
                     "chat_messages l WHERE l.local_address=m.local_address AND "
                     "l.peer_address=m.peer_address "
                     "ORDER BY l.received DESC,l.rowid DESC LIMIT 1) "
                     "ORDER BY m.received DESC,m.rowid DESC");
    while (s.row())
        result.push_back(ChatSummary{s.text(0), s.text(1), s.text(2), s.text(3), s.number(4),
                                     s.text(5) == "out", int(s.number(6))});
    return result;
}

int Mailbox::chatUnread() const {
    Statement s(db_, "SELECT COUNT(*) FROM chat_messages WHERE direction='in' AND unread=1");
    return s.row() ? int(s.number(0)) : 0;
}

int Mailbox::markChatRead(const std::string &local, const std::string &peer) {
    Statement s(db_, "UPDATE chat_messages SET unread=0 WHERE local_address=? AND peer_address=? "
                     "AND direction='in' AND unread=1");
    s.text(1, local);
    s.text(2, peer);
    s.row();
    return sqlite3_changes(db_);
}

std::vector<ChatMessage> Mailbox::unreadChats(int limit) const {
    std::vector<ChatMessage> result;
    // The partial index chat_unread holds exactly these rows, in this order.
    Statement s(db_,
                "SELECT id,local_address,peer_address,direction,seqno,body,received,expires,state "
                "FROM chat_messages WHERE direction='in' AND unread=1 "
                "ORDER BY received,rowid LIMIT ?");
    s.number(1, std::max(1, limit));
    while (s.row())
        result.push_back(ChatMessage{s.text(0), s.text(1), s.text(2), s.text(5), s.text(8),
                                     s.number(4), s.number(6), s.number(7), s.text(3) == "out"});
    return result;
}

std::string Mailbox::chatState(const std::string &id) const {
    Statement s(db_, "SELECT state FROM chat_messages WHERE id=?");
    s.text(1, id);
    return s.row() ? s.text(0) : std::string();
}

bool Mailbox::markChatMessageRead(const std::string &id) {
    Statement s(db_,
                "UPDATE chat_messages SET unread=0 WHERE id=? AND direction='in' AND unread=1");
    s.text(1, id);
    s.row();
    return sqlite3_changes(db_) > 0;
}

void Mailbox::endChatDeliveries(int64_t time) {
    Statement jobs(
        db_,
        "DELETE FROM network_jobs WHERE owner IN (SELECT id FROM chat_messages WHERE expires<=?)");
    jobs.number(1, time);
    jobs.row();
}
} // namespace bmc
