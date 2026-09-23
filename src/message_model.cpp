#include "message_model.h"
#include "session.h"

namespace bm {
MessageModel::MessageModel(Session *session, QObject *parent)
    : QAbstractListModel(parent), session_(session) {}
int MessageModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : count_;
}
QVariant MessageModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count_)
        return {};
    const int page = index.row() / 100;
    if (!pages_.contains(page)) {
        try {
            pages_.insert(page, new QVariantList(session_->messagePage(
                                    folder_, search_, page * 100, 100,
                                    folder_ == "Channels" ? channel_ : QString(), unreadOnly_,
                                    anonymousOnly_)));
        } catch (...) {
            return {};
        }
    }
    const auto &rows = *pages_.object(page);
    if (index.row() % 100 >= rows.size())
        return {};
    const auto row = rows[index.row() % 100].toMap();
    const auto key = roleNames().value(role);
    return row.value(QString::fromLatin1(key));
}
QHash<int, QByteArray> MessageModel::roleNames() const {
    static const QHash<int, QByteArray> roles{
        {Qt::UserRole + 1, "hash"},     {Qt::UserRole + 2, "from"},
        {Qt::UserRole + 3, "to"},       {Qt::UserRole + 4, "subject"},
        {Qt::UserRole + 5, "preview"},  {Qt::UserRole + 6, "folder"},
        {Qt::UserRole + 7, "state"},    {Qt::UserRole + 8, "deliveryError"},
        {Qt::UserRole + 9, "unread"},   {Qt::UserRole + 10, "kind"},
        {Qt::UserRole + 11, "received"}};
    return roles;
}
void MessageModel::setFolder(const QString &folder) {
    if (folder_ != folder) {
        folder_ = folder;
        emit folderChanged();
        reload();
    }
}
void MessageModel::setSearch(const QString &search) {
    if (search_ != search) {
        search_ = search;
        emit searchChanged();
        reload();
    }
}
void MessageModel::reload() {
    const int count =
        folder_ == "Channels" && channel_.isEmpty()
            ? 0
            : session_->messageCount(folder_, search_, folder_ == "Channels" ? channel_ : QString(),
                                     unreadOnly_, anonymousOnly_);
    if (count == count_) {
        pages_.clear();
        if (count_)
            emit dataChanged(index(0), index(count_ - 1));
        return;
    }
    beginResetModel();
    pages_.clear();
    count_ = count;
    endResetModel();
}
void MessageModel::setChannel(const QString &address) {
    if (channel_ == address)
        return;
    beginResetModel();
    channel_ = address;
    pages_.clear();
    count_ = folder_ == "Channels" && channel_.isEmpty()
                 ? 0
                 : session_->messageCount(folder_, search_, folder_ == "Channels" ? channel_ : QString(),
                                          unreadOnly_, anonymousOnly_);
    endResetModel();
}
void MessageModel::setUnreadOnly(bool on) {
    if (unreadOnly_ == on)
        return;
    unreadOnly_ = on;
    reload();
}
void MessageModel::setAnonymousOnly(bool on) {
    if (anonymousOnly_ == on)
        return;
    anonymousOnly_ = on;
    reload();
}
int MessageModel::totalCount() const {
    return folder_ == "Channels" && channel_.isEmpty()
               ? 0
               : session_->messageCount(folder_, search_, folder_ == "Channels" ? channel_ : QString());
}
int MessageModel::rowForHash(const QString &hash) const {
    if (hash.isEmpty())
        return -1;
    for (int row = 0; row < count_; ++row)
        if (data(index(row), Qt::UserRole + 1).toString() == hash)
            return row;
    return -1;
}
void MessageModel::markRead(const QString &id) {
    for (int page : pages_.keys()) {
        auto *rows = pages_.object(page);
        for (int i = 0; i < rows->size(); ++i) {
            auto row = (*rows)[i].toMap();
            if (row.value("hash") != id)
                continue;
            row["unread"] = false;
            (*rows)[i] = row;
            emit dataChanged(index(page * 100 + i), index(page * 100 + i), {Qt::UserRole + 9});
            return;
        }
    }
}
} // namespace bm
