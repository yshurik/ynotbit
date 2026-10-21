#pragma once
#include <QLocalServer>
#include <QObject>
namespace bm {
// One ynotbit runs per node folder. A second launch finds the first through a local socket named
// for the folder, asks it to show its window, and exits: the first may be living in the
// notification area, with no window to be seen.
class SingleInstance : public QObject {
    Q_OBJECT
  public:
    // Listens under `key`.
    explicit SingleInstance(const QString &key, QObject *parent = nullptr);
    bool listening() const { return server_.isListening(); }
    // The socket's name for a node folder: the same folder, however it is spelled, is the same name.
    static QString keyFor(const QString &nodeFolder);
    // Asks the instance listening under `key` to show its window; false when none answers.
    static bool wake(const QString &key);
  signals:
    void woken(); // someone asked for the window

  private:
    QLocalServer server_;
};
} // namespace bm
