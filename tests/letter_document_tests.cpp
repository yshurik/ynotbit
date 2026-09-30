// A letter body -> rich document -> body round trip is exact, and quoting,
// headings, lists and line breaks survive at every quote level.
#include "letter_document.h"
#include "quoting.h"
#include <QGuiApplication>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static QString roundTrip(const QString &body) {
    QTextDocument doc;
    bm::loadLetter(&doc, body, true);
    return bm::letterMarkdown(&doc);
}
static void same(const QString &body, const char *message) {
    const auto back = roundTrip(body);
    if (back != body) {
        std::cerr << "expected:\n" << body.toStdString() << "\n--- got:\n" << back.toStdString() << "\n";
        require(false, message);
    }
}
static QTextBlock blockWith(const QTextDocument &doc, const QString &text) {
    for (auto block = doc.begin(); block.isValid(); block = block.next())
        if (block.text().contains(text))
            return block;
    return {};
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        same("Hi Bob,\nthe second line stays a line.\n\nA new paragraph.\n\n-- \nsent by ynotbit",
             "a plain letter keeps its lines, paragraphs and signature");
        const QString markdownReply =
            "Thanks, noted.\n\n-- \nsent by ynotbit\n\nOn 29 Sep 2026, Alice wrote:\n"
            "> # Release notes\n>\n> ## What changed\n>\n> The **address book** is here.\n>\n"
            "> - contacts page\n> - one-click add\n>\n> Thanks!";
        same(markdownReply, "a reply quoting Markdown comes back exactly");
        {
            QTextDocument doc;
            bm::loadLetter(&doc, markdownReply, true);
            const auto h1 = blockWith(doc, "Release notes");
            const auto item = blockWith(doc, "contacts page");
            require(h1.blockFormat().headingLevel() == 1 && bm::blockQuoteLevel(h1) == 1,
                    "a quoted heading is a heading inside the quote");
            require(item.textList() && bm::blockQuoteLevel(item) == 1, "a quoted list is a list");
            require(!blockWith(doc, "Thanks!").textList(), "the paragraph after the list is not in it");
            require(bm::blockQuoteLevel(blockWith(doc, "Thanks, noted.")) == 0, "own words are unquoted");
        }
        same("Agreed.\n\nOn X, Bob wrote:\n> go offline\n>> What happened?\n>> They were fine.\n"
             ">>> We have zero working bootstrap addresses.",
             "nested plain quoting keeps its levels and lines");
        {
            const auto quote = bm::quoteForReply(
                "go offline\n\n" + QString(54, '-') + "\nWhat happened?\n\n" + QString(54, '-') +
                    "\nWe have zero bootstrap addresses.\nNew people are always offline.",
                "On X, Bob wrote:");
            same("Hi\n\n" + quote, "a quoted PyBitmessage thread survives the round trip");
        }
        same("1. first\n2. second\n\n- bullet\n    - nested\n\nSee [the site](https://example.com) "
             "and `code`, *italic*, ~~gone~~.",
             "numbered and nested lists, links and inline formats");
        same("Before\n\n```\nint x = 1;\nint y = 2;\n```\n\nAfter", "a code block is kept verbatim");
        same("> quoted code:\n>\n> ```\n> x = 1\n> ```", "a quoted code block keeps its quote");
        {
            QTextDocument doc;
            bm::loadLetter(&doc, "#hashtag is not a heading\n\n1) neither is this", false);
            require(bm::letterMarkdown(&doc) == "#hashtag is not a heading\n\n\\1) neither is this",
                    "plain text that looks like list syntax is escaped, and only then");
        }
        require(!roundTrip("x\n\n\\-- sent by ynotbit").contains("\\--"),
                "an older draft's escaped \"\\--\" signature is not sent escaped again");
        std::cout << "PASS: letter document round trips (plain, quoted Markdown, nesting, lists, code)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
