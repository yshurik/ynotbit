// Ciphertext-only filesystem boundary. Runs in the keyless relay process.
#include "pow.h"
#include "protocol.h"
#include "relay_api.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

static bool writeFile(const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}
extern "C" void ynotbit_relay_tick(struct ntb_network *nw, const char *directory) {
    static qint64 last = 0;
    const auto now = QDateTime::currentSecsSinceEpoch();
    if (last == now || !directory)
        return;
    last = now;
    try {
        const QString root = QFile::decodeName(directory);
        QDir().mkpath(root + "/publish");
        QDir().mkpath(root + "/receipts");
        const int peers = ntb_network_connected_peers(nw);
        writeFile(root + "/status.json",
                  QJsonDocument(QJsonObject{{"peers", peers}, {"time", now}}).toJson());
        int count = 0;
        for (const auto &entry :
             QDir(root + "/publish").entryInfoList({"*.object"}, QDir::Files, QDir::Name)) {
            if (++count > 8)
                break;
            const auto id = entry.completeBaseName();
            if (!QRegularExpression("^[a-f0-9-]{36}$").match(id).hasMatch() || entry.isSymLink())
                continue;
            QFile file(entry.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const auto object = file.read(262145);
            file.close();
            auto hash = bm::Protocol::inventoryHash(object);
            bool accepted = bm::ProofOfWork::valid(object, now);
            auto rawHash = QByteArray::fromHex(hash.toLatin1());
            if (accepted) {
                accepted = ntb_network_submit(
                    nw, reinterpret_cast<const uint8_t *>(object.constData()), object.size());
            }
            // Keep the job until peers exist; re-offer after restart or a lost receipt.
            if (accepted && peers > 0)
                ntb_network_offer(nw, reinterpret_cast<const uint8_t *>(rawHash.constData()));
            const auto state = !accepted ? "rejected" : peers > 0 ? "offered" : "accepted";
            if (writeFile(
                    root + "/receipts/" + id + ".json",
                    QJsonDocument(QJsonObject{{"state", state}, {"hash", hash}, {"time", now}})
                        .toJson()) &&
                (!accepted || peers > 0))
                QFile::remove(entry.absoluteFilePath());
        }
    } catch (...) { /* Preserve queued jobs for a later attempt. */
    }
}
