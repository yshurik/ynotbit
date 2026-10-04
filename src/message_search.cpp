#include "message_search.h"
#include "sqlite_helpers.h"
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <cstring>

namespace bm {
using detail::Statement;

MessageSearch::MessageSearch(QObject *parent) : QObject(parent) {
    thread_ = QThread::create([this] { loop(); });
    thread_->start();
}
MessageSearch::~MessageSearch() {
    {
        QMutexLocker lock(&mutex_);
        quit_ = true;
        ++current_;
        wake_.wakeOne();
    }
    thread_->wait();
    delete thread_;
}
void MessageSearch::open(const QString &path, const Secret &key) {
    QMutexLocker lock(&mutex_);
    if (path_ == path && key_.size() == key.size() && key.size() &&
        std::memcmp(key_.data(), key.data(), key.size()) == 0)
        return;
    path_ = path;
    Secret copy(key.size());
    std::memcpy(copy.data(), key.data(), key.size());
    key_ = std::move(copy);
    reopen_ = true;
    wake_.wakeOne();
}
void MessageSearch::close() {
    QMutexLocker lock(&mutex_);
    ++current_;
    job_.reset();
    if (path_.isEmpty())
        return;
    path_.clear();
    key_ = Secret();
    reopen_ = true;
    wake_.wakeOne();
}
bool MessageSearch::isOpen() const {
    QMutexLocker lock(&mutex_);
    return !path_.isEmpty();
}
quint64 MessageSearch::start(const QString &text, const QStringList &hashes) {
    QMutexLocker lock(&mutex_);
    const quint64 search = ++current_;
    job_ = Job{search, text, hashes};
    wake_.wakeOne();
    return search;
}
void MessageSearch::cancel() {
    QMutexLocker lock(&mutex_);
    ++current_;
    job_.reset();
}

static sqlite3 *openReader(const QString &path, const Secret &key) {
    sqlite3 *db = nullptr;
    const auto name = QFile::encodeName(path);
    try {
        if (sqlite3_open_v2(name.constData(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                            nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot open mailbox");
        // A letter being saved holds the file briefly; wait for it like the
        // UI's own connection does.
        sqlite3_busy_timeout(db, 3000);
        if (sqlite3_key(db, key.data(), int(key.size())) != SQLITE_OK)
            throw std::runtime_error("Cannot apply mailbox key");
        if (sqlite3_exec(db,
                         "PRAGMA cipher_memory_security=ON; PRAGMA temp_store=MEMORY; "
                         "PRAGMA query_only=1;",
                         nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
        Statement verify(db, "SELECT count(*) FROM sqlite_master");
        verify.row();
        return db;
    } catch (...) {
        sqlite3_close_v2(db);
        throw;
    }
}
void MessageSearch::loop() {
    sqlite3 *db = nullptr;
    QString openError;
    const auto closeDb = [&] {
        sqlite3_close_v2(db);
        db = nullptr;
    };
    while (true) {
        Job job{0, {}, {}};
        bool reopen = false;
        QString path;
        Secret key;
        {
            QMutexLocker lock(&mutex_);
            while (!quit_ && !job_ && !reopen_)
                wake_.wait(&mutex_);
            if (quit_)
                break;
            std::swap(reopen, reopen_);
            if (reopen && !path_.isEmpty()) {
                path = path_;
                key = Secret(key_.size());
                std::memcpy(key.data(), key_.data(), key_.size());
            }
            if (job_) {
                job = std::move(*job_);
                job_.reset();
            }
        }
        if (reopen) {
            closeDb();
            openError.clear();
            if (!path.isEmpty()) {
                try {
                    db = openReader(path, key);
                } catch (const std::exception &e) {
                    openError = QString::fromUtf8(e.what());
                }
            }
        }
        key = Secret();
        if (!job.search)
            continue;
        if (!db) {
            emit failed(job.search, openError.isEmpty() ? QStringLiteral("Mailbox is closed")
                                                        : openError);
            continue;
        }
        try {
            run(db, job);
        } catch (const std::exception &e) {
            emit failed(job.search, QString::fromUtf8(e.what()));
        }
    }
    closeDb();
}
void MessageSearch::run(sqlite3 *db, const Job &job) {
    // The same test as the list's own search (Mailbox::messageCount): subject,
    // sender, recipient or body contains the text, ASCII case folded.
    Statement s(db, "SELECT 1 FROM messages WHERE hash=?1 AND (instr(lower(subject),lower(?2)) "
                    "OR instr(lower(sender),lower(?2)) OR instr(lower(recipient),lower(?2)) OR "
                    "instr(lower(body),lower(?2)))");
    QStringList done, matched;
    QElapsedTimer slice;
    slice.start();
    for (const auto &hash : job.hashes) {
        if (current_ != job.search)
            return;
        s.reset();
        s.text(1, hash);
        s.text(2, job.text);
        const bool hit = s.row();
        // Between letters this connection holds no lock, so new mail is saved
        // while a long search runs.
        s.reset();
        if (hit)
            matched << hash;
        done << hash;
        if (slice.elapsed() >= kSliceMs) {
            emit checked(job.search, done, matched);
            done.clear();
            matched.clear();
            slice.restart();
        }
    }
    if (!done.isEmpty() && current_ == job.search)
        emit checked(job.search, done, matched);
}
} // namespace bm
