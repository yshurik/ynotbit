#pragma once
// Publishing one broadcast from a keys file, headless, for the release and
// digest tools. Runs on a throwaway vault in a temporary directory with an
// in-memory password, so the key never enters a personal vault; the node is
// the tool's own executable with --node, like the app.
#include <QString>
#include <QStringList>
namespace tools {
struct Broadcast {
    QString tool;    // the program's name, for messages
    QString keys;    // keys.dat holding the sender's private keys
    QString from;    // the address it must be sent from
    QString subject;
    QString body;
    bool send = false; // otherwise a dry run: offline, prints the letter
    int linger = 10;   // minutes the node stays up after peers were offered it
};
// "--name value" from the command line, or empty.
QString option(const QStringList &args, const QString &name);
// Reads --send and --linger into the broadcast; false (with a message) when
// --linger is not a number of minutes.
bool sendOptions(const QStringList &args, Broadcast &broadcast);
int fail(const QString &tool, const QString &message);
// 0 once published (or after a dry run), 1 with a message otherwise.
int publish(const Broadcast &broadcast);
// For main(): the node process the session starts is this executable.
int runNode(int argc, char **argv);
} // namespace tools
