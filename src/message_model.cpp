#include "message_model.h"
#include "message_search.h"
#include "session.h"
#include <QSet>

namespace bm {
MessageModel::MessageModel(Session *session, QObject *parent)
    : QAbstractListModel(parent), session_(session), engine_(new MessageSearch(this)) {
    connect(engine_, &MessageSearch::checked, this, &MessageModel::onChecked);
    connect(engine_, &MessageSearch::failed, this, [this](quint64 search, const QString &) {
        if (search == searchId_)
            setSearching(false);
    });
}
int MessageModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : showingSearch_ ? int(shown_.size()) : count_;
}
QVariant MessageModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const auto key = QString::fromLatin1(roleNames().value(role));
    if (showingSearch_) {
        const auto &hash = shown_[index.row()];
        if (!rows_.contains(hash))
            fetchAround(index.row());
        const auto row = rows_.constFind(hash);
        return row == rows_.cend() ? QVariant() : row->value(key);
    }
    const int page = index.row() / 100;
    if (!pages_.contains(page)) {
        try {
            pages_.insert(page, new QVariantList(session_->messagePage(
                                    folder_, search_, page * 100, 100, source(), unreadOnly_,
                                    anonymousOnly_)));
        } catch (...) {
            return {};
        }
    }
    const auto &rows = *pages_.object(page);
    if (index.row() % 100 >= rows.size())
        return {};
    return rows[index.row() % 100].toMap().value(key);
}
void MessageModel::fetchAround(int row) const {
    if (rows_.size() > 1000)
        rows_.clear();
    // A row's preview reads its whole body, so fetch about a screenful at a time.
    QStringList wanted;
    const int first = row - row % 20;
    for (int i = first; i < std::min<int>(first + 20, shown_.size()); ++i)
        if (!rows_.contains(shown_[i]))
            wanted << shown_[i];
    try {
        rows_.insert(session_->messageRows(wanted));
    } catch (...) {
    }
}
QHash<int, QByteArray> MessageModel::roleNames() const {
    static const QHash<int, QByteArray> roles{
        {Qt::UserRole + 1, "hash"},     {Qt::UserRole + 2, "from"},
        {Qt::UserRole + 3, "to"},       {Qt::UserRole + 4, "subject"},
        {Qt::UserRole + 5, "preview"},  {Qt::UserRole + 6, "folder"},
        {Qt::UserRole + 7, "state"},    {Qt::UserRole + 8, "deliveryError"},
        {Qt::UserRole + 9, "unread"},   {Qt::UserRole + 10, "kind"},
        {Qt::UserRole + 11, "received"}, {Qt::UserRole + 12, "fromName"},
        {Qt::UserRole + 13, "toName"}};
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
    if (!search_.isEmpty()) {
        restartSearch();
        return;
    }
    if (session_->mailboxOpen())
        engine_->cancel();
    else
        engine_->close();
    setSearching(false);
    const int count = perSource() && channel_.isEmpty()
                          ? 0
                          : session_->messageCount(folder_, {}, source(), unreadOnly_,
                                                   anonymousOnly_);
    if (count == count_ && !showingSearch_) {
        pages_.clear();
        if (count_)
            emit dataChanged(index(0), index(count_ - 1));
        return;
    }
    beginResetModel();
    pages_.clear();
    count_ = count;
    showingSearch_ = false;
    shown_.clear();
    rows_.clear();
    endResetModel();
}
void MessageModel::restartSearch() {
    const auto scope = folder_ + '\n' + source();
    // A draft's text changes under the same hash, so its verdicts never last.
    if (scope != verdictScope_ || folder_ == "Drafts") {
        verdicts_.clear();
        verdictScope_ = scope;
    }
    if (search_ != verdictText_) {
        // A longer text matches a subset: rejections stand, matches are rechecked.
        if (!verdictText_.isEmpty() && search_.contains(verdictText_)) {
            for (auto i = verdicts_.begin(); i != verdicts_.end();)
                i = i.value() ? verdicts_.erase(i) : std::next(i);
        } else {
            verdicts_.clear();
        }
        verdictText_ = search_;
    }
    const auto *key = session_->mailboxSecret();
    if (key)
        engine_->open(session_->mailPath(), *key);
    else
        engine_->close();
    QStringList candidates;
    try {
        if (key && !(perSource() && channel_.isEmpty()))
            candidates = session_->messageHashes(folder_, source(), unreadOnly_, anonymousOnly_);
    } catch (...) {
    }
    QStringList shown, pending;
    for (const auto &hash : candidates) {
        const auto verdict = verdicts_.constFind(hash);
        if (verdict == verdicts_.cend())
            pending << hash;
        else if (*verdict)
            shown << hash;
    }
    if (showingSearch_ && listedText_ == search_ && candidates == candidates_ &&
        shown == shown_) {
        // New mail elsewhere: rows may have changed (read state), the list did not.
        rows_.clear();
        if (!shown_.isEmpty())
            emit dataChanged(index(0), index(int(shown_.size()) - 1));
        if (searching_ || pending.isEmpty())
            return;
    } else {
        beginResetModel();
        pages_.clear();
        rows_.clear();
        showingSearch_ = true;
        listedText_ = search_;
        candidates_ = candidates;
        shown_ = shown;
        endResetModel();
    }
    pendingTotal_ = int(pending.size());
    pendingDone_ = 0;
    if (pending.isEmpty()) {
        engine_->cancel();
        setSearching(false);
        return;
    }
    searchId_ = engine_->start(search_, pending);
    setSearching(true);
    emit searchStateChanged();
}
void MessageModel::onChecked(quint64 search, const QStringList &done,
                             const QStringList &matched) {
    if (search != searchId_ || !showingSearch_)
        return;
    const QSet<QString> hits(matched.cbegin(), matched.cend());
    for (const auto &hash : done)
        verdicts_.insert(hash, hits.contains(hash));
    pendingDone_ += int(done.size());
    if (!hits.isEmpty()) {
        // Matches join the list in its order, wherever they fall.
        int row = 0;
        for (const auto &hash : candidates_) {
            const auto verdict = verdicts_.constFind(hash);
            if (verdict == verdicts_.cend() || !*verdict)
                continue;
            if (row < shown_.size() && shown_[row] == hash) {
                ++row;
                continue;
            }
            beginInsertRows({}, row, row);
            shown_.insert(row, hash);
            endInsertRows();
            ++row;
        }
    }
    if (pendingDone_ >= pendingTotal_)
        setSearching(false);
    emit searchStateChanged();
}
void MessageModel::setSearching(bool on) {
    if (searching_ == on)
        return;
    searching_ = on;
    emit searchStateChanged();
}
int MessageModel::searchProgress() const {
    return pendingTotal_ ? 100 * pendingDone_ / pendingTotal_ : 100;
}
void MessageModel::setChannel(const QString &address) {
    if (channel_ == address)
        return;
    channel_ = address;
    if (!search_.isEmpty()) {
        restartSearch();
        return;
    }
    beginResetModel();
    pages_.clear();
    showingSearch_ = false;
    shown_.clear();
    rows_.clear();
    count_ = perSource() && channel_.isEmpty()
                 ? 0
                 : session_->messageCount(folder_, {}, source(), unreadOnly_, anonymousOnly_);
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
    if (showingSearch_)
        return int(shown_.size());
    return perSource() && channel_.isEmpty() ? 0
                                             : session_->messageCount(folder_, {}, source());
}
int MessageModel::rowForHash(const QString &hash) const {
    if (hash.isEmpty())
        return -1;
    if (showingSearch_)
        return int(shown_.indexOf(hash));
    if (perSource() && channel_.isEmpty())
        return -1;
    try {
        const int row = session_->messagePosition(hash, folder_, source(), unreadOnly_,
                                                  anonymousOnly_);
        return row < count_ ? row : -1;
    } catch (...) {
        return -1;
    }
}
void MessageModel::markRead(const QString &id) {
    if (showingSearch_) {
        const auto row = rows_.find(id);
        if (row != rows_.end()) {
            (*row)["unread"] = false;
            const int at = int(shown_.indexOf(id));
            if (at >= 0)
                emit dataChanged(index(at), index(at), {Qt::UserRole + 9});
        }
        return;
    }
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
