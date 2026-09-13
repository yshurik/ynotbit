#include "session.h"
#include "protocol.h"
#include "scanner.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QStandardPaths>
#include <QUuid>
#include <stdexcept>
namespace bm {
static void check(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
struct Password {
    QByteArray bytes;
    ~Password() {
        if (!bytes.isEmpty())
            sodium_memzero(bytes.data(), bytes.size());
    }
};
static Password password(const QString &title, bool confirm = false) {
    bool ok = false;
    QString text =
        QInputDialog::getText(nullptr, title, "Vault password", QLineEdit::Password, {}, &ok);
    if (!ok)
        throw std::runtime_error("Cancelled");
    Password p{text.toUtf8()};
    text.fill(QChar(0));
    check(!p.bytes.isEmpty(), "Password cannot be empty");
    if (confirm) {
        auto repeated =
            QInputDialog::getText(nullptr, title, "Repeat password", QLineEdit::Password, {}, &ok);
        auto b = repeated.toUtf8();
        bool same = ok && b == p.bytes;
        sodium_memzero(b.data(), b.size());
        repeated.fill(QChar(0));
        check(same, "Passwords do not match");
    }
    return p;
}
static QString chooseSave(const QString &title, const QString &filter, const QString &suffix) {
    auto p = QFileDialog::getSaveFileName(
        nullptr, title, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        filter);
    if (!p.isEmpty() && !p.endsWith(suffix))
        p += suffix;
    return p;
}
Session::Session(QString root, bool offline, QObject *parent)
    : QObject(parent), root_(std::move(root)), offline_(offline) {
    QDir().mkpath(root_);
    nodeLock_ = std::make_unique<QLockFile>(root_ + "/desktop.lock");
    check(nodeLock_->tryLock(), "Another app instance is using this node folder");
    cache_ = std::make_unique<Cache>(root_);
    connect(&node_, &QProcess::readyReadStandardOutput, this,
            [this] { node_.readAllStandardOutput(); });
    connect(&node_, &QProcess::readyReadStandardError, this, [this] {
        auto text = QString::fromUtf8(node_.readAllStandardError()).trimmed();
        if (!text.isEmpty()) {
            error_ = "Node: " + text.left(400);
            emit changed();
        }
    });
    connect(&node_, &QProcess::errorOccurred, this, [this] {
        error_ = "Node: " + node_.errorString();
        emit changed();
    });
    connect(&node_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) { emit changed(); });
#ifdef Q_OS_UNIX
    if (!offline_) {
        node_.setProgram(QCoreApplication::applicationFilePath());
        node_.setArguments({"--node", "-D", root_, "-m", root_ + "/unused-maildir", "-i"});
        node_.start();
    }
#else
    if (!offline_)
        error_ = "Native Windows relay port is not yet available in this development build";
#endif
    connect(&timer_, &QTimer::timeout, this, &Session::tick);
    timer_.start(750);
    activity_ = "Waiting for network objects";
}
Session::~Session() {
    timer_.stop();
    lock();
    if (node_.state() != QProcess::NotRunning) {
        node_.terminate();
        if (!node_.waitForFinished(3000)) {
            node_.kill();
            node_.waitForFinished(1000);
        }
    }
}
QString Session::status() const {
    return offline_                             ? "Offline mode"
           : node_.state() == QProcess::Running ? "Node running · receiving and relaying"
                                                : "Node stopped";
}
QString Session::document() const {
    return mailboxOpen() ? QFileInfo(mailPath_).fileName() : "No mailbox open";
}
QVariantList Session::identities() const {
    QVariantList result;
    for (const auto &i : vault_.identities())
        result << QVariantMap{{"label", i.label}, {"address", i.address}, {"chan", i.chan}};
    return result;
}
void Session::attempt(const std::function<void()> &f) {
    if (busy_)
        return;
    busy_ = true;
    try {
        error_.clear();
        f();
    } catch (const std::exception &e) {
        auto message = QString::fromUtf8(e.what());
        if (message != "Cancelled")
            error_ = message;
    }
    busy_ = false;
    emit changed();
}
void Session::acquireVault(const QString &p) {
    vaultLock_ = std::make_unique<QLockFile>(p + ".lock");
    check(vaultLock_->tryLock(), "Vault is in use by another instance");
}
void Session::createVault() {
    attempt([&] {
        check(!unlocked(), "Lock the current vault first");
        auto p = chooseSave("Create vault", "Bitmessage vault (*.bmvault)", ".bmvault");
        if (p.isEmpty())
            return;
        auto pass = password("Create vault", true);
        acquireVault(p);
        try {
            vault_.create(p, pass.bytes);
            vaultPath_ = p;
            mailPath_.clear();
            mailKey_.clear();
            activity_ = "Vault created. Add an identity and create a mailbox.";
        } catch (...) {
            vaultLock_.reset();
            throw;
        }
    });
}
void Session::openVault() {
    attempt([&] {
        check(!unlocked(), "Lock the current vault first");
        auto p =
            QFileDialog::getOpenFileName(nullptr, "Open vault", {}, "Bitmessage vault (*.bmvault)");
        if (p.isEmpty())
            return;
        auto pass = password("Unlock vault");
        acquireVault(p);
        try {
            vault_.unlock(p, pass.bytes);
            vaultPath_ = p;
            mailPath_.clear();
            mailKey_.clear();
            activity_ = "Vault unlocked. Open a mailbox to inspect cached objects.";
        } catch (...) {
            vaultLock_.reset();
            throw;
        }
    });
}
void Session::unlockVault() {
    if (vaultPath_.isEmpty()) {
        openVault();
        return;
    }
    attempt([&] {
        if (unlocked())
            return;
        auto pass = password("Unlock vault");
        acquireVault(vaultPath_);
        try {
            vault_.unlock(vaultPath_, pass.bytes);
        } catch (...) {
            vaultLock_.reset();
            throw;
        }
        if (!mailPath_.isEmpty())
            openMailboxPath(mailPath_);
    });
}
void Session::createMailbox() {
    attempt([&] {
        check(unlocked(), "Unlock a vault first");
        check(!mailboxOpen(), "Lock and unlock the vault before choosing another mailbox");
        auto p = chooseSave("Create mailbox", "Bitmessage mailbox (*.bmmail)", ".bmmail");
        if (p.isEmpty())
            return;
        mailLock_ = std::make_unique<QLockFile>(p + ".lock");
        check(mailLock_->tryLock(), "Mailbox is in use");
        try {
            auto id = vault_.addMailboxKey();
            mailbox_.create(p, id, vault_.mailboxKey(id));
            mailPath_ = p;
            mailKey_ = id;
            mailbox_.bindCache(cache_->id());
            refresh();
        } catch (...) {
            mailLock_.reset();
            throw;
        }
    });
}
void Session::openMailboxPath(const QString &p) {
    check(unlocked(), "Unlock a vault first");
    mailbox_.close();
    messages_.clear();
    mailLock_.reset();
    mailLock_ = std::make_unique<QLockFile>(p + ".lock");
    check(mailLock_->tryLock(), "Mailbox is open in another instance");
    for (const auto &id : vault_.mailboxIds()) {
        try {
            mailbox_.open(p, vault_.mailboxKey(id));
            check(mailbox_.keyId() == id, "Mailbox key identifier mismatch");
            mailbox_.bindCache(cache_->id());
            mailPath_ = p;
            mailKey_ = id;
            refresh();
            return;
        } catch (...) {
            mailbox_.close();
        }
    }
    mailLock_.reset();
    throw std::runtime_error("This vault cannot open that mailbox, or the mailbox is damaged");
}
void Session::openMailbox() {
    attempt([&] {
        check(unlocked(), "Unlock a vault first");
        auto p = QFileDialog::getOpenFileName(nullptr, "Open mailbox", {},
                                              "Bitmessage mailbox (*.bmmail)");
        if (!p.isEmpty())
            openMailboxPath(p);
    });
}
void Session::lock() {
    mailbox_.close();
    messages_.clear();
    vault_.lock();
    mailLock_.reset();
    vaultLock_.reset();
    activity_ = "Vault locked. Objects remain cached for later inspection.";
    emit locked();
    emit changed();
}
void Session::addIdentity() {
    attempt([&] {
        check(unlocked(), "Unlock a vault first");
        bool ok;
        auto label =
            QInputDialog::getText(nullptr, "New identity", "Label", QLineEdit::Normal, {}, &ok);
        if (!ok)
            return;
        vault_.addIdentity(label);
        if (mailboxOpen())
            mailbox_.advance(0);
        activity_ = "Identity created. Retained objects will be inspected again.";
    });
}
void Session::joinChannel() {
    attempt([&] {
        check(unlocked(), "Unlock a vault first");
        bool ok;
        auto phrase =
            QInputDialog::getText(nullptr, "Join or create chan", "Shared phrase (exact spelling)",
                                  QLineEdit::Password, {}, &ok);
        if (!ok)
            return;
        auto expected =
            QInputDialog::getText(nullptr, "Verify chan address",
                                  "Expected BM-address (leave empty to create a version 4 chan)",
                                  QLineEdit::Normal, {}, &ok);
        if (!ok) {
            phrase.fill(QChar(0));
            return;
        }
        vault_.addChannel(phrase, "Chan", expected.trimmed());
        phrase.fill(QChar(0));
        if (mailboxOpen())
            mailbox_.advance(0);
        activity_ = "Chan joined. The address appears in Identities.";
    });
}
void Session::importIdentities() {
    attempt([&] {
        check(unlocked(), "Unlock a vault first");
        auto p = QFileDialog::getOpenFileName(nullptr, "Import notbit / PyBitmessage identities",
                                              {}, "Key files (*.dat);;All files (*)");
        if (p.isEmpty())
            return;
        vault_.importKeys(p);
        if (mailboxOpen())
            mailbox_.advance(0);
        activity_ = "Identities imported into the encrypted vault. The source file was preserved.";
    });
}
void Session::changePassword() {
    attempt([&] {
        check(unlocked(), "Unlock a vault first");
        auto pass = password("Change vault password", true);
        vault_.changePassword(pass.bytes);
    });
}
void Session::backup() {
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        auto dir = QFileDialog::getExistingDirectory(nullptr, "Choose backup folder");
        if (dir.isEmpty())
            return;
        auto base =
            dir + "/bitmessage-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmss");
        mailbox_.backup(base + ".bmmail", vault_.mailboxKey(mailKey_));
        check(QFile::copy(vaultPath_, base + ".bmvault"),
              "Mailbox backed up, but vault copy failed");
        activity_ = "Mailbox and vault backup saved to " + dir;
    });
}
void Session::saveDraft(QString recipient, QString subject, QString body) {
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        check(body.size() <= 200000, "Draft is too large");
        mailbox_.store("draft-" + QUuid::createUuid().toString(), "", recipient, subject, body,
                       mailbox_.checkpoint(), "Drafts");
        refresh();
        activity_ = "Draft saved in the encrypted mailbox. It has not been sent.";
    });
}
void Session::rescan() {
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        mailbox_.advance(0);
    });
}
void Session::refresh() {
    messages_.clear();
    if (!mailboxOpen())
        return;
    for (const auto &m : mailbox_.messages())
        messages_ << QVariantMap{
            {"hash", m.hash},
            {"from", m.from},
            {"to", m.to},
            {"subject", m.subject},
            {"body", m.body},
            {"folder", m.folder},
            {"received",
             QDateTime::fromSecsSinceEpoch(m.received).toString("dd MMM yyyy · hh:mm")}};
}
void Session::tick() {
    if (busy_)
        return;
    try {
        cache_->discover();
        cache_->prune();
        if (mailboxOpen()) {
            if (scanMailbox(*cache_, mailbox_, vault_) > 0)
                refresh();
            activity_ = cache_->after(mailbox_.checkpoint(), 1).isEmpty()
                            ? "Mailbox up to date with retained objects"
                            : "Inspecting cached objects · checkpoint " +
                                  QString::number(mailbox_.checkpoint());
        }
        if (cache_->pruned() > 0)
            activity_ = "Retention cleanup removed older objects · Some older letters may no "
                        "longer be recoverable";
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    emit changed();
}
} // namespace bm
