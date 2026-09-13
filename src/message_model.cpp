#include "message_model.h"
#include "session.h"

namespace bm {
MessageModel::MessageModel(Session *session, QObject *parent) : QAbstractListModel(parent), session_(session) {}
int MessageModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : rows_.size(); }
QVariant MessageModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
    const auto &row = rows_[index.row()];
    const auto key = roleNames().value(role);
    return row.value(QString::fromLatin1(key));
}
QHash<int, QByteArray> MessageModel::roleNames() const {
    static const QHash<int, QByteArray> roles{{Qt::UserRole + 1, "hash"}, {Qt::UserRole + 2, "from"},
        {Qt::UserRole + 3, "to"}, {Qt::UserRole + 4, "subject"}, {Qt::UserRole + 5, "preview"},
        {Qt::UserRole + 6, "folder"}, {Qt::UserRole + 7, "state"}, {Qt::UserRole + 8, "deliveryError"},
        {Qt::UserRole + 9, "unread"}, {Qt::UserRole + 10, "kind"}, {Qt::UserRole + 11, "received"}};
    return roles;
}
void MessageModel::setFolder(const QString &folder) { if (folder_ != folder) { folder_ = folder; emit folderChanged(); reload(); } }
void MessageModel::setSearch(const QString &search) { if (search_ != search) { search_ = search; emit searchChanged(); reload(); } }
void MessageModel::reload() {
    beginResetModel(); rows_.clear(); endResetModel(); offset_ = 0; exhausted_ = false; fetchMore();
}
void MessageModel::fetchMore() {
    if (loading_ || exhausted_ || !session_) return;
    loading_ = true; emit loadingChanged();
    const auto page = session_->messagePage(folder_, search_, offset_, 100);
    QVector<QVariantMap> added;
    for (const auto &v : page) added.push_back(v.toMap());
    if (added.isEmpty()) exhausted_ = true;
    else {
        const int first = rows_.size(); beginInsertRows({}, first, first + added.size() - 1);
        rows_ += added; endInsertRows(); offset_ += added.size();
        if (added.size() < 100) exhausted_ = true;
    }
    loading_ = false; emit loadingChanged();
}
void MessageModel::markRead(const QString &id) {
    for (int i = 0; i < rows_.size(); ++i) if (rows_[i].value("hash") == id) {
        rows_[i]["unread"] = false; emit dataChanged(index(i), index(i), {Qt::UserRole + 9}); break;
    }
}
}
