#pragma once
#include <QAbstractListModel>
#include <QVariantMap>

namespace bm {
class Session;
class MessageModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY searchChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
public:
    explicit MessageModel(Session *session, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString folder() const { return folder_; }
    QString search() const { return search_; }
    bool loading() const { return loading_; }
    Q_INVOKABLE void reload();
    Q_INVOKABLE void fetchMore();
    Q_INVOKABLE void markRead(const QString &id);
signals:
    void folderChanged();
    void searchChanged();
    void loadingChanged();
public slots:
    void setFolder(const QString &folder);
    void setSearch(const QString &search);
private:
    Session *session_;
    QVector<QVariantMap> rows_;
    QString folder_ = "Inbox", search_;
    int offset_ = 0;
    bool loading_ = false, exhausted_ = false;
};
}
