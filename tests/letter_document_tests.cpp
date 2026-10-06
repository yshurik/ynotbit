// A letter body -> rich document -> body round trip is exact, and quoting,
// headings, lists and line breaks survive at every quote level.
#include "letter_document.h"
#include "quoting.h"
#include <QBuffer>
#include <QGuiApplication>
#include <QPainter>
#include <QUrl>
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
// The pictures in a document, by their URLs (one entry per picture).
static QStringList pictures(const QTextDocument &doc) {
    QStringList urls;
    for (auto block = doc.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().isImageFormat())
                for (int i = 0; i < it.fragment().length(); ++i)
                    urls << it.fragment().charFormat().toImageFormat().name();
    return urls;
}
static QString pngUrl(const QImage &image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return "data:image/png;base64," + QString::fromLatin1(bytes.toBase64());
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
        {
            // Pictures: made for a letter, read back, and nothing else loads.
            QImage photo(900, 600, QImage::Format_RGB32);
            {
                QPainter p(&photo);
                QLinearGradient sky(0, 0, 900, 600);
                sky.setColorAt(0, Qt::darkBlue);
                sky.setColorAt(1, QColor(250, 180, 90));
                p.fillRect(photo.rect(), sky);
                for (int i = 0; i < 300; ++i)
                    p.fillRect((i * 37) % 900, (i * 53) % 600, 9, 9, QColor::fromHsv(i % 360, 200, 200));
            }
            photo.setText("GPSPosition", "52.37N 4.89E secret");
            const auto url = bm::imageDataUrl(photo, 40000);
            require(url.startsWith("data:image/") && url.size() <= 40000,
                    "a picture is shrunk to fit its budget");
            require(!QByteArray::fromBase64(url.section(',', 1).toLatin1()).contains("secret"),
                    "the picture's metadata is left behind");
            const auto back = bm::letterImage(QUrl(url));
            require(!back.isNull() && qAbs(back.width() * 2 - back.height() * 3) <= 6,
                    "it decodes again, the same shape");
            require(bm::imageDataUrl(photo, 100).isEmpty(), "no room, no picture");
            QImage icon(32, 32, QImage::Format_ARGB32);
            icon.fill(Qt::transparent);
            QPainter(&icon).fillRect(8, 8, 16, 16, Qt::red);
            require(bm::imageDataUrl(icon, 40000).startsWith("data:image/png;"),
                    "a small transparent picture stays PNG");
            require(bm::letterImage(QUrl("https://example.com/a.png")).isNull(),
                    "a remote picture is never loaded");
            const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg' width='9' height='9'/>";
            require(bm::letterImage(QUrl("data:image/svg+xml;base64," + svg.toBase64())).isNull() &&
                        bm::letterImage(QUrl("data:image/png;base64," + svg.toBase64())).isNull(),
                    "SVG is refused, whatever the label says");
            require(bm::letterImage(QUrl(pngUrl(QImage(5000, 8, QImage::Format_RGB32)))).isNull(),
                    "an oversized picture is refused before it is decoded");
            require(bm::letterImage(QUrl("data:image/png;base64,@@not base64@@")).isNull(),
                    "broken base64 is refused");
            require(bm::letterImageSize(QUrl(pngUrl(icon))) == QSize(32, 32),
                    "a picture's size is read from its header");
            const QUrl oversized(pngUrl(QImage(5000, 8, QImage::Format_RGB32)));
            require(!bm::letterImageSize(oversized).isValid() &&
                        !bm::letterImageSize(QUrl("https://example.com/a.png")).isValid(),
                    "...only for pictures a letter shows");
            {
                // A phone photo: stored sideways, with an EXIF orientation
                // (6: turn 90 degrees clockwise) right after the JPEG's start.
                QImage wide(40, 20, QImage::Format_RGB32);
                wide.fill(Qt::gray);
                QByteArray bytes;
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::WriteOnly);
                wide.save(&buffer, "JPG");
                const QByteArray exif("\xff\xe1\x00\x22"
                                      "Exif\x00\x00MM\x00\x2a\x00\x00\x00\x08"
                                      "\x00\x01\x01\x12\x00\x03\x00\x00\x00\x01\x00\x06\x00\x00"
                                      "\x00\x00\x00\x00",
                                      36);
                bytes.insert(2, exif);
                const QUrl turned("data:image/jpeg;base64," +
                                  QString::fromLatin1(bytes.toBase64()));
                require(bm::letterImageSize(turned) == QSize(20, 40) &&
                            bm::letterImage(turned).size() == QSize(20, 40),
                        "a turned photo's size is its upright size, as it is shown");
            }

            // In a letter: by reference, the definitions at the end.
            const auto dot = pngUrl(icon);
            const QString letter = "Look:\n\n![sky][img1]\n\n-- \nsent by ynotbit\n\n[img1]: " + dot;
            same(letter, "a letter with a picture round trips, the definition at the end");
            QTextDocument doc;
            bm::loadLetter(&doc, letter, true);
            require(pictures(doc) == QStringList{dot}, "the reference becomes the picture");
            bm::loadLetter(&doc, "> On Monday, Bob wrote:\n> ![x][A]\n> [a]: " + dot, true);
            require(pictures(doc) == QStringList{dot} &&
                        bm::blockQuoteLevel(blockWith(doc, QString(QChar::ObjectReplacementCharacter))) == 1,
                    "a quoted picture resolves its definition, labels case-insensitive");
            bm::loadLetter(&doc, "Inline ![a](" + dot + ") and again ![b](" + dot + ")", true);
            require(bm::letterMarkdown(&doc) ==
                        "Inline ![a][img1] and again ![b][img1]\n\n[img1]: " + dot,
                    "inline pictures are written by reference, one definition per picture");
            bm::loadLetter(&doc, "Sun:\n\n<img src=\"" + dot + "\"/>", true);
            require(pictures(doc) == QStringList{dot}, "PyBitmessage's <img src=data:> shows too");
            bm::loadLetter(&doc, "![x](https://example.com/a.png)", true);
            require(bm::letterImage(QUrl(pictures(doc).value(0))).isNull(),
                    "a remote picture in Markdown stays unloaded");
        }
        std::cout << "PASS: letter document round trips (plain, quoted Markdown, nesting, lists, code)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
