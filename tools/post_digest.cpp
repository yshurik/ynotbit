// Publishes an issue of the Bitmessage digest: a broadcast from the digest
// address, which every ynotbit mailbox is subscribed to by default. The issue
// is a Markdown file; the footer asking for submissions is always appended.
//
//   build/post_digest --keys digest_keys.dat --issue issue.md [--subject "…"]
//
// Without --send it stays offline and only prints the letter it would publish.
#include "publish_broadcast.h"
#include "updates.h"
#include <QApplication>
#include <QDate>
#include <QFile>
#include <QLocale>
namespace {
// Every issue ends with this, so readers always know how to contribute.
QString footer(const QString &address) {
    return QString(
               "---\n\n"
               "**Bitmessage digest** is a weekly round-up from the Bitmessage network.\n\n"
               "Want something in the next issue? Reply privately to this broadcast, or write "
               "to `%1`, and send:\n\n"
               "- broadcast addresses you want promoted: yours, or ones worth following\n"
               "- news, offers, updates, new chans and tools\n\n"
               "Follow the digest: ynotbit subscribes to it by default. In PyBitmessage, add a "
               "subscription to `%1`.")
        .arg(address);
}
} // namespace
int main(int argc, char **argv) {
    // Session runs its network node as this same executable with --node.
    if (argc > 1 && std::string(argv[1]) == "--node")
        return tools::runNode(argc, argv);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("YnotbitTools");
    app.setApplicationName("post_digest");
    const auto args = app.arguments();
    tools::Broadcast b;
    b.tool = "post_digest";
    b.keys = tools::option(args, "--keys");
    const auto issue = tools::option(args, "--issue");
    if (b.keys.isEmpty() || issue.isEmpty())
        return tools::fail(b.tool, "usage: post_digest --keys <keys.dat> --issue <file.md> "
                                   "[--subject <text>] [--send] [--linger <minutes>]");
    QFile issueFile(issue);
    if (!issueFile.open(QIODevice::ReadOnly))
        return tools::fail(b.tool, "cannot read " + issue);
    const auto text = QString::fromUtf8(issueFile.readAll()).trimmed();
    if (text.isEmpty())
        return tools::fail(b.tool, issue + " is empty");
    b.from = bm::updates::digestAddress();
    const auto end = footer(b.from);
    // An issue drafted from the last one may carry its footer already.
    b.body = text.endsWith(end) ? text : text + "\n\n" + end;
    b.subject = tools::option(args, "--subject");
    if (b.subject.isEmpty())
        b.subject = "Bitmessage digest · " +
                    QLocale::c().toString(QDate::currentDate(), "d MMM yyyy");
    if (!tools::sendOptions(args, b))
        return 1;
    return tools::publish(b);
}
