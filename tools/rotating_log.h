#pragma once
// A log file for a service that runs for months: it keeps to a size. When the next line would
// pass maxBytes, log becomes log.1, log.1 becomes log.2, and so on, and the oldest of the kept
// ones goes. A log that already exists is gone on with.
#include <QFile>
#include <QString>
namespace tools {
class RotatingLog {
  public:
    explicit RotatingLog(qint64 maxBytes = 10 * 1024 * 1024, int keep = 5);
    bool open(const QString &path);
    bool isOpen() const {
        return file_.isOpen();
    }
    // One line, without its end of line, made durable at once: a log that is read while it is
    // written, or after a crash, has what was said.
    void write(const QString &line);

  private:
    QFile file_;
    qint64 maxBytes_;
    int keep_;
    void rotate();
};
} // namespace tools
