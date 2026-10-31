// Publishes an issue of the Bitmessage digest: a broadcast from the digest
// address, which every ynotbit mailbox is subscribed to by default. The issue
// is a Markdown file; the footer asking for submissions is appended to it.
//
//   build/post_digest --keys digest_keys.dat --issue issue.md [--subject "…"]
//
// Without --send it stays offline and only prints the letter it would publish.
// With --chan <phrase> (repeatable) the issue goes to those chans, from the
// digest address, instead of being broadcast, and without the footer, which
// speaks of replying to a broadcast.
#include "digest_body.h"
#include "publish_broadcast.h"
#include "updates.h"
#include <QApplication>
#include <QDate>
#include <QFile>
#include <QLocale>
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
                                   "[--subject <text>] [--chan <phrase>]... [--send] "
                                   "[--linger <minutes>]");
    QFile issueFile(issue);
    if (!issueFile.open(QIODevice::ReadOnly))
        return tools::fail(b.tool, "cannot read " + issue);
    const auto text = QString::fromUtf8(issueFile.readAll()).trimmed();
    if (text.isEmpty())
        return tools::fail(b.tool, issue + " is empty");
    b.from = bm::updates::digestAddress();
    b.chans = tools::options(args, "--chan");
    b.body = tools::digestBody(text, b.from, !b.chans.isEmpty());
    b.subject = tools::option(args, "--subject");
    if (b.subject.isEmpty())
        b.subject = "Bitmessage digest · " +
                    QLocale::c().toString(QDate::currentDate(), "d MMM yyyy");
    if (!tools::sendOptions(args, b))
        return 1;
    return tools::publish(b);
}
