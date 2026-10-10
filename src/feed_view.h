#pragma once
// The Subscriptions page's feed: one sender's broadcasts as a column of
// cards, newest on top, loaded a page at a time as it scrolls.
#include <QDateTime>
#include <QHash>
#include <QVariantMap>
#include <QWidget>
class QLabel;
class QScrollArea;
class QTimer;
class QToolButton;
class QVBoxLayout;
namespace bm {
class Session;
// "just now", "5 min", "3 h", "Yesterday", then a date: how long ago a post
// arrived, as a feed shows it.
QString relativeTime(const QDateTime &when, const QDateTime &now);
// A button for a group of actions, as on a feed card: an 18 px icon.
QToolButton *actionButton(const QString &name, const QString &icon, const QString &tip, bool dark);
// Draws a group's buttons as one control, as on a feed card: one border round
// them all, a divider between neighbours, the group's corners rounded. The
// group is styled by its object name.
void styleActionGroup(QWidget *group, bool dark);
// A button's place in its group's grid, which its dividers and corners follow.
void placeInActionGroup(QToolButton *button, int row, int column, int rows, int columns);
class FeedView : public QWidget {
    Q_OBJECT
  public:
    explicit FeedView(Session &session, QWidget *parent = nullptr);
    // Shows this sender's posts from the top; an empty address clears the feed.
    void setSource(const QString &address, const QString &label);
    QString source() const {
        return address_;
    }
    QString label() const {
        return label_;
    }
    void setDark(bool dark);
    static constexpr int kPageSize = 20;
    // A card on screen this long (at least half of it) counts as read.
    static constexpr int kReadAfterMs = 1000;

  public slots:
    // Mail changed: new posts go on top (the view stays on what is being
    // read), archived or deleted ones leave, read marks catch up.
    void refresh();

  signals:
    void replyRequested(const QVariantMap &letter);
    void forwardRequested(const QVariantMap &letter);
    void openRequested(const QVariantMap &letter);
    void addContactRequested(const QString &address);

  private:
    QWidget *makeCard(const QVariantMap &letter);
    void setUnread(QWidget *card, bool unread);
    void loadMore();
    void fillViewport();
    void trackReading();
    void updateHeader();
    void clearCards();
    QWidget *cardFor(const QString &hash) const;

    Session &session_;
    QString address_, label_;
    bool dark_ = false, hasMore_ = false;
    QLabel *headerIcon_, *headerName_, *headerAddress_, *empty_;
    QToolButton *headerCopy_;
    QScrollArea *scroll_;
    QWidget *cardsHolder_;
    QVBoxLayout *cards_;
    QTimer *readTimer_;
    QHash<QString, QDateTime> seenSince_; // unread cards on screen, since when
};
} // namespace bm
