#pragma once
#include <QAbstractListModel>
#include <QCache>
#include <QHash>
#include <QStringList>
#include <QVariantMap>

namespace bm {
class Session;
class MessageSearch;
class MessageModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY searchChanged)
  public:
    explicit MessageModel(Session *session, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString folder() const {
        return folder_;
    }
    QString search() const {
        return search_;
    }
    Q_INVOKABLE void reload();
    Q_INVOKABLE void markRead(const QString &id);
    Q_INVOKABLE int rowForHash(const QString &hash) const;
    int cachedPages() const {
        return pages_.size();
    }
    int totalCount() const;
    // A search still checking letters, and how far it got (0-100).
    bool searching() const {
        return searching_;
    }
    int searchProgress() const;
  signals:
    void folderChanged();
    void searchChanged();
    void searchStateChanged();
  public slots:
    void setFolder(const QString &folder);
    void setSearch(const QString &search);
    void setChannel(const QString &address);
    void setUnreadOnly(bool on);
    void setAnonymousOnly(bool on);

  private:
    // Channels and Broadcasts list one source at a time: the chan, or the
    // subscribed sender (a broadcast's recipient is the address it came from).
    bool perSource() const {
        return folder_ == "Channels" || folder_ == "Broadcasts";
    }
    QString source() const {
        return perSource() ? channel_ : QString();
    }
    // Searching: the list holds the letters found so far, filled in while a
    // worker thread reads the rest (MessageSearch); the UI never waits on it.
    void restartSearch();
    void onChecked(quint64 search, const QStringList &done, const QStringList &matched);
    void setSearching(bool on);
    void fetchAround(int row) const;
    Session *session_;
    MessageSearch *engine_;
    mutable QCache<int, QVariantList> pages_{3};
    int count_ = 0;
    QString folder_ = "Inbox", search_, channel_;
    bool unreadOnly_ = false, anonymousOnly_ = false;
    bool showingSearch_ = false, searching_ = false;
    // The searched list: candidates in list order, each one's verdict once
    // checked (kept while the text only grows), and the matches shown.
    QStringList candidates_, shown_;
    QHash<QString, bool> verdicts_;
    QString verdictText_, verdictScope_, listedText_;
    quint64 searchId_ = 0;
    int pendingTotal_ = 0, pendingDone_ = 0;
    mutable QHash<QString, QVariantMap> rows_;
};
} // namespace bm
