// Reply quoting: reading ">" and PyBitmessage's dash-separated history, and
// keeping line structure through the composer's Markdown round trip.
#include "quoting.h"
#include <QGuiApplication>
#include <QTextBlock>
#include <QTextDocument>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
// What the composer does to a body: load it as Markdown, export it again.
static QString throughComposer(const QString &body) {
    QTextDocument doc;
    doc.setMarkdown(bm::toComposerMarkdown(body), QTextDocument::MarkdownDialectGitHub);
    return bm::fromComposerMarkdown(doc.toMarkdown());
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        using bm::quoteLevel;
        int length = 0;
        require(quoteLevel("plain") == 0 && quoteLevel("") == 0, "unquoted lines are level 0");
        require(quoteLevel("> a", &length) == 1 && length == 2, "\"> a\" is one level");
        require(quoteLevel(">a", &length) == 1 && length == 1, "\">a\" (no space) is one level");
        require(quoteLevel(">> a") == 2 && quoteLevel("> > a", &length) == 2 && length == 4,
                "both nesting spellings count");
        require(quoteLevel(" > a") == 0, "an indented > is not a quote");

        const QString dashes(54, '-');
        require(bm::hasQuoting("hi\n> quoted") && bm::hasQuoting("hi\n" + dashes + "\nold") &&
                    !bm::hasQuoting("plain\n" + QString(60, '-') + "\nsig"),
                "quoting is recognised both ways, but a long rule is not a separator");
        require(bm::isPyBitmessageSeparator(dashes) && !bm::isPyBitmessageSeparator(QString(60, '-')),
                "only PyBitmessage's 54-dash line is a separator, not a signature rule");
        // The shape PyBitmessage threads have in real chans.
        const auto chain = "go offline\n\n" + dashes + "\nWhat happened?\n\n" + dashes +
                           "\nWe have zero working bootstrap addresses.\nNew people are offline.";
        require(bm::normalizeQuotes(chain) ==
                    "go offline\n> What happened?\n>> We have zero working bootstrap addresses.\n"
                    ">> New people are offline.",
                "each separator makes the history below it one level deeper");
        require(bm::normalizeQuotes(">working bootstrap\ncrap\n\n" + dashes + "\n> old\nolder") ==
                    ">working bootstrap\ncrap\n>> old\n> older",
                "inline > quotes and separators combine");

        const auto quoted = bm::quoteForReply(
            "Sounds good.\n\n> earlier point\n\n-- \nsent by ynotbit", "On 29 Sep 2026, Alice wrote:");
        require(quoted == "On 29 Sep 2026, Alice wrote:\n> Sounds good.\n>\n>> earlier point",
                "a reply quotes one level deeper, keeps blank lines, drops the signature");
        require(bm::quoteForReply("hi\n\n" + dashes + "\nold", "A:") == "A:\n> hi\n>> old",
                "a PyBitmessage thread is quoted as nested levels");

        // Through the composer: quoted lines stay consecutive and the
        // signature delimiter keeps its own, unescaped line.
        const QString reply = "\n\n-- \nsent by ynotbit\n\nOn X, Alice wrote:\n> line one\n> line two\n"
                              ">> deeper one\n>> deeper two";
        const auto sent = throughComposer("Thanks!" + reply);
        require(sent.contains("-- \nsent by ynotbit") && !sent.contains("\\--"),
                "the signature is sent as \"-- \" then its text, never \"\\--\"");
        require(sent.contains("> line one  \n> line two"), "quoted lines stay consecutive");
        require(sent.contains("> > deeper one  \n> > deeper two"), "and so do deeper ones");
        require(throughComposer(sent) == sent, "reopening a draft changes nothing");
        require(bm::fromComposerMarkdown("x\n\n\\-- sent by ynotbit") == "x\n\n-- \nsent by ynotbit",
                "older drafts with the merged, escaped signature are repaired");

        // What the composer shows: one block per quoted line, at its level.
        QTextDocument doc;
        doc.setMarkdown(bm::toComposerMarkdown(sent), QTextDocument::MarkdownDialectGitHub);
        QList<int> levels;
        for (auto block = doc.begin(); block.isValid(); block = block.next())
            if (block.text().contains("line") || block.text().contains("deeper"))
                levels << block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel);
        require(levels == QList<int>({1, 1, 2, 2}), "the composer shows quote levels per line");
        std::cout << "PASS: quote levels, PyBitmessage threads, reply quoting, composer round trip\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
