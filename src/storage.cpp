#include "storage.h"
#include "protocol.h"
#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>
#include <cstring>
#include <sqlite3.h>
#include <stdexcept>
#include <utility>
namespace bm {
static void fail(const QString &m) {
    throw std::runtime_error(m.toStdString());
}
static void check(bool b, const QString &m) {
    if (!b)
        fail(m);
}
struct SensitiveBytes {
    QByteArray bytes;
    ~SensitiveBytes() {
        if (!bytes.isEmpty())
            sodium_memzero(bytes.data(), size_t(bytes.size()));
    }
};
Secret::Secret(size_t n) : n_(n) {
    if (sodium_init() < 0)
        fail("Cannot initialize cryptography");
    if (n) {
        p_ = static_cast<unsigned char *>(sodium_malloc(n));
        if (!p_)
            throw std::bad_alloc();
        sodium_memzero(p_, n);
    }
}
Secret::~Secret() {
    clear();
}
void Secret::clear() {
    if (p_)
        sodium_free(p_);
    p_ = nullptr;
    n_ = 0;
}
Secret::Secret(Secret &&o) noexcept
    : p_(std::exchange(o.p_, nullptr)), n_(std::exchange(o.n_, 0)) {}
Secret &Secret::operator=(Secret &&o) noexcept {
    if (this != &o) {
        clear();
        p_ = std::exchange(o.p_, nullptr);
        n_ = std::exchange(o.n_, 0);
    }
    return *this;
}
static Secret derive(const QByteArray &password, const QByteArray &salt) {
    check(!password.isEmpty(), "Enter a vault password");
    Secret key(32);
    check(crypto_pwhash(key.data(), 32, password.constData(), password.size(),
                        reinterpret_cast<const unsigned char *>(salt.constData()), 3,
                        64 * 1024 * 1024, crypto_pwhash_ALG_ARGON2ID13) == 0,
          "Insufficient memory to unlock vault");
    return key;
}
static void reserveFile(const QString &path) {
    QFile f(path);
    check(f.open(QIODevice::WriteOnly | QIODevice::NewOnly),
          "File already exists or cannot be created");
    check(f.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
          "Cannot protect file permissions");
}
void Vault::create(const QString &path, const QByteArray &password) {
    check(!unlocked_, "Lock the current vault first");
    check(!password.isEmpty(), "Enter a vault password");
    reserveFile(path);
    try {
        path_ = path;
        salt_.resize(16);
        randombytes_buf(salt_.data(), 16);
        wrapping_ = derive(password, salt_);
        unlocked_ = true;
        save();
    } catch (...) {
        lock();
        QFile::remove(path);
        throw;
    }
}
void Vault::save() {
    check(unlocked_, "Vault is locked");
    check(identities_.size() <= 1024 && mailboxKeys_.size() <= 1024,
          "Vault capacity exceeded; existing vault preserved");
    SensitiveBytes plain;
    QDataStream out(&plain.bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << quint32(identities_.size());
    for (const auto &i : identities_) {
        out << i.label << i.address << i.chan;
        out.writeRawData(reinterpret_cast<const char *>(i.keys.data()), 64);
    }
    out << quint32(mailboxKeys_.size());
    for (const auto &[id, key] : mailboxKeys_) {
        out << id;
        out.writeRawData(reinterpret_cast<const char *>(key.data()), 32);
    }
    check(out.status() == QDataStream::Ok, "Cannot serialize vault");
    check(plain.bytes.size() + 64 <= 4 * 1024 * 1024,
          "Vault is too large; existing vault preserved");
    QByteArray header("BMVAULT1", 8);
    header += salt_;
    QByteArray nonce(24, 0);
    randombytes_buf(nonce.data(), nonce.size());
    header += nonce;
    QByteArray cipher(plain.bytes.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, 0);
    unsigned long long n = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char *>(cipher.data()), &n,
        reinterpret_cast<const unsigned char *>(plain.bytes.constData()), plain.bytes.size(),
        reinterpret_cast<const unsigned char *>(header.constData()), header.size(), nullptr,
        reinterpret_cast<const unsigned char *>(nonce.constData()), wrapping_.data());
    QSaveFile f(path_);
    check(f.open(QIODevice::WriteOnly), "Cannot write vault");
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    check(f.write(header) == header.size() && f.write(cipher) == cipher.size() && f.commit(),
          "Vault save failed; previous vault preserved");
}
void Vault::unlock(const QString &path, const QByteArray &password) {
    check(!unlocked_, "Lock the current vault first");
    QFile f(path);
    check(f.open(QIODevice::ReadOnly), "Cannot open vault");
    check(f.size() >= 64 && f.size() <= 4 * 1024 * 1024, "Invalid vault size");
    auto file = f.readAll();
    check(file.left(8) == "BMVAULT1", "Unsupported vault format");
    auto salt = file.mid(8, 16);
    auto key = derive(password, salt);
    SensitiveBytes plain;
    plain.bytes.resize(file.size() - 48 - 16);
    unsigned long long n = 0;
    check(crypto_aead_xchacha20poly1305_ietf_decrypt(
              reinterpret_cast<unsigned char *>(plain.bytes.data()), &n, nullptr,
              reinterpret_cast<const unsigned char *>(file.constData() + 48), file.size() - 48,
              reinterpret_cast<const unsigned char *>(file.constData()), 48,
              reinterpret_cast<const unsigned char *>(file.constData() + 24), key.data()) == 0,
          "Wrong password or damaged vault");
    QDataStream in(plain.bytes);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 count = 0;
    in >> count;
    check(count <= 1024, "Too many vault identities");
    std::vector<Identity> identities;
    for (quint32 j = 0; j < count; ++j) {
        Identity i;
        in >> i.label >> i.address >> i.chan;
        check(in.readRawData(reinterpret_cast<char *>(i.keys.data()), 64) == 64, "Truncated vault");
        identities.push_back(std::move(i));
    }
    in >> count;
    check(count <= 1024, "Too many mailbox keys");
    std::map<QString, Secret> keys;
    for (quint32 j = 0; j < count; ++j) {
        QString id;
        in >> id;
        Secret k(32);
        check(in.readRawData(reinterpret_cast<char *>(k.data()), 32) == 32, "Truncated vault key");
        check(keys.emplace(id, std::move(k)).second, "Duplicate mailbox key");
    }
    check(in.status() == QDataStream::Ok && in.atEnd(), "Invalid vault contents");
    path_ = path;
    salt_ = salt;
    wrapping_ = std::move(key);
    identities_ = std::move(identities);
    mailboxKeys_ = std::move(keys);
    unlocked_ = true;
}
void Vault::lock() {
    identities_.clear();
    mailboxKeys_.clear();
    wrapping_.clear();
    salt_.clear();
    path_.clear();
    unlocked_ = false;
}
void Vault::changePassword(const QByteArray &password) {
    check(unlocked_, "Vault is locked");
    auto oldSalt = salt_;
    Secret oldKey = std::move(wrapping_);
    try {
        salt_.resize(16);
        salt_.detach();
        randombytes_buf(salt_.data(), 16);
        wrapping_ = derive(password, salt_);
        save();
    } catch (...) {
        salt_ = oldSalt;
        wrapping_ = std::move(oldKey);
        throw;
    }
}
QString Vault::addIdentity(const QString &label) {
    check(unlocked_, "Vault is locked");
    identities_.push_back(Protocol::identity(label));
    try {
        save();
    } catch (...) {
        identities_.pop_back();
        throw;
    }
    return identities_.back().address;
}

QString Vault::addMailboxKey() {
    check(unlocked_, "Vault is locked");
    auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Secret key(32);
    randombytes_buf(key.data(), 32);
    mailboxKeys_.emplace(id, std::move(key));
    try {
        save();
    } catch (...) {
        mailboxKeys_.erase(id);
        throw;
    }
    return id;
}
const Secret &Vault::mailboxKey(const QString &id) const {
    check(unlocked_, "Vault is locked");
    auto it = mailboxKeys_.find(id);
    check(it != mailboxKeys_.end(), "Mailbox key is not in this vault");
    return it->second;
}
QStringList Vault::mailboxIds() const {
    QStringList ids;
    for (const auto &[id, key] : mailboxKeys_)
        ids << id;
    return ids;
}
class Statement {
    sqlite3_stmt *s_ = nullptr;

  public:
    Statement(sqlite3 *db, const char *sql) {
        check(db, "Mailbox is closed");
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK)
            fail(QString::fromUtf8(sqlite3_errmsg(db)));
    }
    ~Statement() {
        sqlite3_finalize(s_);
    }
    sqlite3_stmt *get() const {
        return s_;
    }
    void text(int n, const QString &t) {
        auto b = t.toUtf8();
        check(sqlite3_bind_text(s_, n, b.constData(), b.size(), SQLITE_TRANSIENT) == SQLITE_OK,
              "Cannot bind value");
    }
    void number(int n, qint64 i) {
        sqlite3_bind_int64(s_, n, i);
    }
    bool row() {
        int r = sqlite3_step(s_);
        if (r == SQLITE_ROW)
            return true;
        check(r == SQLITE_DONE, QString::fromUtf8(sqlite3_errmsg(sqlite3_db_handle(s_))));
        return false;
    }
    QString text(int n) const {
        auto p = sqlite3_column_text(s_, n);
        return p ? QString::fromUtf8(reinterpret_cast<const char *>(p), sqlite3_column_bytes(s_, n))
                 : QString();
    }
    qint64 number(int n) const {
        return sqlite3_column_int64(s_, n);
    }
};
void Mailbox::sql(const char *q) {
    check(db_, "Mailbox is closed");
    char *err = nullptr;
    int r = sqlite3_exec(db_, q, nullptr, nullptr, &err);
    QString m = QString::fromUtf8(err ? err : "");
    sqlite3_free(err);
    check(r == SQLITE_OK, m);
}
void Mailbox::connect(const QString &path, const Secret &key, bool create) {
    check(!db_, "Close the current mailbox first");
    check(key.size() == 32, "Invalid mailbox key");
    if (create)
        reserveFile(path);
    auto bytes = QFile::encodeName(path);
    int r = sqlite3_open_v2(bytes.constData(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                            nullptr);
    if (r != SQLITE_OK) {
        close();
        if (create)
            QFile::remove(path);
        fail("Cannot open mailbox");
    }
    try {
        sqlite3_busy_timeout(db_, 3000);
#ifdef SQLITE_HAS_CODEC
        check(sqlite3_key(db_, key.data(), 32) == SQLITE_OK, "Cannot apply mailbox key");
#else
        fail("This build lacks SQLCipher");
#endif
        Statement version(db_, "PRAGMA cipher_version");
        check(version.row() && !version.text(0).isEmpty(), "SQLCipher is required");
        sql("PRAGMA cipher_memory_security=ON; PRAGMA temp_store=MEMORY; PRAGMA foreign_keys=ON;");
        Statement verify(db_, "SELECT count(*) FROM sqlite_master");
        verify.row();
    } catch (...) {
        close();
        if (create)
            QFile::remove(path);
        throw;
    }
}
void Mailbox::create(const QString &path, const QString &id, const Secret &key) {
    connect(path, key, true);
    try {
        sql("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; BEGIN IMMEDIATE; CREATE TABLE "
            "meta(id TEXT NOT NULL, checkpoint INTEGER NOT NULL, cache_id TEXT NOT NULL DEFAULT "
            "'', identities TEXT NOT NULL DEFAULT ''); CREATE TABLE messages(hash TEXT PRIMARY "
            "KEY, sender TEXT NOT NULL, recipient TEXT NOT NULL, subject TEXT NOT NULL, body TEXT "
            "NOT NULL, folder TEXT NOT NULL, received INTEGER NOT NULL); PRAGMA user_version=1;");
        Statement s(db_, "INSERT INTO meta(id,checkpoint) VALUES(?,0)");
        s.text(1, id);
        s.row();
        sql("COMMIT");
    } catch (...) {
        close();
        QFile::remove(path);
        throw;
    }
}
void Mailbox::open(const QString &path, const Secret &key) {
    connect(path, key, false);
    try {
        Statement s(db_, "PRAGMA user_version");
        check(s.row() && s.number(0) == 1, "Unsupported mailbox format");
        keyId();
    } catch (...) {
        close();
        throw;
    }
}
void Mailbox::close() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}
QString Mailbox::keyId() const {
    Statement s(db_, "SELECT id FROM meta");
    check(s.row(), "Invalid mailbox");
    auto id = s.text(0);
    check(!s.row(), "Invalid mailbox metadata");
    return id;
}
qint64 Mailbox::checkpoint() const {
    Statement s(db_, "SELECT checkpoint FROM meta");
    check(s.row(), "Invalid checkpoint");
    return s.number(0);
}
void Mailbox::advance(qint64 c) {
    Statement s(db_, "UPDATE meta SET checkpoint=?");
    s.number(1, c);
    s.row();
}
void Mailbox::store(const QString &hash, const QString &from, const QString &to,
                    const QString &subject, const QString &body, qint64 c, const QString &folder) {
    sql("BEGIN IMMEDIATE");
    try {
        Statement s(db_, "INSERT OR IGNORE INTO messages VALUES(?,?,?,?,?,?,?)");
        s.text(1, hash);
        s.text(2, from);
        s.text(3, to);
        s.text(4, subject);
        s.text(5, body);
        s.text(6, folder);
        s.number(7, QDateTime::currentSecsSinceEpoch());
        s.row();
        advance(c);
        sql("COMMIT");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
QVector<Message> Mailbox::messages() const {
    Statement s(db_, "SELECT hash,sender,recipient,subject,body,folder,received FROM messages "
                     "ORDER BY received DESC,rowid DESC");
    QVector<Message> list;
    while (s.row())
        list.push_back(
            {s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), s.text(5), s.number(6)});
    return list;
}
void Mailbox::backup(const QString &path, const Secret &key) {
    check(db_, "Mailbox is closed");
    Mailbox dest;
    dest.connect(path, key, true);
    auto b = sqlite3_backup_init(dest.db_, "main", db_, "main");
    if (!b) {
        dest.close();
        QFile::remove(path);
        fail("Cannot start mailbox backup");
    }
    int r = sqlite3_backup_step(b, -1);
    int f = sqlite3_backup_finish(b);
    dest.close();
    if (r != SQLITE_DONE || f != SQLITE_OK) {
        QFile::remove(path);
        fail("Mailbox backup failed");
    }
}
} // namespace bm

namespace bm {
void Mailbox::bindCache(const QString &id) {
    Statement s(
        db_,
        "UPDATE meta SET checkpoint=CASE WHEN cache_id=? THEN checkpoint ELSE 0 END, cache_id=?");
    s.text(1, id);
    s.text(2, id);
    s.row();
}
} // namespace bm

namespace bm {
void Mailbox::bindIdentities(const QString &fingerprint) {
    Statement s(db_, "UPDATE meta SET checkpoint=CASE WHEN identities=? THEN checkpoint ELSE 0 "
                     "END, identities=?");
    s.text(1, fingerprint);
    s.text(2, fingerprint);
    s.row();
}
} // namespace bm
