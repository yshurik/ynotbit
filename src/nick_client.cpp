#include "nick_client.h"
#include "coolname.h"
#include "qt_bridge.h"
#include "session.h"
#include "updates.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QRandomGenerator>
#include <QSet>
#include <algorithm>
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/nickbot.h>
#include <bmc/records.h>
#include <bmc/wire.h>
#include <exception>

namespace bm {
namespace {
namespace nb = bmc::nickbot;
// Seconds. A lookup the user waits for goes out soon; one that only keeps the book up to date goes
// out some time in the next ten minutes, and never close behind another or a message with the
// user's name on it (docs/nickbot.md, "Privacy").
constexpr int kUrgentDelay[2] = {3, 30};
constexpr int kBackgroundDelay[2] = {60, 600};
constexpr qint64 kGapBetweenBackground = 120;
constexpr qint64 kAfterIdentified = 120;
constexpr qint64 kAnswerWait = nb::kLookupTtlSeconds + 300;
constexpr qint64 kRegistrationWait = bmc::kChatTtlSeconds;
constexpr qint64 kRecheckAfter = 7 * 86400;
constexpr qint64 kAutoEvery = 600;
constexpr int kServiceBatch = 50;

const QString kListSetting = "nickbots:list", kEnabledSetting = "nickbots:enabled",
              kAutoSetting = "nickbots:auto", kRegistrationSetting = "nickreg:",
              kVouchedBySetting = "nickbot:", kCheckedSetting = "nickchecked:";
// What the vault calls the chan lookups go out from. The label is how the app knows its own chan
// from one the user joined with the same phrase.
const char *const kLaneLabel = "nickbot lookups";

QString q(const std::string &s) {
    return bridge::fromStd(s);
}
std::string st(const QString &s) {
    return bridge::toStd(s);
}
// The vault's identity at the lane's address, whoever made it, or null.
const bmc::Identity *laneIdentity(const bmc::Vault &vault) {
    for (const auto &i : vault.identities())
        if (i.address == nb::kAnonymousAddress)
            return &i;
    return nullptr;
}
bool madeByTheApp(const bmc::Identity &i) {
    return i.chan && i.label == bmc::channelName(kLaneLabel);
}
} // namespace

QColor nickbotColor(int index) {
    static const QColor colors[] = {"#4a8fd6", "#3fa46a", "#c98a1e", "#9a63c9", "#d0584f"};
    return colors[qBound(0, index, 4)];
}

ShownName shownName(const QString &address, const QString &local, const NickInfo &nick) {
    ShownName shown;
    shown.nick = nick.text;
    shown.color = nick.color;
    shown.bot = nick.bot;
    // A nickname says more than a name made up from the address; and one that repeats it says
    // nothing new.
    const bool generated = local.isEmpty() || local == address || local == addressTitle(address);
    shown.local = nick.text.isEmpty()                 ? local
                  : (generated || local == nick.text) ? QString()
                                                      : local;
    return shown;
}
QString ShownName::plain() const {
    return nick.isEmpty() ? local : local.isEmpty() ? nick : nick + " · " + local;
}
QString ShownName::html() const {
    QString out;
    if (!nick.isEmpty()) {
        const auto text = nick.toHtmlEscaped();
        out = color >= 0
                  ? "<span style=\"color:" + nickbotColor(color).name() + "\">" + text + "</span>"
                  : text;
    }
    if (!local.isEmpty())
        out += (out.isEmpty() ? QString() : QString(" · ")) + local.toHtmlEscaped();
    return out;
}

NickClient::NickClient(Session &session) : QObject(&session), session_(session) {}

qint64 NickClient::now() const {
    return clock_ ? clock_() : QDateTime::currentSecsSinceEpoch();
}
void NickClient::setClockForTesting(std::function<qint64()> now) {
    clock_ = std::move(now);
}
void NickClient::setDelayForTesting(std::function<int(int, int)> pick) {
    pick_ = std::move(pick);
}
int NickClient::delay(bool urgent) const {
    const auto *range = urgent ? kUrgentDelay : kBackgroundDelay;
    return pick_ ? pick_(range[0], range[1])
                 : QRandomGenerator::global()->bounded(range[0], range[1] + 1);
}
QString NickClient::setting(const QString &name) const {
    return session_.mailboxOpen() ? q(session_.coreMailbox().setting(st(name))) : QString();
}
void NickClient::setSetting(const QString &name, const QString &value) {
    if (session_.mailboxOpen())
        session_.coreMailbox().setSetting(st(name), st(value));
}

// --- the list ---------------------------------------------------------------------------------

QList<Nickbot> NickClient::bots() const {
    QList<Nickbot> list;
    const auto text = setting(kListSetting);
    if (text.isEmpty()) {
        for (const auto &d : updates::defaultNickbots())
            list.push_back({d.address, d.label});
        return list;
    }
    const auto doc = QJsonDocument::fromJson(text.toUtf8());
    for (const auto &v : doc.array()) {
        const auto o = v.toObject();
        list.push_back({o["address"].toString(), o["label"].toString()});
    }
    return list;
}

bool NickClient::setBots(const QList<Nickbot> &bots, QString *problem) {
    const auto refuse = [&](const QString &why) {
        if (problem)
            *problem = why;
        return false;
    };
    if (!session_.mailboxOpen())
        return refuse(tr("Open a mailbox first"));
    if (bots.size() > bmc::nickbot::kMaxBots)
        return refuse(tr("Up to five nickbots can be listed"));
    QSet<QString> seen;
    QJsonArray array;
    for (const auto &b : bots) {
        const auto address = b.address.trimmed();
        if (!bmc::Wire::validAddress(st(address)))
            return refuse(tr("That is not a valid Bitmessage address"));
        if (seen.contains(address))
            return refuse(tr("That nickbot is listed twice"));
        seen << address;
        for (const auto &i : session_.coreVault().identities())
            if (q(i.address) == address)
                return refuse(tr("That is one of your own addresses"));
        array.push_back(QJsonObject{{"address", address}, {"label", b.label.trimmed()}});
    }
    // An empty list is a list: the user has no nickbot, which is not "never set".
    setSetting(kListSetting,
               bots.isEmpty()
                   ? QString("[]")
                   : QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    emit changed();
    return true;
}

int NickClient::botIndex(const QString &address) const {
    const auto list = bots();
    for (int i = 0; i < list.size(); ++i)
        if (list[i].address == address)
            return i;
    return -1;
}

bool NickClient::enabled() const {
    return setting(kEnabledSetting) != "0";
}
void NickClient::setEnabled(bool on) {
    setSetting(kEnabledSetting, on ? "1" : "0");
    emit changed();
}
bool NickClient::autoLookups() const {
    return setting(kAutoSetting) != "0";
}
void NickClient::setAutoLookups(bool on) {
    setSetting(kAutoSetting, on ? "1" : "0");
    emit changed();
}

// --- the lane ---------------------------------------------------------------------------------

// The lane is a chan of the vault, not a setting of one mailbox: a vault has many mailboxes, and
// each of them has to find the one chan. It is the app's when it has the label the app gave it; the
// same address under another label was joined by the user.
QString NickClient::laneAddress() const {
    const auto *lane = laneIdentity(session_.coreVault());
    return lane && madeByTheApp(*lane) ? q(lane->address) : QString();
}

bool NickClient::laneIsTheUsers() const {
    const auto *lane = laneIdentity(session_.coreVault());
    return lane && !madeByTheApp(*lane);
}

QString NickClient::ensureLane(QString *problem) {
    const auto fail = [&](const QString &why) {
        if (problem)
            *problem = why;
        return QString();
    };
    if (!session_.mailboxOpen() || !session_.unlocked())
        return fail(tr("Open a mailbox first"));
    if (const auto lane = laneAddress(); !lane.isEmpty())
        return lane;
    if (laneIsTheUsers())
        return fail(
            tr("You have joined the chan “anonymous” yourself, so nicknames cannot be looked up "
               "through it. Leave it first."));
    const auto lane = q(std::string(nb::kAnonymousAddress));
    try {
        session_.coreVault().addChannel(std::string(nb::kAnonymousPhrase), kLaneLabel, st(lane));
    } catch (const std::exception &e) {
        return fail(Session::errorText(e));
    }
    setLaneInDelivery();
    return lane;
}

void NickClient::setLaneInDelivery() {
    std::set<std::string> addresses;
    if (const auto lane = laneAddress(); !lane.isEmpty())
        addresses.insert(st(lane));
    session_.coreClient().delivery().setServiceAddresses(std::move(addresses));
}

// --- asking -----------------------------------------------------------------------------------

// Whether a lookup to this nickbot (empty: the default) can be made; if so, *resolved is its
// address.
QString NickClient::problemWith(const QString &bot, QString *resolved) {
    if (!session_.mailboxOpen())
        return tr("Open a mailbox first");
    if (!enabled())
        return tr("Nicknames are turned off in Settings");
    const auto list = bots();
    if (list.isEmpty())
        return tr("No nickbot is listed");
    if (bot.isEmpty()) {
        *resolved = list.first().address;
    } else if (botIndex(bot) < 0) {
        return tr("That nickbot is not in your list");
    } else {
        *resolved = bot;
    }
    QString problem;
    if (ensureLane(&problem).isEmpty())
        return problem;
    return {};
}

QString NickClient::ask(Pending pending) {
    for (const auto *list : {&queue_, &waiting_})
        for (const auto &p : *list)
            if (p.bot == pending.bot && p.key == pending.key)
                return {}; // the same question is already out
    pending.due = now() + delay(pending.urgent);
    queue_.push_back(std::move(pending));
    return {};
}

QString NickClient::lookupAddress(const QString &text, const QString &bot) {
    const auto n = nb::parseNick(st(text));
    if (!n)
        return tr(
            "That is not a valid nickname. A nickname is @, / or # followed by a name of 3 to "
            "32 characters: a letter, then letters, digits, _ or -.");
    QString resolved;
    if (const auto problem = problemWith(bot, &resolved); !problem.isEmpty())
        return problem;
    return ask({resolved,
                "address",
                q(nb::keyOfAddressRequest(*n)),
                q(nb::addressRequest(*n)),
                {},
                0,
                0,
                true});
}

QString NickClient::lookupNicknames(const QStringList &addresses, const QString &bot, bool urgent) {
    if (addresses.isEmpty())
        return tr("Enter an address");
    std::vector<std::string> asked;
    for (const auto &a : addresses) {
        if (!bmc::Wire::validAddress(st(a.trimmed())))
            return tr("That is not a valid Bitmessage address");
        asked.push_back(st(a.trimmed()));
    }
    QString resolved;
    if (const auto problem = problemWith(bot, &resolved); !problem.isEmpty())
        return problem;
    for (const auto &batch : nb::nicknameBatches(asked)) {
        QStringList list;
        for (const auto &a : batch)
            list << q(a);
        const auto problem = ask({resolved, "nickname", q(nb::keyOfNicknameRequest(batch)),
                                  q(nb::nicknameRequest(batch)), list, 0, 0, urgent});
        if (!problem.isEmpty())
            return problem;
    }
    return {};
}

QString NickClient::search(const QString &text, const QString &bot) {
    nb::Term term;
    switch (nb::parseTerm(st(text.trimmed()), &term)) {
    case nb::TermResult::TooShort:
        return tr("Search for at least %1 letters, digits, _ or -").arg(nb::kMinTerm);
    case nb::TermResult::Syntax:
        return tr("A search term is letters, digits, _ and - only");
    case nb::TermResult::Ok:
        break;
    }
    QString resolved;
    if (const auto problem = problemWith(bot, &resolved); !problem.isEmpty())
        return problem;
    return ask({resolved,
                "search",
                q(nb::keyOfSearchRequest(term)),
                q(nb::searchRequest(term)),
                {},
                0,
                0,
                true});
}

void NickClient::finish(const Pending &p, const QVariantMap &result) {
    emit lookupFinished(p.kind, p.key, p.bot, result);
}

void NickClient::sendDue() {
    const auto lane = laneAddress();
    if (queue_.isEmpty() || lane.isEmpty())
        return;
    const auto t = now();
    for (int i = 0; i < queue_.size();) {
        const auto p = queue_[i];
        const bool held =
            t < lastIdentified_.value(p.bot, -kAfterIdentified * 10) + kAfterIdentified;
        const bool crowded =
            !p.urgent &&
            t < lastAnonymous_.value(p.bot, -kGapBetweenBackground * 10) + kGapBetweenBackground;
        if (p.due > t || held || crowded) {
            ++i;
            continue;
        }
        queue_.removeAt(i);
        try {
            session_.coreClient().sendServiceChat(st(lane), st(p.bot), st(p.command),
                                                  nb::kLookupTtlSeconds, bmc::now());
        } catch (const std::exception &e) {
            finish(p, {{"error", "send"}, {"text", Session::errorText(e)}});
            continue;
        }
        auto sent = p;
        sent.sent = t;
        waiting_.push_back(sent);
        lastAnonymous_[p.bot] = t;
    }
}

void NickClient::expire() {
    const auto t = now();
    for (int i = 0; i < waiting_.size();) {
        if (t - waiting_[i].sent > kAnswerWait) {
            const auto p = waiting_.takeAt(i);
            finish(p, {{"error", "timeout"}});
        } else {
            ++i;
        }
    }
}

// --- answers ----------------------------------------------------------------------------------

void NickClient::processService() {
    if (laneAddress().isEmpty())
        return;
    for (const auto &m : session_.coreMailbox().takeService(kServiceBatch)) {
        const auto bot = q(m.from);
        if (botIndex(bot) < 0)
            continue; // anybody can write to the chan: only a listed nickbot is believed
        bmc::ChatPayload payload;
        if (bmc::parseChatMessage(m.subject, m.body, &payload) != bmc::ChatParseResult::Chat)
            continue;
        if (const auto reply = nb::parseTrailer(payload.body))
            apply(*reply, bot);
    }
}

bool NickClient::known(const QString &address) const {
    if (session_.isContact(address))
        return true;
    for (const auto &i : session_.coreVault().identities())
        if (q(i.address) == address)
            return true;
    return false;
}

void NickClient::markChecked(const QString &address) {
    setSetting(kCheckedSetting + address, QString::number(now()));
}

// A nickname set by hand or built in is confirmed by a nickbot, never replaced by one.
bool NickClient::learn(const QString &address, const QString &nick, const QString &bot) {
    if (!known(address))
        return false;
    markChecked(address);
    const auto have = setting(updates::kNicknameSetting + address);
    const auto vouched = setting(kVouchedBySetting + address);
    if (!have.isEmpty() && vouched.isEmpty() && have != nick)
        return false;
    if (have == nick && vouched == bot)
        return false;
    setSetting(updates::kNicknameSetting + address, nick);
    setSetting(kVouchedBySetting + address, bot);
    return true;
}

// The nickbot has no nickname for the address: one it vouched for earlier goes.
bool NickClient::forget(const QString &address, const QString &bot) {
    if (!known(address))
        return false;
    markChecked(address);
    if (setting(kVouchedBySetting + address) != bot)
        return false;
    setSetting(updates::kNicknameSetting + address, {});
    setSetting(kVouchedBySetting + address, {});
    return true;
}

void NickClient::apply(const nb::Reply &reply, const QString &bot) {
    bool changedBook = false;
    QVariantMap result;
    QString kind;
    if (reply.cmd == "address") {
        kind = "address";
        const bool found = reply.found.value_or(false);
        result = {{"found", found}, {"nick", q(reply.nick)}, {"address", q(reply.address)}};
        if (reply.ok && found && !reply.nick.empty() && !reply.address.empty())
            changedBook = learn(q(reply.address), q(reply.nick), bot);
    } else if (reply.cmd == "nickname") {
        kind = "nickname";
        QVariantList list;
        for (const auto &entry : reply.byAddress) {
            QStringList nicks;
            for (const auto &n : entry.nicks)
                nicks << q(n);
            list << QVariantMap{{"address", q(entry.address)}, {"nicks", nicks}};
            changedBook |= nicks.isEmpty() ? forget(q(entry.address), bot)
                                           : learn(q(entry.address), nicks.first(), bot);
        }
        result = {{"results", list}};
    } else if (reply.cmd == "search") {
        kind = "search";
        QVariantList hits;
        for (const auto &h : reply.hits) {
            hits << QVariantMap{{"nick", q(h.nick)}, {"address", q(h.address)}};
            changedBook |= learn(q(h.address), q(h.nick), bot);
        }
        result = {{"hits", hits}, {"more", reply.more}};
    }
    if (!reply.ok && !kind.isEmpty())
        result["error"] = q(reply.error);
    // The question this answers.
    const auto key = nb::keyOfReply(reply);
    int at = -1;
    for (int i = 0; i < waiting_.size() && at < 0; ++i)
        if (waiting_[i].bot == bot && (key ? waiting_[i].key == q(*key) : waiting_[i].kind == kind))
            at = i;
    if (at >= 0 && !kind.isEmpty() && (key || !reply.ok)) {
        const auto p = waiting_.takeAt(at);
        finish(p, result);
    }
    if (changedBook)
        emit changed();
}

// --- registration -----------------------------------------------------------------------------

// The id of the newest chat the nickbot has sent this identity, or empty: what came before a
// registration is not its answer.
static QString newestFromBot(Session &session, const QString &identity, const QString &bot) {
    const auto chats = session.coreMailbox().chatMessages(st(identity), st(bot), 30);
    for (auto it = chats.rbegin(); it != chats.rend(); ++it)
        if (!it->outgoing)
            return q(it->id);
    return {};
}

QString NickClient::registerNickname(const QString &identity, const QString &text,
                                     const QString &bot) {
    if (!session_.mailboxOpen())
        return tr("Open a mailbox first");
    const auto n = nb::parseNick(st(text));
    if (!n)
        return tr(
            "That is not a valid nickname. A nickname is @, / or # followed by a name of 3 to "
            "32 characters: a letter, then letters, digits, _ or -.");
    bool own = false;
    for (const auto &i : session_.coreVault().identities())
        own = own || (!i.chan && q(i.address) == identity);
    if (!own)
        return tr("Choose one of your identities");
    if (!enabled())
        return tr("Nicknames are turned off in Settings");
    const auto list = bots();
    if (list.isEmpty())
        return tr("No nickbot is listed");
    QString resolved = list.first().address;
    if (!bot.isEmpty()) {
        if (botIndex(bot) < 0)
            return tr("That nickbot is not in your list");
        resolved = bot;
    }
    const auto after = newestFromBot(session_, identity, resolved);
    // The client directly, not Session::sendChat: that one is a no-op inside another attempt(), and
    // would leave this looking as if it had worked.
    QString id;
    try {
        id = q(session_.coreClient().sendChat(st(identity), st(resolved), nb::registerRequest(*n),
                                              bmc::now()));
    } catch (const std::exception &e) {
        return Session::errorText(e);
    }
    lastIdentified_[resolved] = now();
    QJsonObject state{{"bot", resolved},      {"nick", q(n->text())}, {"state", "waiting"},
                      {"error", ""},          {"chat", id},           {"after", after},
                      {"sent", double(now())}};
    setSetting(kRegistrationSetting + identity,
               QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact)));
    emit changed();
    return {};
}

QVariantMap NickClient::registration(const QString &identity) const {
    const auto text = setting(kRegistrationSetting + identity);
    if (text.isEmpty())
        return {{"state", "none"}};
    return QJsonDocument::fromJson(text.toUtf8()).object().toVariantMap();
}

void NickClient::registerWhenAMailboxOpens(const QString &identity, const QString &nick) {
    for (auto &r : toRegister_)
        if (r.first == identity) {
            r.second = nick;
            return;
        }
    toRegister_.push_back({identity, nick});
}

QStringList NickClient::waitingForAMailbox() const {
    QStringList identities;
    for (const auto &r : toRegister_)
        identities << r.first;
    return identities;
}

void NickClient::checkRegistrations() {
    if (!session_.mailboxOpen())
        return;
    for (const auto &[name, text] : session_.coreMailbox().settings(st(kRegistrationSetting))) {
        auto state = QJsonDocument::fromJson(q(text).toUtf8()).object();
        if (state["state"].toString() != "waiting")
            continue;
        const auto identity = q(name).mid(kRegistrationSetting.size());
        const auto bot = state["bot"].toString();
        const auto after = state["after"].toString();
        const qint64 sent = qint64(state["sent"].toDouble());
        QString verdict;
        // What the nickbot has said since the request, oldest first; the first word on it is the
        // answer.
        const auto chats = session_.coreMailbox().chatMessages(st(identity), st(bot), 30);
        bool past = after.isEmpty();
        for (const auto &m : chats) {
            if (!past) {
                past = q(m.id) == after;
                continue;
            }
            if (m.outgoing)
                continue;
            const auto reply = nb::parseTrailer(m.body);
            if (!reply || reply->cmd != "register")
                continue;
            if (reply->ok) {
                state["state"] = "registered";
                if (!reply->nick.empty())
                    state["nick"] = q(reply->nick);
                state["error"] = "";
                learn(identity, state["nick"].toString(), bot);
            } else {
                state["state"] = "refused";
                state["error"] = q(reply->error);
            }
            verdict = state["state"].toString();
            break;
        }
        // The id was not in the window: everything in it is newer.
        if (verdict.isEmpty() && !past) {
            for (const auto &m : chats) {
                const auto reply = m.outgoing ? std::nullopt : nb::parseTrailer(m.body);
                if (!reply || reply->cmd != "register")
                    continue;
                state["state"] = reply->ok ? "registered" : "refused";
                state["error"] = reply->ok ? "" : q(reply->error);
                if (reply->ok && !reply->nick.empty())
                    state["nick"] = q(reply->nick);
                if (reply->ok)
                    learn(identity, state["nick"].toString(), bot);
                verdict = state["state"].toString();
                break;
            }
        }
        if (verdict.isEmpty() && now() - sent > kRegistrationWait) {
            state["state"] = "noanswer";
            verdict = "noanswer";
        }
        if (!verdict.isEmpty()) {
            setSetting(q(name),
                       QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact)));
            emit changed();
        }
    }
}

// --- keeping the book up to date --------------------------------------------------------------

void NickClient::autoRefresh() {
    const auto t = now();
    if (!backgroundLookups_ || t < nextAuto_ || !autoLookups() || !enabled() || bots().isEmpty())
        return;
    nextAuto_ = t + kAutoEvery;
    // One batch of the background at a time.
    for (const auto *list : {&queue_, &waiting_})
        for (const auto &p : *list)
            if (!p.urgent && p.kind == "nickname")
                return;
    QHash<QString, qint64> checked;
    for (const auto &[name, value] : session_.coreMailbox().settings(st(kCheckedSetting)))
        checked[q(name).mid(kCheckedSetting.size())] = q(value).toLongLong();
    QList<QPair<qint64, QString>> stale;
    for (const auto &c : session_.coreMailbox().contacts()) {
        const auto address = q(c.address);
        const auto last = checked.value(address, 0);
        if (t - last >= kRecheckAfter)
            stale.push_back({last, address});
    }
    if (stale.isEmpty())
        return;
    std::sort(stale.begin(), stale.end());
    QStringList batch;
    for (const auto &entry : stale.mid(0, nb::kBatch))
        batch << entry.second;
    lookupNicknames(batch, {}, false);
}

// --- nicknames on the screen ------------------------------------------------------------------

QHash<QString, NickInfo> NickClient::nicknames() const {
    QHash<QString, NickInfo> all;
    if (!session_.mailboxOpen())
        return all;
    QHash<QString, QString> vouched;
    for (const auto &[name, value] : session_.coreMailbox().settings(st(kVouchedBySetting)))
        vouched[q(name).mid(kVouchedBySetting.size())] = q(value);
    const auto list = bots();
    for (const auto &[name, value] :
         session_.coreMailbox().settings(st(updates::kNicknameSetting))) {
        if (value.empty())
            continue;
        NickInfo info;
        info.text = q(value);
        const auto address = q(name).mid(updates::kNicknameSetting.size());
        const auto bot = vouched.value(address);
        for (int i = 0; i < list.size() && !bot.isEmpty(); ++i)
            if (list[i].address == bot) {
                info.color = i;
                info.bot = list[i].label.isEmpty() ? bot : list[i].label;
            }
        all.insert(address, info);
    }
    return all;
}

void NickClient::adopt(const QString &address, const QString &nick, const QString &bot) {
    if (learn(address, nick, bot))
        emit changed();
}

NickInfo NickClient::nicknameOf(const QString &address) const {
    NickInfo info;
    info.text = setting(updates::kNicknameSetting + address);
    if (info.text.isEmpty())
        return info;
    const auto bot = setting(kVouchedBySetting + address);
    const auto list = bots();
    for (int i = 0; i < list.size() && !bot.isEmpty(); ++i)
        if (list[i].address == bot) {
            info.color = i;
            info.bot = list[i].label.isEmpty() ? bot : list[i].label;
        }
    return info;
}

// --- words ------------------------------------------------------------------------------------

QString NickClient::suggestNick(const QString &name) {
    if (name.startsWith("BM-"))
        return {}; // an address is no name
    QString out;
    for (const auto c : name.toLower()) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (plain)
            out += c;
        else if (!out.endsWith('_'))
            out += '_';
    }
    while (!out.isEmpty() && !(out[0] >= 'a' && out[0] <= 'z'))
        out.remove(0, 1);
    out = out.left(nb::kMaxName);
    while (out.endsWith('_') || out.endsWith('-'))
        out.chop(1);
    return out.size() < nb::kMinName ? QString() : "@" + out;
}

QString NickClient::errorText(const QString &code) {
    if (code == "taken")
        return tr("That nickname is taken");
    if (code == "reserved")
        return tr("That nickname is reserved");
    if (code == "syntax")
        return tr("That is not a valid nickname");
    if (code == "rate_limited")
        return tr("Too many changes lately; try again tomorrow");
    if (code == "busy")
        return tr("The nickbot is busy; try again later");
    if (code == "static")
        return tr("That nickname is permanent");
    if (code == "not_chan")
        return tr("A chan's nickname can only be registered from the chan itself");
    if (code == "not_yours")
        return tr("That nickname belongs to another address");
    if (code == "timeout")
        return tr("No answer");
    return tr("The nickbot refused (%1)").arg(code);
}

QString NickClient::withoutTrailer(const QString &text) {
    if (!nb::parseTrailer(st(text)))
        return text;
    auto end = text.size();
    while (end > 0 && text[end - 1].isSpace())
        --end;
    const auto cut = text.lastIndexOf('\n', end - 1);
    return cut < 0 ? QString() : text.left(cut).trimmed();
}

QString NickClient::describe(const QVariantMap &registration) {
    const auto state = registration["state"].toString();
    const auto nick = registration["nick"].toString();
    if (state == "waiting")
        return tr("Asked for %1; waiting for the nickbot").arg(nick);
    if (state == "registered")
        return nick;
    if (state == "refused")
        return tr("%1: %2").arg(nick, errorText(registration["error"].toString()));
    if (state == "noanswer")
        return tr("%1: the nickbot did not answer").arg(nick);
    return tr("No nickname");
}

// --- the session's calls ----------------------------------------------------------------------

void NickClient::tick() {
    if (!session_.mailboxOpen())
        return;
    if (flushRegistrations_) {
        flushRegistrations_ = false;
        const auto pending = toRegister_;
        toRegister_.clear();
        for (const auto &[identity, nick] : pending)
            registerNickname(identity, nick);
    }
    processService();
    expire();
    sendDue();
    checkRegistrations();
    autoRefresh();
}

void NickClient::mailboxOpened() {
    queue_.clear();
    waiting_.clear();
    lastAnonymous_.clear();
    lastIdentified_.clear();
    nextAuto_ = now() + 60;
    setLaneInDelivery();
    // Opening a mailbox happens inside Session::attempt, which refuses a nested one, and sending a
    // chat is one: the registrations asked for before the mailbox existed go on the next tick.
    flushRegistrations_ = !toRegister_.isEmpty();
}

void NickClient::mailboxClosing() {
    queue_.clear();
    waiting_.clear();
}
} // namespace bm
