#pragma once
#include <QObject>
#include <QPalette>

namespace bm {
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
