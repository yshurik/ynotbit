#include "tray_icon.h"
#include "mac_integration.h"
#include <QGuiApplication>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <algorithm>
#include <cmath>

namespace bm {
namespace {
// The shield of the app logo (in its 1024 px master it spans 394..630 by 430..715), the peak at
// (centre, top), `width` wide.
QPainterPath shieldPath(qreal centre, qreal top, qreal width) {
    const qreal scale = width / 235.52;
    const auto x = [&](qreal v) { return centre + (v - 512.0) * scale; };
    const auto y = [&](qreal v) { return top + (v - 430.28) * scale; };
    QPainterPath path;
    path.moveTo(x(512), y(430.28));
    path.quadTo(x(469.6), y(467.97), x(394.24), y(479.74));
    path.lineTo(x(394.24), y(552.76));
    path.quadTo(x(394.24), y(663.45), x(512), y(715.26));
    path.quadTo(x(629.76), y(663.45), x(629.76), y(552.76));
    path.lineTo(x(629.76), y(479.74));
    path.quadTo(x(554.4), y(467.97), x(512), y(430.28));
    path.closeSubpath();
    return path;
}
// Takes `path` out of what is drawn, with `halo` to spare round it.
void erase(QPainter &p, const QPainterPath &path, qreal halo) {
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.setPen(halo > 0 ? QPen(Qt::black, halo * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                      : QPen(Qt::NoPen));
    p.setBrush(Qt::black);
    p.drawPath(path);
    p.restore();
}
} // namespace
QImage trayGlyphImage(int pixels, const QColor &color) {
    // Drawn at eight times the size and scaled down: Qt's own anti-aliasing of a line a pixel wide
    // is off by up to half a pixel, and the mark would lean.
    constexpr int kSupersample = 8;
    QImage image(pixels * kSupersample, pixels * kSupersample, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.scale(kSupersample, kSupersample);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = pixels;
    const bool tiny = pixels <= 20, small = pixels <= 26, keyhole = pixels >= 28;
    // The envelope is squarer, and its lines thinner, where there are few pixels to go round.
    const qreal stroke = tiny ? 1.0 : std::max(1.3, s * 0.075);
    const qreal margin = std::max(1.0, s * 0.04);
    qreal top = (s - s * (tiny ? 0.70 : small ? 0.64 : 0.60)) / 2;
    if (tiny)
        top = std::round(top); // a line one pixel wide lies on the pixel grid and stays one pixel
    const QRectF body(margin + stroke / 2, top + stroke / 2, s - 2 * margin - stroke,
                      s - 2 * top - stroke);
    const qreal radius = tiny ? 1.5 : s * 0.10;
    p.setPen(QPen(color, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(body, radius, radius);
    // The Y's two arms start on the rounded corners, where they join the outline unseen.
    const qreal centre = s / 2, join = body.top() + body.height() / 2;
    const qreal inset = radius * (1 - std::sqrt(0.5));
    QPainterPath arms;
    arms.moveTo(body.left() + inset, body.top() + inset);
    arms.lineTo(centre, join);
    arms.lineTo(body.right() - inset, body.top() + inset);
    p.drawPath(arms);
    // The shield sits on the join, with a clear border that sets it off from the arms.
    const qreal width = s * (tiny ? 0.32 : 0.30), height = width * 284.98 / 235.52;
    const qreal gap = tiny ? 0.8 : std::max(1.0, s * 0.045);
    const qreal shieldTop = std::max(std::min(join - height * 0.36, body.bottom() - stroke / 2 -
                                                                       height - (tiny ? 0.8 : 1.0)),
                                     body.top() + stroke / 2 + 0.6);
    const auto shield = shieldPath(centre, shieldTop, width);
    erase(p, shield, gap);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(shield);
    if (keyhole) {
        const qreal r = width * 0.125, y = shieldTop + height * 0.37;
        QPainterPath hole;
        hole.addEllipse(QPointF(centre, y), r, r);
        QPainterPath slot;
        slot.moveTo(centre - r * 0.45, y + r * 0.4);
        slot.lineTo(centre - r * 0.8, y + r * 2.5);
        slot.lineTo(centre + r * 0.8, y + r * 2.5);
        slot.lineTo(centre + r * 0.45, y + r * 0.4);
        slot.closeSubpath();
        erase(p, hole.united(slot), 0);
    }
    p.end();
    return image.scaled(pixels, pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
QIcon trayGlyph(const QColor &color) {
    QIcon icon;
    for (int size : kTraySizes)
        icon.addPixmap(QPixmap::fromImage(trayGlyphImage(size, color)));
    return icon;
}
TrayTone trayTone(TrayPlatform platform, const TrayEnvironment &environment) {
    switch (platform) {
    case TrayPlatform::Mac: return {QColor(Qt::black), true};
    case TrayPlatform::Windows:
        return {environment.lightTaskbar ? QColor("#1c1c1c") : QColor(Qt::white), false};
    case TrayPlatform::Linux: break;
    }
    // XDG_CURRENT_DESKTOP lists the desktops a session is, most specific first: "ubuntu:GNOME".
    for (const auto &desktop : environment.desktop.split(':', Qt::SkipEmptyParts))
        if (desktop.compare("GNOME", Qt::CaseInsensitive) == 0 ||
            desktop.compare("Unity", Qt::CaseInsensitive) == 0)
            return {QColor(Qt::white), false};
    return {environment.darkScheme ? QColor("#f0f0f0") : QColor("#232323"), false};
}
TrayPlatform thisPlatform() {
#if defined(Q_OS_MACOS)
    return TrayPlatform::Mac;
#elif defined(Q_OS_WIN)
    return TrayPlatform::Windows;
#else
    return TrayPlatform::Linux;
#endif
}
TrayEnvironment currentTrayEnvironment() {
    TrayEnvironment environment;
#ifdef Q_OS_WIN
    // The taskbar's theme is its own; AppsUseLightTheme, which Qt follows, is the apps'.
    environment.lightTaskbar =
        QSettings(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
                  QSettings::NativeFormat)
            .value("SystemUsesLightTheme", 0)
            .toInt() != 0;
#endif
    environment.desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    environment.darkScheme = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    return environment;
}
namespace {
std::optional<bool> &forcedAvailability() {
    static std::optional<bool> forced;
    return forced;
}
} // namespace
TrayIcon::TrayIcon(Labels labels, QObject *parent)
    : QObject(parent), labels_(std::move(labels)), icon_(new QSystemTrayIcon(this)),
      menu_(new QMenu) {
    mac::silenceStatusItemClicks(); // a click on Qt's menu-bar icon aborts the app on macOS 27
    menu_->setObjectName("trayMenu");
    const auto item = [this](const char *name, const QString &text, auto signal) {
        auto action = menu_->addAction(text);
        action->setObjectName(name);
        // macOS takes an item to the application menu by its words (Quit, Settings): not these.
        action->setMenuRole(QAction::NoRole);
        connect(action, &QAction::triggered, this, signal);
        return action;
    };
    showHide_ = item("trayShowHideAction", labels_.show, &TrayIcon::showHideRequested);
    menu_->addSeparator();
    newLetter_ = item("trayNewLetterAction", labels_.newLetter, &TrayIcon::newLetterRequested);
    newChat_ = item("trayNewChatAction", labels_.newChat, &TrayIcon::newChatRequested);
    item("traySettingsAction", labels_.settings, &TrayIcon::settingsRequested);
    menu_->addSeparator();
    item("trayQuitAction", labels_.quit, &TrayIcon::quitRequested);
    setMailboxOpen(false);
    icon_->setToolTip("ynotbit");
    icon_->setContextMenu(menu_);
    connect(icon_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    emit showRequested();
            });
    // A bar's colours can change under a running app.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this] { refreshIcon(); });
    refreshIcon();
    icon_->show();
}
TrayIcon::~TrayIcon() {
    icon_->hide();
    icon_->setContextMenu(nullptr);
    delete menu_;
}
bool TrayIcon::active() const {
    const bool there = forcedAvailability() ? *forcedAvailability()
                                            : QSystemTrayIcon::isSystemTrayAvailable();
    return there && icon_->isVisible();
}
void TrayIcon::setWindowShown(bool shown) {
    showHide_->setText(shown ? labels_.hide : labels_.show);
}
void TrayIcon::setMailboxOpen(bool open) {
    newLetter_->setEnabled(open);
    newChat_->setEnabled(open);
}
void TrayIcon::setAvailableForTesting(std::optional<bool> available) {
    forcedAvailability() = available;
}
void TrayIcon::refreshIcon() {
    const auto tone = trayTone(thisPlatform(), currentTrayEnvironment());
    auto icon = trayGlyph(tone.color);
    icon.setIsMask(tone.mask);
    icon_->setIcon(icon);
}
} // namespace bm
