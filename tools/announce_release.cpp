// Publishes a release announcement: a broadcast from ynotbit's release address
// whose subject is "ynotbit X.Y.Z", which every ynotbit mailbox turns into an
// update notice. Runs headless on a throwaway vault in a temporary directory,
// so the release key never enters a personal vault.
//
//   build/announce_release --keys release_key.dat --version 0.5.1 --notes notes.md
//
// Without --send it stays offline and only prints the letter it would publish.
// With --chan <phrase> (repeatable) the letter goes to those chans, from the
// release address, instead of being broadcast.
#include "publish_broadcast.h"
#include "updates.h"
#include <QApplication>
#include <QFile>
int main(int argc, char **argv) {
    // Session runs its network node as this same executable with --node.
    if (argc > 1 && std::string(argv[1]) == "--node")
        return tools::runNode(argc, argv);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("YnotbitTools");
    app.setApplicationName("announce_release");
    const auto args = app.arguments();
    tools::Broadcast b;
    b.tool = "announce_release";
    b.keys = tools::option(args, "--keys");
    const auto version = tools::option(args, "--version"), notes = tools::option(args, "--notes");
    if (b.keys.isEmpty() || version.isEmpty() || notes.isEmpty())
        return tools::fail(b.tool, "usage: announce_release --keys <keys.dat> --version <X.Y.Z> "
                                   "--notes <file> [--chan <phrase>]... [--send] "
                                   "[--linger <minutes>]");
    b.subject = "ynotbit " + version;
    if (bm::updates::announcedVersion(b.subject) != version)
        return tools::fail(b.tool, "\"" + b.subject + "\" is not a subject ynotbit reads as a release");
    QFile notesFile(notes);
    if (!notesFile.open(QIODevice::ReadOnly))
        return tools::fail(b.tool, "cannot read " + notes);
    b.body = QString::fromUtf8(notesFile.readAll()).trimmed();
    if (b.body.isEmpty())
        return tools::fail(b.tool, notes + " is empty");
    b.chans = tools::options(args, "--chan");
    if (!tools::sendOptions(args, b))
        return 1;
    b.from = bm::updates::publisherAddress();
    return tools::publish(b);
}
