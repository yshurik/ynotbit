#include "updates.h"
#include <QRegularExpression>
#include <QStringList>
namespace bm::updates {
namespace {
QString &publisher() {
    // Signs ynotbit's release announcements; its key is held by the maintainer.
    static QString address = QStringLiteral("BM-2666hf5eAbjCJMaPwC7eG3Um55QJGM");
    return address;
}
} // namespace
QString publisherAddress() {
    return publisher();
}
void setPublisherAddressForTesting(const QString &address) {
    publisher() = address;
}
std::optional<QString> announcedVersion(const QString &subject) {
    static const QRegularExpression version("^\\s*ynotbit\\s+v?(\\d{1,4}(?:\\.\\d{1,4}){1,3})\\s*$",
                                            QRegularExpression::CaseInsensitiveOption);
    const auto v = version.match(subject);
    if (!v.hasMatch())
        return {};
    return v.captured(1);
}
QString releaseUrl(const QString &version) {
    return "https://github.com/yshurik/ynotbit/releases/tag/v" + version;
}
int compareVersions(const QString &a, const QString &b) {
    const auto x = a.split('.'), y = b.split('.');
    for (int i = 0; i < qMax(x.size(), y.size()); ++i) {
        const int p = i < x.size() ? x[i].toInt() : 0, q = i < y.size() ? y[i].toInt() : 0;
        if (p != q)
            return p < q ? -1 : 1;
    }
    return 0;
}
} // namespace bm::updates
