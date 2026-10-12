#pragma once
#include <QObject>
#include <QPalette>
#include <QFont>
#include <QIcon>
class QPushButton;

namespace bm {
QFont addressFont();
// Makes a dialog's default button, bold by its own font: Qt measures a button
// in the font of its plain state, so a bold :default style would cut its text.
void makeDefault(QPushButton *button);
QIcon appLogo();
QIcon windowLogo();
class Appearance : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
public:
    explicit Appearance(QObject *parent = nullptr);
    QString mode() const { return mode_; }
    bool dark() const;
    void setMode(const QString &mode);
signals:
    void changed();
private:
    void apply();
    QString mode_;
};
}
