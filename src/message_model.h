#pragma once
#include <QAbstractListModel>
#include <QCache>
#include <QVariantMap>

namespace bm {
class Session;
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
  signals:
    void folderChanged();
    void searchChanged();
  public slots:
    void setFolder(const QString &folder);
    void setSearch(const QString &search);
    void setChannel(const QString &address);
    void setUnreadOnly(bool on);
    void setAnonymousOnly(bool on);

  private:
    Session *session_;
    mutable QCache<int, QVariantList> pages_{3};
    int count_ = 0;
    QString folder_ = "Inbox", search_, channel_;
    bool unreadOnly_ = false, anonymousOnly_ = false;
};
} // namespace bm
