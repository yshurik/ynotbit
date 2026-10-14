#include "chat_format.h"
#include "sqlite_helpers.h"
#include "storage.h"
#include <QDateTime>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
namespace bm {
using detail::Statement;
using detail::Transaction;
void Mailbox::storeRouted(const QString &id, const QString &sender, const QString &recipient,
                          const QString &subject, const QString &body, qint64 checkpoint,
                          const QString &folder, qint64 expires) {
    ChatPayload payload;
    const auto parsed = folder == "Inbox" ? parseChatMessage(subject, body, &payload)
                                          : ChatParseResult::Letter;
    if (parsed == ChatParseResult::Chat) {
        storeChat(id, sender, recipient, payload.seqno, payload.body, expires);
        if (checkpoint >= 0) advance(checkpoint);
    } else if (parsed == ChatParseResult::InvalidChat) {
        if (checkpoint >= 0) advance(checkpoint);
    } else {
        store(id, sender, recipient, subject, body, checkpoint, folder);
    }
}
QString Mailbox::queueChat(const QString &local, const QString &peer, const QString &body,
                           qint64 expires) {
    const auto time = QDateTime::currentSecsSinceEpoch();
    if (local.isEmpty() || peer.isEmpty() || body.trimmed().isEmpty() ||
        body.toUtf8().size() > kMaxChatBodyBytes || expires <= time ||
        expires > time + kChatTtlSeconds)
        throw std::runtime_error("Invalid chat message or expiry");
    Transaction t(db_);
    // The sender's clock in milliseconds, kept increasing within a conversation. Not a
    // count from 1: a restored mailbox has lost its counters, and the peer still holds
    // the numbers sent before, so a count would send ones it drops as repeats.
    qint64 seq = QDateTime::currentMSecsSinceEpoch();
    {
        Statement previous(db_, "SELECT last_seq FROM chat_sequences WHERE local_address=? AND peer_address=?");
        previous.text(1, local);
        previous.text(2, peer);
        if (previous.row())
            seq = std::max(seq, previous.number(0) + 1);
    }
    const auto id = "chat-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    Statement counter(db_, "INSERT INTO chat_sequences(local_address,peer_address,last_seq) VALUES(?,?,?) "
                           "ON CONFLICT(local_address,peer_address) DO UPDATE SET last_seq=excluded.last_seq");
    counter.text(1, local); counter.text(2, peer); counter.number(3, seq); counter.row();
    Statement message(db_, "INSERT INTO chat_messages(id,local_address,peer_address,direction,seqno,body,received,expires,state) "
                           "VALUES(?,?,?,'out',?,?,?,?,'queued')");
    message.text(1, id); message.text(2, local); message.text(3, peer); message.number(4, seq);
    message.text(5, body); message.number(6, time); message.number(7, expires); message.row();
    Statement delivery(db_, "INSERT INTO chat_outbox(id,state,expires) VALUES(?,'queued',?)");
    delivery.text(1, id); delivery.number(2, expires); delivery.row();
    t.commit();
    return id;
}
void Mailbox::storeChat(const QString &id, const QString &sender, const QString &recipient,
                        qint64 seqno, const QString &body, qint64 expires, bool outgoing) {
    const auto time = QDateTime::currentSecsSinceEpoch();
    if (id.isEmpty() || sender.isEmpty() || recipient.isEmpty() || seqno <= 0 ||
        body.trimmed().isEmpty() || body.toUtf8().size() > kMaxChatBodyBytes || expires <= time ||
        expires > time + kChatTtlSeconds + 300)
        return;
    Statement s(db_, "INSERT OR IGNORE INTO chat_messages(id,local_address,peer_address,direction,seqno,body,received,expires,state,unread) "
                    "VALUES(?,?,?, ?,?,?,?,?,'received',?)");
    s.text(1, id); s.text(2, outgoing ? sender : recipient); s.text(3, outgoing ? recipient : sender);
    s.text(4, outgoing ? "out" : "in"); s.number(5, seqno); s.text(6, body);
    s.number(7, time); s.number(8, expires); s.number(9, outgoing ? 0 : 1); s.row();
}
QVector<ChatMessage> Mailbox::chatMessages(const QString &local, const QString &peer, int limit,
                                           bool *more) const {
    QVector<ChatMessage> result;
    // A window is the newest messages, read newest first off the conversation's index, one
    // more than asked for to know whether older ones are left, and turned around.
    Statement s(db_, limit > 0 ? "SELECT id,local_address,peer_address,direction,seqno,body,received,expires,state "
                                 "FROM chat_messages WHERE local_address=? AND peer_address=? "
                                 "ORDER BY received DESC,rowid DESC LIMIT ?"
                               : "SELECT id,local_address,peer_address,direction,seqno,body,received,expires,state "
                                 "FROM chat_messages WHERE local_address=? AND peer_address=? "
                                 "ORDER BY received,rowid");
    s.text(1, local); s.text(2, peer);
    if (limit > 0) s.number(3, qint64(limit) + 1);
    while (s.row()) result << ChatMessage{s.text(0),s.text(1),s.text(2),s.text(5),s.text(8),
                                           s.number(4),s.number(6),s.number(7),s.text(3)=="out"};
    if (more) *more = limit > 0 && result.size() > limit;
    if (limit > 0) {
        if (result.size() > limit) result.removeLast();
        std::reverse(result.begin(), result.end());
    }
    return result;
}
QVector<ChatSummary> Mailbox::chatSummaries() const {
    QVector<ChatSummary> result;
    Statement s(db_, "SELECT m.local_address,m.peer_address,m.body,m.state,m.received,m.direction,"
                     "(SELECT COUNT(*) FROM chat_messages u WHERE u.local_address=m.local_address "
                     "AND u.peer_address=m.peer_address AND u.direction='in' AND u.unread=1) "
                     "FROM chat_messages m WHERE m.rowid=(SELECT l.rowid FROM "
                     "chat_messages l WHERE l.local_address=m.local_address AND "
                     "l.peer_address=m.peer_address "
                     "ORDER BY l.received DESC,l.rowid DESC LIMIT 1) "
                     "ORDER BY m.received DESC,m.rowid DESC");
    while (s.row())
        result << ChatSummary{s.text(0),   s.text(1),          s.text(2),       s.text(3),
                              s.number(4), s.text(5) == "out", int(s.number(6))};
    return result;
}
int Mailbox::chatUnread() const {
    Statement s(db_, "SELECT COUNT(*) FROM chat_messages WHERE direction='in' AND unread=1");
    return s.row() ? int(s.number(0)) : 0;
}
int Mailbox::markChatRead(const QString &local, const QString &peer) {
    Statement s(db_, "UPDATE chat_messages SET unread=0 WHERE local_address=? AND peer_address=? "
                     "AND direction='in' AND unread=1");
    s.text(1, local);
    s.text(2, peer);
    s.row();
    return sqlite3_changes(db_);
}
void Mailbox::endChatDeliveries(qint64 time) {
    Statement jobs(db_, "DELETE FROM network_jobs WHERE owner IN (SELECT id FROM chat_messages WHERE expires<=?)");
    jobs.number(1,time); jobs.row();
}
}
