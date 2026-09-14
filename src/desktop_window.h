#pragma once
#include "appearance.h"
#include <QMainWindow>
#include <QVariantMap>
class QListView;
class QListWidget;
class QLabel;
class QTextBrowser;
class QWidget;
class QVBoxLayout;
namespace bm {
class Session;
class DesktopWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit DesktopWindow(Session &session);
    void compose(QVariantMap letter = {}, bool reply = false);
    void selectMessage(const QString &id);

  private:
    Session &session_;
    Appearance appearance_;
    QListView *letters_;
    QListWidget *folders_;
    QLabel *heading_, *document_, *status_, *error_, *subject_, *details_;
    QTextBrowser *body_;
    QWidget *actions_, *reader_, *identities_, *welcome_;
    QVBoxLayout *identityLayout_;
    QVariantMap selected_;
    void updateState();
    void updateTheme();
    void refreshIdentities();
    void vaultDialog(QString path, bool create);
};
} // namespace bm
