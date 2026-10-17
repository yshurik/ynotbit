// The mailbox's chats: a thin wrapper over bmc::Mailbox (bmclient), which holds the SQL.
#include "storage.h"
#include "qt_bridge.h"
namespace bm {
namespace {
template <class Out, class In> QVector<Out> convertAll(const std::vector<In> &in) {
    QVector<Out> out;
    out.reserve(int(in.size()));
    for (const auto &v : in)
        out.push_back(bridge::fromCore(v));
    return out;
}
} // namespace

QString Mailbox::queueChat(const QString &local, const QString &peer, const QString &body,
                           qint64 expires) {
    return bridge::fromStd(
        core_->queueChat(bridge::toStd(local), bridge::toStd(peer), bridge::toStd(body), expires));
}
void Mailbox::storeRouted(const QString &id, const QString &sender, const QString &recipient,
                          const QString &subject, const QString &body, qint64 checkpoint,
                          const QString &folder, qint64 expires) {
    core_->storeRouted(bridge::toStd(id), bridge::toStd(sender), bridge::toStd(recipient),
                       bridge::toStd(subject), bridge::toStd(body), checkpoint,
                       bridge::toStd(folder), expires);
}
void Mailbox::storeChat(const QString &id, const QString &sender, const QString &recipient,
                        qint64 seqno, const QString &body, qint64 expires, bool outgoing) {
    core_->storeChat(bridge::toStd(id), bridge::toStd(sender), bridge::toStd(recipient), seqno,
                     bridge::toStd(body), expires, outgoing);
}
QVector<ChatMessage> Mailbox::chatMessages(const QString &local, const QString &peer, int limit,
                                           bool *more) const {
    return convertAll<ChatMessage>(
        core_->chatMessages(bridge::toStd(local), bridge::toStd(peer), limit, more));
}
QVector<ChatSummary> Mailbox::chatSummaries() const {
    return convertAll<ChatSummary>(core_->chatSummaries());
}
int Mailbox::chatUnread() const {
    return core_->chatUnread();
}
int Mailbox::markChatRead(const QString &local, const QString &peer) {
    return core_->markChatRead(bridge::toStd(local), bridge::toStd(peer));
}
QVector<ChatMessage> Mailbox::unreadChats(int limit) const {
    return convertAll<ChatMessage>(core_->unreadChats(limit));
}
bool Mailbox::markChatMessageRead(const QString &id) {
    return core_->markChatMessageRead(bridge::toStd(id));
}
QString Mailbox::chatState(const QString &id) const {
    return bridge::fromStd(core_->chatState(bridge::toStd(id)));
}
void Mailbox::endChatDeliveries(qint64 time) {
    core_->endChatDeliveries(time);
}
} // namespace bm
