#include "single_instance.h"
#include <QCryptographicHash>
#include <QDir>
#include <QLocalSocket>
namespace bm {
SingleInstance::SingleInstance(const QString &key, QObject *parent) : QObject(parent) {
    // Only this user may knock. (A socket file left by a run that crashed is replaced by the next
    // listen: with an access option, Qt binds a new one beside it and renames it over the old.)
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    server_.listen(key);
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        while (auto socket = server_.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            // Only a request to show is heeded: the other end may be anything.
            const auto heed = [this, socket] {
                if (socket->readAll().startsWith("show")) {
                    emit woken();
                    socket->disconnectFromServer();
                }
            };
            connect(socket, &QLocalSocket::readyRead, this, heed);
            if (socket->bytesAvailable())
                heed();
        }
    });
}
QString SingleInstance::keyFor(const QString &nodeFolder) {
    const auto folder = QDir::cleanPath(QDir(nodeFolder).absolutePath());
    const auto hash = QCryptographicHash::hash(folder.toUtf8(), QCryptographicHash::Sha256);
    return "ynotbit-" + QString::fromLatin1(hash.toHex().left(16));
}
bool SingleInstance::wake(const QString &key) {
    QLocalSocket socket;
    socket.connectToServer(key);
    if (!socket.waitForConnected(500))
        return false;
    socket.write("show\n");
    socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    return true;
}
} // namespace bm
