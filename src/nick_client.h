#pragma once
// The app's side of the nicknames service (docs/nickbot.md): the nickbots a mailbox lists, the
// hidden chan that lookups are sent from, the lookups themselves (slow, queued, paced, jittered),
// the registration of an identity's nickname, and the nicknames the address book keeps.
#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>

namespace bmc::nickbot {
struct Reply;
}
namespace bm {
class Session;

// The colour of the nth nickbot of a mailbox's list (0 is the default one): the colours of the
// first four quote levels of a letter (letter_document.cpp, which the test compares them with), and
// a fifth. A nickname is shown in it, so that it tells which nickbot said it.
QColor nickbotColor(int index);

// A nickbot a mailbox lists. The first of the list is the default; a nickbot's colour is its place.
struct Nickbot {
    QString address, label;
};

// What a nickname on the screen needs.
struct NickInfo {
    QString text; // "@ben"
    int color =
        -1;      // the place of the nickbot that said it in the list; -1: none, or no longer listed
    QString bot; // that nickbot's label, or empty
};

// The name of an address as the screen shows it: what the user calls it, and what it calls itself.
struct ShownName {
    QString local; // the user's name for it; empty when the nickname stands in for a generated one
    QString nick;
    int color = -1; // the nickname's colour: the place of the nickbot that vouches for it
    QString bot;
    bool empty() const {
        return local.isEmpty() && nick.isEmpty();
    }
    // "@ben · Ben Franklin", for places that show text.
    QString plain() const;
    // The same with the nickname in its colour, escaped, for places that show HTML.
    QString html() const;
};
// local is what the user's address book (or the vault, or a subscription) calls the address, or
// empty. A local name that is only the address's generated title gives way to the nickname.
ShownName shownName(const QString &address, const QString &local, const NickInfo &nick);

class NickClient : public QObject {
    Q_OBJECT
  public:
    explicit NickClient(Session &session);

    // --- the list of nickbots (settings of the open mailbox) -------------------------------------
    // The mailbox's list, or the built-in one if it has never been set.
    QList<Nickbot> bots() const;
    // At most five, each a valid address, none twice, none of the user's own. On false, *problem
    // says why (already translated) and nothing is changed.
    bool setBots(const QList<Nickbot> &bots, QString *problem = nullptr);
    int botIndex(const QString &address) const; // -1 if it is not listed
    // Every message to a nickbot is off when this is. On by default.
    bool enabled() const;
    void setEnabled(bool on);

    // --- the chan the lookups go out from --------------------------------------------------------
    // Its address once the app has joined it to the vault (so in every mailbox of it), else empty.
    QString laneAddress() const;
    // The user joined the chan by hand: the app leaves it alone and so cannot look anything up.
    bool laneIsTheUsers() const;
    // Joins it if need be. Empty, with *problem, if that cannot be done.
    QString ensureLane(QString *problem = nullptr);

    // --- lookups: each returns empty when queued, else why not (translated) ----------------------
    // The answers come as lookupFinished, minutes later.
    QString lookupAddress(const QString &nick, const QString &bot = {});
    QString lookupNicknames(const QStringList &addresses, const QString &bot = {});
    QString search(const QString &term, const QString &bot = {});

    // --- checking the address book: only when the user asks -------------------------------------
    // Asks the default nickbot for the nicknames of every contact, eight to a message and the
    // messages ten seconds apart. Returns how many contacts it asks about; 0, with *problem
    // (already translated), when it cannot: nickbots off or none listed, no contacts, or a check
    // that is under way. The answers come as the other lookups' do.
    int checkContacts(QString *problem = nullptr);
    // A check was asked for and some of its messages are not answered yet (or timed out). The
    // client says changed() when that stops being so.
    bool checking() const;

    // --- registration ----------------------------------------------------------------------------
    // Why no nickname can be asked for now (translated), or empty: no mailbox, nickbots off, or
    // none listed.
    QString registrationProblem() const;
    // Asks the nickbot, by a chat from the identity, to give the nickname to it.
    QString registerNickname(const QString &identity, const QString &nick, const QString &bot = {});
    // state: none, waiting, registered, refused, noanswer; with nick, bot and error (a code).
    QVariantMap registration(const QString &identity) const;
    // A registration to make when the next mailbox opens: a new vault has no mailbox yet.
    void registerWhenAMailboxOpens(const QString &identity, const QString &nick);
    // The identities that wait for that.
    QStringList waitingForAMailbox() const;

    // --- what the session drives -----------------------------------------------------------------
    void tick();
    void mailboxOpened();
    void mailboxClosing();

    // A contact (or an identity) goes by this nickname, said by this nickbot: what the Find people
    // dialog does for the ones it adds. A nickname set by hand stays unless it is the same.
    void adopt(const QString &address, const QString &nick, const QString &bot);

    // --- nicknames on the screen -----------------------------------------------------------------
    // Every address with a nickname.
    QHash<QString, NickInfo> nicknames() const;
    NickInfo nicknameOf(const QString &address) const;

    // A nickname made from a name: "Quick Otter of Zeta" gives "@quick_otter_of_zeta". Empty when
    // none can be made (no letters, or fewer than three characters).
    static QString suggestNick(const QString &name);
    // The words for an error code of a nickbot, in the user's language.
    static QString errorText(const QString &code);
    // A registration, as registration() gives it, in words.
    static QString describe(const QVariantMap &registration);
    // What a nickbot said, without the last line that is for programs: what a person reads. The
    // text as it is when that line is not a good one.
    static QString withoutTrailer(const QString &text);

    // Tests move time.
    void setClockForTesting(std::function<qint64()> now);

  signals:
    // kind: address, nickname or search. result: for address {found, nick, address}, for nickname
    // {results: [{address, nicks}]}, for search {hits: [{nick, address}], more}; any of them
    // {error} with a code, or "timeout", when it failed.
    void lookupFinished(QString kind, QString key, QString bot, QVariantMap result);
    // The list, a registration or a contact's nickname changed.
    void changed();

  private:
    // A lookup waiting to be sent, or sent and waiting for its answer.
    struct Pending {
        QString bot, kind, key, command;
        QStringList addresses; // for a nickname lookup
        qint64 due = 0, sent = 0;
        bool check = false; // one of the messages of a check of the whole address book
    };
    Session &session_;
    QList<Pending> queue_, waiting_;
    QList<QPair<QString, QString>> toRegister_; // identity, nickname
    bool flushRegistrations_ = false;
    std::function<qint64()> clock_;

    qint64 now() const;
    QString problemWith(const QString &bot, QString *resolved);
    // Queues a question: it goes at the next tick, or `later` seconds after now.
    QString ask(Pending pending, int later = 0);
    void sendDue();
    void expire();
    void processService();
    void apply(const bmc::nickbot::Reply &reply, const QString &bot);
    bool learn(const QString &address, const QString &nick, const QString &bot);
    bool forget(const QString &address, const QString &bot);
    bool known(const QString &address) const;
    void checkRegistrations();
    void setLaneInDelivery();
    QString setting(const QString &name) const;
    void setSetting(const QString &name, const QString &value);
    void finish(const Pending &p, const QVariantMap &result);
};
} // namespace bm
