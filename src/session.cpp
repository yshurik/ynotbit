#include "session.h"
#include "appearance.h"
#include "message_model.h"
#include "protocol.h"
#include "protocol_wire.h"
#include "scanner.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
namespace bm {
static QString addressInput(const QString &title, const QString &label, bool *accepted) {
    QInputDialog dialog;
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setInputMode(QInputDialog::TextInput);
    if (auto field = dialog.findChild<QLineEdit *>())
        field->setFont(addressFont());
    *accepted = dialog.exec() == QDialog::Accepted;
    return dialog.textValue();
}
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
static QString documentsPath() {
    const auto path = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return path.isEmpty() ? QDir::homePath() : path;
}
Session::Session(QString root, bool offline, QObject *parent)
    : QObject(parent), root_(std::move(root)), offline_(offline) {
    messageModel_ = std::make_unique<MessageModel>(this, this);
    QDir().mkpath(root_);
    nodeLock_ = std::make_unique<QLockFile>(root_ + "/desktop.lock");
    check(nodeLock_->tryLock(), "Another app instance is using this node folder");
    cache_ = std::make_unique<Cache>(root_);
    delivery_ = std::make_unique<Delivery>(root_);
    QSettings recent(root_ + "/desktop.ini", QSettings::IniFormat);
    vaultPath_ = recent.value("vault").toString();
    mailPath_ = recent.value("mailbox").toString();
    recentVaultPaths_ = recent.value("recentVaults").toStringList();
    recentMailboxPaths_ = recent.value("recentMailboxes").toStringList();
    retentionMB_ = std::clamp(recent.value("retentionMB", 512).toInt(), 64, 32768);
    retentionDays_ = std::clamp(recent.value("retentionDays", 90).toInt(), 1, 3650);
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
    startNode();
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
    if (offline_)
        return "Offline · outgoing objects stay queued";
    if (node_.state() != QProcess::Running)
        return "Node stopped · outgoing objects stay queued";
    QFile f(root_ + "/status.json");
    if (!f.open(QIODevice::ReadOnly))
        return "Node starting · connecting to peers";
    auto status = QJsonDocument::fromJson(f.read(4096)).object();
    if (QDateTime::currentSecsSinceEpoch() - status.value("time").toInteger() > 10)
        return "Node status unavailable";
    auto peers = status.value("peers").toInt();
    return peers ? QString("%1 connected peers · receiving and relaying").arg(peers)
                 : "No connected peers · waiting for network";
}
QString Session::document() const {
    return mailboxOpen() ? QFileInfo(mailPath_).fileName() : "No mailbox open";
}
QVariantList Session::identities() const {
    QVariantList result;
    for (const auto &i : vault_.identities())
        result << QVariantMap{{"label", i.label},
                              {"address", i.address},
                              {"chan", i.chan},
                              {"default", i.isDefault}};
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
void Session::rememberVault(const QString &path) {
    recentVaultPaths_.removeAll(path);
    recentVaultPaths_.prepend(path);
    while (recentVaultPaths_.size() > 8)
        recentVaultPaths_.removeLast();
}
void Session::rememberMailbox(const QString &path) {
    recentMailboxPaths_.removeAll(path);
    recentMailboxPaths_.prepend(path);
    while (recentMailboxPaths_.size() > 8)
        recentMailboxPaths_.removeLast();
}
QVariantList Session::recentVaults() const {
    QVariantList result;
    for (const auto &p : recentVaultPaths_)
        if (QFileInfo::exists(p))
            result << QVariantMap{{"name", QFileInfo(p).fileName()}, {"path", p}};
    return result;
}
QVariantList Session::recentMailboxes() const {
    QVariantList result;
    for (const auto &p : recentMailboxPaths_)
        if (QFileInfo::exists(p))
            result << QVariantMap{{"name", QFileInfo(p).fileName()}, {"path", p}};
    return result;
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
            rememberVault(p);
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
        auto p = QFileDialog::getOpenFileName(nullptr, "Open vault", documentsPath(),
                                              "Bitmessage vault (*.bmvault)");
        if (p.isEmpty())
            return;
        auto pass = password("Unlock vault");
        acquireVault(p);
        try {
            vault_.unlock(p, pass.bytes);
            vaultPath_ = p;
            mailPath_.clear();
            mailKey_.clear();
            rememberVault(p);
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
            rememberVault(vaultPath_);
        } catch (...) {
            vaultLock_.reset();
            throw;
        }
        if (!mailPath_.isEmpty())
            openMailboxPath(mailPath_);
    });
}
void Session::beginVaultCreate() {
    attempt([&] {
        check(!unlocked(), "Lock the current vault first");
        auto p = chooseSave("Create vault", "Bitmessage vault (*.bmvault)", ".bmvault");
        if (p.isEmpty())
            return;
        pendingVaultPath_ = p;
        emit vaultPasswordRequired(p, true);
    });
}
void Session::beginVaultOpen() {
    attempt([&] {
        check(!unlocked(), "Lock the current vault first");
        auto p = QFileDialog::getOpenFileName(nullptr, "Open vault", documentsPath(),
                                              "Bitmessage vault (*.bmvault)");
        if (p.isEmpty())
            return;
        pendingVaultPath_ = p;
        emit vaultPasswordRequired(p, false);
    });
}
void Session::beginVaultUnlock() {
    if (vaultPath_.isEmpty()) {
        beginVaultOpen();
        return;
    }
    attempt([&] {
        check(!unlocked(), "Vault is already unlocked");
        pendingVaultPath_ = vaultPath_;
        emit vaultPasswordRequired(vaultPath_, false);
    });
}
void Session::submitVaultPassword(QString passphrase, QString repeated, bool create) {
    attempt([&] {
        check(!pendingVaultPath_.isEmpty(), "Choose a vault first");
        check(!passphrase.isEmpty(), "Password cannot be empty");
        if (create)
            check(passphrase == repeated, "Passwords do not match");
        const auto path = pendingVaultPath_;
        Password secret{passphrase.toUtf8()};
        auto &bytes = secret.bytes;
        passphrase.fill(QChar(0));
        repeated.fill(QChar(0));
        if (create) {
            check(!QFile::exists(path),
                  "Choose a new filename; existing vaults are never overwritten");
            acquireVault(path);
            try {
                vault_.create(path, bytes);
                vaultPath_ = path;
                mailPath_.clear();
                mailKey_.clear();
                rememberVault(path);
                activity_ = "Vault created. Add an identity and create a mailbox.";
            } catch (...) {
                vaultLock_.reset();
                sodium_memzero(bytes.data(), bytes.size());
                throw;
            }
        } else {
            acquireVault(path);
            try {
                vault_.unlock(path, bytes);
                if (vaultPath_ != path) {
                    mailPath_.clear();
                    mailKey_.clear();
                }
                vaultPath_ = path;
                rememberVault(path);
                activity_ = "Vault unlocked. Open a mailbox to inspect cached objects.";
            } catch (...) {
                vaultLock_.reset();
                sodium_memzero(bytes.data(), bytes.size());
                throw;
            }
        }
        sodium_memzero(bytes.data(), bytes.size());
        pendingVaultPath_.clear();
        emit vaultPasswordAccepted();
        if (!create && !mailPath_.isEmpty())
            openMailboxPath(mailPath_);
        emit changed();
    });
}
void Session::createMailbox() {
    emit aboutToCloseMailbox();
    attempt([&] {
        check(unlocked(), "Unlock a vault first");

        auto p = chooseSave("Create mailbox", "Bitmessage mailbox (*.bmmail)", ".bmmail");
        if (p.isEmpty())
            return;
        check(!QFile::exists(p),
              "Choose a new filename; existing mailbox documents are never overwritten");
        delivery_->stop();
        mailbox_.close();
        clearMessages();
        mailLock_.reset();
        mailLock_ = std::make_unique<QLockFile>(p + ".lock");
        check(mailLock_->tryLock(), "Mailbox is in use");
        try {
            auto id = vault_.addMailboxKey();
            mailbox_.create(p, id, vault_.mailboxKey(id));
            mailPath_ = p;
            mailKey_ = id;
            rememberMailbox(p);
            mailbox_.bindCache(cache_->id());
            refresh();
        } catch (...) {
            mailLock_.reset();
            throw;
        }
    });
}
void Session::openMailboxPath(const QString &p) {
    delivery_->stop();
    check(unlocked(), "Unlock a vault first");
    mailbox_.close();
    clearMessages();
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
            rememberMailbox(p);
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
    emit aboutToCloseMailbox();
    attempt([&] {
        check(unlocked(), "Unlock a vault first");
        auto p = QFileDialog::getOpenFileName(nullptr, "Open mailbox", documentsPath(),
                                              "Bitmessage mailbox (*.bmmail)");
        if (!p.isEmpty())
            openMailboxPath(p);
    });
}
void Session::openMailboxAt(QString path) {
    emit aboutToCloseMailbox();
    attempt([&] { openMailboxPath(path); });
}
void Session::lock() {
    emit aboutToCloseMailbox();
    if (delivery_)
        delivery_->stop();
    QSettings recent(root_ + "/desktop.ini", QSettings::IniFormat);
    recent.setValue("vault", vaultPath_);
    recent.setValue("mailbox", mailPath_);
    recent.setValue("recentVaults", recentVaultPaths_);
    recent.setValue("recentMailboxes", recentMailboxPaths_);
    mailbox_.close();
    clearMessages();
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
            addressInput("Verify chan address",
                         "Expected BM-address (leave empty to create a version 4 chan)", &ok);
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
                                              documentsPath(), "Key files (*.dat);;All files (*)");
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
        auto dir =
            QFileDialog::getExistingDirectory(nullptr, "Choose backup folder", documentsPath());
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
        mailbox_.saveDraft({}, {}, recipient, subject, body);
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
    if (!mailboxOpen()) {
        clearMessages();
        return;
    }
    if (displayedRevision_ == mailbox_.messageRevision())
        return;
    displayedRevision_ = mailbox_.messageRevision();
    emit messagesChanged();
    if (messageModel_)
        messageModel_->reload();
}
QVariantList Session::channels() const {
    QMap<QString, QString> labels;
    if (mailboxOpen())
        for (const auto &address : mailbox_.channelAddresses())
            labels[address] = address;
    if (unlocked())
        for (const auto &i : vault_.identities())
            if (i.chan)
                labels[i.address] = i.label.isEmpty() ? i.address : i.label;
    QVariantList result;
    for (auto i = labels.cbegin(); i != labels.cend(); ++i)
        result << QVariantMap{{"address", i.key()}, {"label", i.value()}};
    return result;
}
QVariantList Session::messagePage(const QString &folder, const QString &search, int offset,
                                  int limit, const QString &recipient) const {
    QVariantList result;
    if (!mailboxOpen())
        return result;
    for (const auto &m : mailbox_.messageSummaries(folder, search, offset, limit, recipient)) {
        OutboxItem out;
        if (m.folder == "Outbox" || m.folder == "Sent") {
            try {
                out = mailbox_.outgoing(m.hash);
            } catch (...) {
            }
        }
        result << QVariantMap{
            {"hash", m.hash},
            {"from", m.from},
            {"to", m.to},
            {"subject", m.subject},
            {"preview", m.body},
            {"folder", m.folder},
            {"state", out.state},
            {"deliveryError", out.error},
            {"unread", mailbox_.unread(m.hash)},
            {"kind",
             out.kind.isEmpty() ? mailbox_.setting("draftkind:" + m.hash, "direct") : out.kind},
            {"received",
             QDateTime::fromSecsSinceEpoch(m.received).toString("dd MMM yyyy · hh:mm")}};
    }
    return result;
}
QVariantMap Session::message(QString id) const {
    if (!mailboxOpen())
        return {};
    const auto m = mailbox_.message(id);
    OutboxItem out;
    try {
        out = mailbox_.outgoing(id);
    } catch (...) {
    }
    return {
        {"hash", m.hash},
        {"from", m.from},
        {"to", m.to},
        {"subject", m.subject},
        {"body", m.body},
        {"preview", m.body.left(240)},
        {"storedAt", m.received},
        {"folder", m.folder},
        {"state", out.state},
        {"deliveryError", out.error},
        {"unread", mailbox_.unread(m.hash)},
        {"kind", out.kind.isEmpty() ? mailbox_.setting("draftkind:" + m.hash, "direct") : out.kind},
        {"received", QDateTime::fromSecsSinceEpoch(m.received).toString("dd MMM yyyy · hh:mm")}};
}
void Session::clearMessages() {
    displayedRevision_.reset();
    if (messageModel_)
        messageModel_->reload();
    emit messagesChanged();
}
void Session::tick() {
    if (busy_)
        return;
    try {
        cache_->discover();
        cache_->prune(qint64(retentionMB_) * 1024 * 1024, retentionDays_);
        if (mailboxOpen()) {
            delivery_->scan(*cache_, mailbox_, vault_);
            delivery_->tick(mailbox_, vault_, !offline_ && node_.state() == QProcess::Running);
            refresh();
            activity_ = cache_->after(mailbox_.checkpoint(), 1).isEmpty()
                            ? "Mailbox up to date with retained objects"
                            : "Inspecting cached objects · checkpoint " +
                                  QString::number(mailbox_.checkpoint());
        }
        if (delivery_->working())
            activity_ = "Preparing outgoing proof of work · locking pauses preparation";
        if (cache_->pruned() > 0)
            activity_ = "Retention cleanup removed older objects · Some older letters may no "
                        "longer be recoverable";
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    emit changed();
}
QString Session::saveLetter(QString id, QString from, QString to, QString subject, QString body,
                            QString kind) {
    QString result;
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        result = mailbox_.saveDraft(id, from, to.trimmed(), subject, body);
        mailbox_.setSetting("draftkind:" + result, kind == "broadcast" ? "broadcast" : "direct");
        refresh();
    });
    return result;
}
bool Session::sendLetter(QString id) {
    bool sent = false;
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        auto m = mailbox_.message(id);
        bool own = false;
        for (const auto &i : vault_.identities())
            if (i.address == m.from)
                own = true;
        check(own, "Choose a sender from this vault");
        auto kind = mailbox_.setting("draftkind:" + id, "direct");
        check(kind == "broadcast" || Wire::validAddress(m.to),
              "Enter a valid Bitmessage recipient address");
        check(!m.subject.contains('\n') && !m.subject.contains('\r'), "Subject must be one line");
        check(!m.body.trimmed().isEmpty(), "Write a message before sending");
        check(m.subject.toUtf8().size() + m.body.toUtf8().size() + 14 <= 200000,
              "Message is too large");
        mailbox_.queueDraft(id, kind, QDateTime::currentSecsSinceEpoch() + 4 * 86400);
        mailbox_.advance(0);
        refresh();
        sent = true;
        activity_ = "Letter queued. Follow its progress in Outbox.";
    });
    return sent;
}
void Session::retryLetter(QString id) {
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        delivery_->retry(mailbox_, id);
        refresh();
    });
}
void Session::cancelLetter(QString id) {
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        delivery_->cancel(mailbox_, id);
        refresh();
    });
}
void Session::moveLetter(QString id, QString folder) {
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        mailbox_.moveMessage(id, folder);
        refresh();
    });
}
void Session::restoreLetter(QString id) {
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        mailbox_.restoreMessage(id);
        refresh();
    });
}
void Session::deleteLetter(QString id) {
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        if (QMessageBox::question(
                nullptr, "Delete letter permanently?",
                "This removes the letter from this mailbox. Backups are unchanged.") !=
            QMessageBox::Yes)
            return;
        mailbox_.deleteMessage(id);
        refresh();
    });
}
void Session::readLetter(QString id) {
    attempt([&] {
        if (mailboxOpen()) {
            // Catch up first only if another operation changed the mailbox. Marking
            // one row read must not reload all message bodies from SQLCipher.
            refresh();
            mailbox_.markRead(id);
            displayedRevision_ = mailbox_.messageRevision();
            messageModel_->markRead(id);
            emit messageRead(id);
        }
    });
}
QVariantList Session::deliveryHistory(QString id) {
    QVariantList result;
    if (!mailboxOpen())
        return result;
    try {
        for (const auto &e : mailbox_.events(id))
            result << QVariantMap{
                {"time",
                 QDateTime::fromSecsSinceEpoch(e.timestamp).toString("dd MMM yyyy · HH:mm:ss")},
                {"state", e.state},
                {"detail", e.detail}};
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    return result;
}
QVariantList Session::subscriptions() const {
    QVariantList result;
    if (mailboxOpen())
        for (const auto &s : mailbox_.subscriptions())
            result << QVariantMap{{"address", s.address}, {"label", s.label}};
    return result;
}
void Session::subscribe() {
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        bool ok = false;
        auto address =
            addressInput("Subscribe to broadcasts", "Publisher BM-address", &ok).trimmed();
        if (!ok)
            return;
        check(Wire::validAddress(address), "Invalid Bitmessage address");
        auto label = QInputDialog::getText(nullptr, "Subscription label", "Label",
                                           QLineEdit::Normal, {}, &ok);
        if (!ok)
            return;
        mailbox_.subscribe(address, label);
        activity_ = "Subscription saved. Retained broadcasts will be inspected.";
    });
}
void Session::unsubscribe(QString address) {
    attempt([&] {
        check(mailboxOpen(), "Open a mailbox first");
        mailbox_.unsubscribe(address);
    });
}
void Session::renameIdentity(QString address) {
    attempt([&] {
        check(unlocked(), "Unlock a vault first");
        bool ok = false;
        auto label =
            QInputDialog::getText(nullptr, "Rename identity", "Label", QLineEdit::Normal, {}, &ok);
        if (ok)
            vault_.renameIdentity(address, label);
    });
}
void Session::copyAddress(QString address) {
    QApplication::clipboard()->setText(address);
}
void Session::setDefaultIdentity(QString address) {
    attempt([&] {
        check(unlocked(), "Unlock a vault first");
        vault_.setDefaultIdentity(address);
    });
}
void Session::deleteIdentity(QString address) {
    attempt([&] {
        check(unlocked(), "Unlock a vault first");
        if (QMessageBox::question(
                nullptr, "Delete identity permanently?",
                "This permanently removes the private key for this address. Mail already sent "
                "or received stays in your mailbox, but you will no longer be able to send as "
                "this address or read anything newly sent to it.") != QMessageBox::Yes)
            return;
        vault_.deleteIdentity(address);
    });
}
void Session::closeMailbox() {
    emit aboutToCloseMailbox();
    delivery_->stop();
    mailbox_.close();
    mailLock_.reset();
    clearMessages();
    mailPath_.clear();
    mailKey_.clear();
    emit changed();
}
void Session::startNode() {
    if (offline_)
        return;
    QSettings config(root_ + "/desktop.ini", QSettings::IniFormat);
    QStringList args{"--node", "-D", root_, "-m", root_ + "/unused-maildir", "-i"};
    auto peer = config.value("peer").toString(), proxy = config.value("proxy").toString();
    if (!peer.isEmpty())
        args << "-P" << peer << "-L";
    if (!proxy.isEmpty())
        args << "-r" << proxy << "-B";
    QFile::remove(root_ + "/status.json");
    node_.setProgram(QCoreApplication::applicationFilePath());
    node_.setArguments(args);
    node_.start();
}
void Session::restartNode() {
    attempt([&] {
        if (node_.state() != QProcess::NotRunning) {
            node_.terminate();
            if (!node_.waitForFinished(3000)) {
                node_.kill();
                node_.waitForFinished(1000);
            }
        }
        startNode();
    });
}
void Session::setNetworkEnabled(bool enabled) {
    offline_ = !enabled;
    restartNode();
}
static bool endpoint(const QString &text) {
    if (text.isEmpty())
        return true;
    QUrl url("tcp://" + text);
    QHostAddress address;
    return url.isValid() && url.userInfo().isEmpty() && url.path().isEmpty() &&
           url.query().isEmpty() && url.fragment().isEmpty() && address.setAddress(url.host()) &&
           url.port() > 0 && url.port() <= 65535;
}
void Session::configureNode() {
    attempt([&] {
        QSettings config(root_ + "/desktop.ini", QSettings::IniFormat);
        bool ok = false;
        auto peer = QInputDialog::getText(nullptr, "Network settings",
                                          "Additional peer IP:port (empty for automatic discovery)",
                                          QLineEdit::Normal, config.value("peer").toString(), &ok)
                        .trimmed();
        if (!ok)
            return;
        check(endpoint(peer), "Enter an IP address and port, e.g. 192.0.2.1:8444 or [::1]:8444");
        auto proxy =
            QInputDialog::getText(nullptr, "SOCKS5 proxy",
                                  "Proxy IP:port (empty for direct; Tor typically 127.0.0.1:9050)",
                                  QLineEdit::Normal, config.value("proxy").toString(), &ok)
                .trimmed();
        if (!ok)
            return;
        check(endpoint(proxy), "Enter a proxy IP address and port");
        config.setValue("peer", peer);
        config.setValue("proxy", proxy);
        activity_ = "Network settings saved. Restart the node to apply them.";
    });
}
void Session::configureRetention() {
    attempt([&] {
        bool ok = false;
        auto mb = QInputDialog::getInt(nullptr, "Retained network objects",
                                       "Maximum MB of encrypted network objects. Older objects are "
                                       "discarded; saved mailbox letters are preserved.",
                                       retentionMB_, 64, 32768, 64, &ok);
        if (!ok)
            return;
        auto days = QInputDialog::getInt(nullptr, "Retained network objects",
                                         "Keep network objects for at most this many days",
                                         retentionDays_, 1, 3650, 1, &ok);
        if (!ok)
            return;
        retentionMB_ = mb;
        retentionDays_ = days;
        QSettings config(root_ + "/desktop.ini", QSettings::IniFormat);
        config.setValue("retentionMB", mb);
        config.setValue("retentionDays", days);
        activity_ = "Retention settings saved";
    });
}
} // namespace bm
