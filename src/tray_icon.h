#pragma once
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QObject>
#include <optional>
class QAction;
class QMenu;
class QSystemTrayIcon;
namespace bm {
// The mark in the notification area: the app's envelope, Y fold and shield in one colour, so a
// platform can tint it for its bar (macOS) or the app can pick white or dark for it (Windows,
// Linux). It is drawn, not shipped as pictures: whatever size a platform asks for is crisp, and
// the small sizes keep less detail than the large ones, as the logo's own optical sizes do.
// The sizes the icon offers: 16 to 24 for a bar at 100%, up to 96 for a Retina menu bar, which
// asks for twice its height and scales it down.
inline constexpr int kTraySizes[] = {16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 56, 64, 72, 96};
// GNOME's AppIndicator extension starts a "launching" sequence (a busy cursor over the panel and
// dock) with each click in the icon's menu, and hands the app its id as an activation token. Qt
// keeps the token in XDG_ACTIVATION_TOKEN for a window to use, and only Wayland windows do: on
// X11 the sequence is never ended and the busy cursor stays until it times out. This ends it, as a
// launcher's app does when it is up: the startup-notification "remove" message for `id`, in the
// 20-byte pieces that go in a client message (the first piece is the one that begins the message).
QList<QByteArray> startupRemoveMessage(const QByteArray &id);
// Ends the sequence the last click began, if there is one and this is X11; else does nothing.
void finishStartupSequence();
QImage trayGlyphImage(int pixels, const QColor &color); // pixels x pixels, clear around the mark
QIcon trayGlyph(const QColor &color);                   // one picture for each of kTraySizes

// The glyph's colour is the platform's to say. macOS tints a template image itself, for a light or
// a dark menu bar. Windows draws the icon as it is on a taskbar that is light or dark, which is
// its own setting, not the apps'. Linux panels differ: GNOME's is dark whatever the apps wear, the
// others tend to follow the system's colour scheme. (Not the app's own theme: the window may be
// set to light under a dark desktop, and the panel stays dark.)
enum class TrayPlatform { Mac, Windows, Linux };
struct TrayEnvironment {
    bool lightTaskbar = false; // Windows: SystemUsesLightTheme
    QString desktop;           // Linux: XDG_CURRENT_DESKTOP, "ubuntu:GNOME"
    bool darkScheme = false;   // Linux: the system's colour scheme is dark
};
struct TrayTone {
    QColor color;
    bool mask = false; // a template image: the system chooses the colour at draw time
};
TrayTone trayTone(TrayPlatform platform, const TrayEnvironment &environment);
TrayPlatform thisPlatform();
TrayEnvironment currentTrayEnvironment(); // what this machine says now

// The app's icon in the notification area, with the menu that is all of its controls: Show/Hide,
// New letter, New chat, Settings, Quit. It only says what was asked; the window does it.
class TrayIcon : public QObject {
    Q_OBJECT
  public:
    // The menu's words, translated by the window that owns the icon.
    struct Labels {
        QString show, hide, newLetter, newChat, settings, quit;
    };
    explicit TrayIcon(Labels labels, QObject *parent = nullptr);
    ~TrayIcon() override;
    // Whether an icon is really showing. Only then may a closed window hide into it: with no tray
    // (stock GNOME) it would have no way back.
    bool active() const;
    void setWindowShown(bool shown); // the first item reads Hide while it is, Show while it is not
    void setMailboxOpen(bool open);  // writing a letter or starting a chat needs one
    QMenu *menu() const { return menu_; }
    QSystemTrayIcon *icon() const { return icon_; }
    // For tests, which run where there is no tray: take one as there, or not there.
    static void setAvailableForTesting(std::optional<bool> available);
  signals:
    // The icon was clicked: bring the window forward. Windows and Linux; on macOS a click opens the
    // menu and does only that, and Show is in the menu.
    void showRequested();
    void showHideRequested(); // the menu's first item
    void newLetterRequested();
    void newChatRequested();
    void settingsRequested();
    void quitRequested();

  private:
    void refreshIcon(); // in the colour the platform wants now
    Labels labels_;
    QSystemTrayIcon *icon_;
    QMenu *menu_;
    QAction *showHide_, *newLetter_, *newChat_;
};
} // namespace bm
