#pragma once
#include <QPushButton>
#include <QUrl>
#include <QVariant>
#include <QWidget>
class QFrame;
class QHBoxLayout;
class QLabel;
class QPlainTextEdit;
class QVBoxLayout;
class QScrollArea;
namespace bm {
// A conversation is shown a window at a time: its newest messages, and a page more of the
// older ones each time the reader scrolls up to the top of what is shown. A page is what
// comes in within one frame (about 10 ms for 25; tools/chat_scroll_bench measures it).
inline constexpr int kChatWindow = 100;
inline constexpr int kChatPage = 25;
// How far a sent message has got, as the tick beside its time shows it.
enum class ChatTick { None, Pending, Sent, Delivered, Failed };
ChatTick chatTickFor(const QString &deliveryState);
// One chat in the chat rail, as in a chat list: who, the newest message and when,
// how far it has got if it is ours, and how many are unread. A chat is between a peer
// and one of my identities; with several identities the row says which, by a small
// picture of it on the peer's. Collapsed, only the pictures, with the unread count
// on the corner.
class ChatRow : public QPushButton {
    Q_OBJECT
  public:
    ChatRow(const QString &address, const QString &name, QWidget *parent = nullptr);
    void setSummary(const QString &body, qint64 received, ChatTick tick, int unread);
    // The identity the chat is under, shown when there are several; empty to hide it.
    void setVia(const QString &address, const QString &name);
    void setCollapsed(bool collapsed);
    QString preview() const {
        return preview_;
    }
    qint64 received() const {
        return received_;
    }
    ChatTick tick() const {
        return tick_;
    }
    int unread() const {
        return unread_;
    }
    QString via() const {
        return via_;
    }
    bool collapsed() const {
        return collapsed_;
    }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    void updateTip();
    QString address_, name_, body_, preview_, via_, viaName_;
    qint64 received_ = 0;
    ChatTick tick_ = ChatTick::None;
    int unread_ = 0;
    bool collapsed_ = false;
};
// The open chat: its peer's name, the thread, and the composer. Navigation belongs to
// DesktopWindow's shared rail.
class ChatsPane : public QWidget {
    Q_OBJECT
  public:
    explicit ChatsPane(QWidget *parent = nullptr);
    // A chat is between one of my identities and a peer, and keeps that identity for as
    // long as it lasts: another identity is another chat. So the identity is only shown,
    // and only when there are several (showIdentity), never chosen here. Without a peer
    // there is no chat, and no composer.
    void setConversation(const QString &identity, const QString &identityLabel, const QString &peer,
                         const QString &peerLabel, bool showIdentity);
    // The conversation's newest messages, oldest first: a window of it. hasOlder says that
    // more are kept further back, which olderRequested() asks for as the reader nears the top.
    void setMessages(const QVariantList &messages, bool hasOlder = false);
    void clearComposer();
    void focusComposer();
    QString contact() const;  // the peer
    QString identity() const; // mine
    void setDark(bool dark);

  signals:
    void sendRequested(const QString &from, const QString &body);
    // The reader is near the oldest message shown, and older ones are kept.
    void olderRequested();
    // A link in a message was clicked. The window decides whether it opens.
    void linkClicked(const QUrl &url);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    // One message's bubble, with the parts that change in place.
    struct Row {
        QString id;
        QVariantMap data;
        QWidget *widget = nullptr; // the bubble and the space beside it
        QWidget *bubble = nullptr;
        QHBoxLayout *footer = nullptr;
        QLabel *tick = nullptr; // how far a sent message has got; none for one received
    };
    void applyTheme();
    void fitComposer();
    void updateSend();
    void showFocus(bool focused);
    Row makeRow(const QVariantMap &item);
    void setTick(Row &row);
    void insertRows(int index, const QVariantList &items);
    void rebuild(const QVariantList &messages);
    void checkOlder();
    QLabel *title_, *address_, *avatar_, *welcomeTitle_, *identityIcon_, *identityName_;
    QFrame *identityChip_, *card_;
    QWidget *composeBar_;
    QPlainTextEdit *input_;
    QScrollArea *scroll_;
    QVBoxLayout *messages_;
    QPushButton *send_;
    QString contact_, identity_;
    bool dark_ = false;
    // What the bubbles show now: setMessages() changes only what differs from this.
    QVariantList shown_;
    QList<Row> rows_; // the bubbles, in the order of shown_
    QString shownContact_, shownIdentity_;
    bool shownDark_ = false;
    // The reader is at the newest message: new ones scroll into view.
    bool atEnd_ = true;
    // Older messages are kept; they were asked for and have not come; a check is waiting.
    bool hasOlder_ = false, askedOlder_ = false, checkQueued_ = false;
};
} // namespace bm
