// Pictures in a letter view: sized before layout, decoded once whichever view
// shows them, kept at the screen's resolution, and bounded in memory. Runs at
// a device pixel ratio of 2 (QT_SCALE_FACTOR=2), as on a Retina screen.
#include "letter_render.h"
#include <QApplication>
#include <QBuffer>
#include <QPainter>
#include <QTextBlock>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static QString dataUrl(const QImage &image, const char *format, const char *type) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return QString("data:%1;base64,").arg(type) + QString::fromLatin1(bytes.toBase64());
}
// The pictures' formats in a document, one per picture.
static QList<QTextImageFormat> pictures(const QTextDocument *doc) {
    QList<QTextImageFormat> formats;
    for (auto block = doc->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().isImageFormat())
                formats << it.fragment().charFormat().toImageFormat();
    return formats;
}
static QPixmap shown(const QTextDocument *doc, const QTextImageFormat &picture) {
    return doc->resource(QTextDocument::ImageResource, QUrl(picture.name())).value<QPixmap>();
}
// A read-only letter view, as the reader, the feed and the letter window use.
struct View {
    bm::LetterView view;
    explicit View(int width) {
        view.setDocument(new bm::SafeDocument(&view));
        view.resize(width, 600);
        view.show();
    }
    void show(const QString &letter) {
        bm::renderBody(&view, letter, bm::BodyView::Markdown);
        view.document()->size(); // laid out
    }
    void paint() {
        view.grab();
    }
};
static QString letterWith(const QString &url) {
    return "From the hill:\n\n![harbour][img1]\n\nMore next week.\n\n[img1]: " + url;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        require(qGuiApp->devicePixelRatio() == 2, "test sanity: a Retina-sized screen");
        QImage photo(1200, 900, QImage::Format_RGB32);
        {
            QPainter p(&photo);
            QLinearGradient sky(0, 0, 1200, 900);
            sky.setColorAt(0, Qt::darkBlue);
            sky.setColorAt(1, QColor(250, 180, 90));
            p.fillRect(photo.rect(), sky);
        }
        const auto photoUrl = dataUrl(photo, "JPG", "image/jpeg");

        View reader(900);
        int decodes = bm::pictureDecodes();
        reader.show(letterWith(photoUrl));
        auto formats = pictures(reader.view.document());
        require(formats.size() == 1 && formats[0].width() == 640 && formats[0].height() == 480,
                "a picture is sized before layout: no wider than a comfortable column");
        require(!formats[0].name().startsWith("data:") && formats[0].name().size() < 100,
                "a shown picture goes by a short name, not its data: URL");
        require(bm::pictureDecodes() == decodes,
                "laying out a letter decodes none of its pictures");

        reader.paint();
        require(bm::pictureDecodes() == decodes + 1, "painting a picture decodes it");
        reader.paint();
        reader.paint();
        require(bm::pictureDecodes() == decodes + 1, "painting it again decodes nothing");
        View feed(900);
        feed.show(letterWith(photoUrl));
        feed.paint();
        require(bm::pictureDecodes() == decodes + 1,
                "another view showing the same picture decodes nothing");

        const auto pixmap = shown(reader.view.document(), formats[0]);
        require(pixmap.size() == QSize(1280, 960) && pixmap.devicePixelRatio() == 2,
                "a picture is kept at the screen's resolution: 640x480 points, 1280x960 pixels");

        reader.view.resize(420, 600);
        const auto document = reader.view.document();
        const int room = int(document->textWidth() - 2 * document->documentMargin());
        formats = pictures(document);
        require(room < 640 && formats[0].width() == room &&
                    qAbs(formats[0].height() - room * 3 / 4) <= 1,
                "a narrow column shrinks a picture, keeping its shape");
        require(shown(document, formats[0]).width() == 2 * room, "...at the screen's resolution");
        reader.view.resize(900, 600);
        require(pictures(document)[0].width() == 640, "a wide column gives it back its size");
        const auto shortName = pictures(document)[0].name();
        reader.show("The next letter, no pictures.");
        require(document->resource(QTextDocument::ImageResource, QUrl(shortName))
                    .value<QPixmap>()
                    .isNull(),
                "the next letter forgets the last one's pictures");

        // The composer keeps the letter's data: URLs, as it writes them out.
        bm::SafeDocument composer;
        decodes = bm::pictureDecodes();
        const auto first = composer.resource(QTextDocument::ImageResource, QUrl(photoUrl));
        const auto again = composer.resource(QTextDocument::ImageResource, QUrl(photoUrl));
        require(!first.value<QPixmap>().isNull() && bm::pictureDecodes() <= decodes + 1 &&
                    again.value<QPixmap>().cacheKey() == first.value<QPixmap>().cacheKey(),
                "a picture by its data: URL is decoded once too");
        require(composer.resource(QTextDocument::ImageResource, QUrl("file:///etc/passwd"))
                    .value<QPixmap>()
                    .isNull(),
                "local files are never loaded");
        // Many pictures: the least recently shown make way, so memory stays
        // bounded. 61 photos at 1280x960 pixels would take 300 MB.
        View many(900);
        const auto colored = [](int i) {
            QImage image(640, 480, QImage::Format_RGB32);
            image.fill(QColor::fromHsv(i * 6 % 360, 200, 200 - i % 50));
            return dataUrl(image, "PNG", "image/png");
        };
        many.show(letterWith(colored(0)));
        many.paint();
        decodes = bm::pictureDecodes();
        for (int i = 1; i <= 60; ++i) {
            many.show(letterWith(colored(i)));
            many.paint();
        }
        many.show(letterWith(colored(0)));
        many.paint();
        require(bm::pictureDecodes() == decodes + 61,
                "a picture shown long ago is decoded again: they are not all kept");
        std::cout << "PASS: pictures are sized up front, decoded once, kept at screen resolution\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
