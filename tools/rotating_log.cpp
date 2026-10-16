#include "rotating_log.h"
namespace tools {
RotatingLog::RotatingLog(qint64 maxBytes, int keep) : maxBytes_(maxBytes), keep_(keep) {}
bool RotatingLog::open(const QString &path) {
    file_.setFileName(path);
    return file_.open(QIODevice::Append | QIODevice::Text);
}
void RotatingLog::write(const QString &line) {
    if (!file_.isOpen())
        return;
    const auto bytes = line.toUtf8() + '\n';
    if (file_.size() > 0 && file_.size() + bytes.size() > maxBytes_)
        rotate();
    file_.write(bytes);
    file_.flush();
}
// log.N goes, log.N-1 becomes log.N, ... and the full log becomes log.1.
void RotatingLog::rotate() {
    const auto path = file_.fileName();
    file_.close();
    QFile::remove(path + "." + QString::number(keep_));
    for (int i = keep_ - 1; i >= 1; --i)
        QFile::rename(path + "." + QString::number(i), path + "." + QString::number(i + 1));
    QFile::rename(path, path + ".1");
    file_.open(QIODevice::Append | QIODevice::Text);
}
} // namespace tools
