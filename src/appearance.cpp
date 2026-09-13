#include "appearance.h"
#include <QApplication>
#include <QSettings>
#include <QStyleHints>

namespace bm {
Appearance::Appearance(QObject *parent) : QObject(parent) {
    mode_ = QSettings().value("appearance/theme", "system").toString();
    if (mode_ != "light" && mode_ != "dark")
        mode_ = "system";
    connect(qGuiApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (mode_ == "system") {
            apply();
            emit changed();
        }
    });
    apply();
}
bool Appearance::dark() const {
    return mode_ == "dark" ||
           (mode_ == "system" && qGuiApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark);
}
void Appearance::setMode(const QString &mode) {
    if ((mode != "system" && mode != "light" && mode != "dark") || mode_ == mode)
        return;
    mode_ = mode;
    QSettings().setValue("appearance/theme", mode_);
    apply();
    emit changed();
}
void Appearance::apply() {
    const bool d = dark();
    QPalette palette;
    const QColor text(d ? "#e6edf3" : "#182c3a");
    const QColor muted(d ? "#a5b4c1" : "#586c7b");
    palette.setColor(QPalette::Window, QColor(d ? "#141b23" : "#f5f7fa"));
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, QColor(d ? "#1b2530" : "#ffffff"));
    palette.setColor(QPalette::AlternateBase, QColor(d ? "#222f3b" : "#edf2f5"));
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, QColor(d ? "#263440" : "#ffffff"));
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::ToolTipBase, QColor(d ? "#263440" : "#ffffff"));
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, muted);
    palette.setColor(QPalette::Highlight, QColor(d ? "#24786f" : "#c4e3df"));
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::Link, QColor(d ? "#73d8c7" : "#126d65"));
    palette.setColor(QPalette::LinkVisited, QColor(d ? "#bca9ef" : "#65509b"));
    palette.setColor(QPalette::Mid, QColor(d ? "#3d4c59" : "#cad5dd"));
    palette.setColor(QPalette::Dark, QColor(d ? "#3d4c59" : "#a2b1bc"));
    palette.setColor(QPalette::Light, QColor(d ? "#344452" : "#ffffff"));
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, muted);
    QApplication::setPalette(palette);
}
}
