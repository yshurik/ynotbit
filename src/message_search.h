#pragma once
#include "storage.h"
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QWaitCondition>
#include <atomic>
#include <optional>

class QThread;
struct sqlite3;

namespace bm {
// The filter box's text search, off the UI thread. Reading every body of a big
// chan takes seconds (it is all decrypted), so a worker thread checks letters
// one by one on its own read-only connection and reports them in slices; a
// newer start() abandons the older search at the next letter.
class MessageSearch final : public QObject {
    Q_OBJECT
  public:
    explicit MessageSearch(QObject *parent = nullptr);
    ~MessageSearch() override;
    // The mailbox to read; the key is copied (and wiped on close()).
    void open(const QString &path, const Secret &key);
    void close();
    bool isOpen() const;
    // Checks the letters, in order, for the text; returns the search's number.
    quint64 start(const QString &text, const QStringList &hashes);
    void cancel();
    // How long one report's slice runs before it is sent.
    static constexpr int kSliceMs = 120;

  signals:
    // Letters checked in one slice, and those of them that match.
    void checked(quint64 search, const QStringList &done, const QStringList &matched);
    void failed(quint64 search, const QString &error);

  private:
    struct Job {
        quint64 search;
        QString text;
        QStringList hashes;
    };
    void loop();
    void run(sqlite3 *db, const Job &job);
    mutable QMutex mutex_;
    QWaitCondition wake_;
    std::optional<Job> job_;
    bool quit_ = false, reopen_ = false;
    QString path_;
    Secret key_;
    std::atomic<quint64> current_{0};
    QThread *thread_ = nullptr;
};
} // namespace bm
