#include "markdown_editor.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextList>
#include <QThread>
#include <QUrl>

namespace bm {
namespace {
class PrivateDocument final : public QTextDocument {
public:
    explicit PrivateDocument(QObject *parent) : QTextDocument(parent) {}
    QVariant loadResource(int, const QUrl &) override {
        // Never delegate to QTextDocument: it can read file:// and relative local paths.
        // A valid, empty image also prevents fallback resource providers from running.
        return QVariant::fromValue(QImage());
    }
};
constexpr QTextDocument::MarkdownFeatures dialect =
    QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub |
                                    QTextDocument::MarkdownNoHTML);
}
MarkdownEditor::MarkdownEditor(QObject *parent) : QObject(parent) {}
void MarkdownEditor::attach(QObject *textEdit) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!textEdit || edit_ == textEdit)
        return;
    auto *quickDocument = qvariant_cast<QQuickTextDocument *>(textEdit->property("textDocument"));
    if (!quickDocument)
        return;
    edit_ = textEdit;
    auto *safeDocument = new PrivateDocument(textEdit);
    safeDocument->setDefaultFont(quickDocument->textDocument()->defaultFont());
    quickDocument->setTextDocument(safeDocument);
    document_ = safeDocument;
    connect(safeDocument, &QTextDocument::contentsChanged, this, [this] {
        if (!loading_) {
            edited_ = true;
            emit contentChanged();
        }
    });
}
void MarkdownEditor::loadMarkdown(const QString &markdown) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!document_)
        return;
    loading_ = true;
    document_->setMarkdown(markdown, dialect);
    document_->clearUndoRedoStacks();
    original_ = markdown;
    edited_ = false;
    loading_ = false;
    if (edit_)
        edit_->setProperty("cursorPosition", 0);
}
QString MarkdownEditor::markdown() const {
    if (!document_ || !edited_)
        return original_;
    auto result = document_->toMarkdown(QTextDocument::MarkdownDialectGitHub);
    // Qt appends a document terminator. It is not part of a single-line letter.
    while (result.endsWith('\n'))
        result.chop(1);
    return result;
}
QTextCursor MarkdownEditor::cursor() const {
    QTextCursor result(document_);
    if (edit_ && document_) {
        const int last = document_->characterCount() - 1;
        const int start = qBound(0, edit_->property("selectionStart").toInt(), last);
        const int end = qBound(0, edit_->property("selectionEnd").toInt(), last);
        result.setPosition(start);
        result.setPosition(end, QTextCursor::KeepAnchor);
    }
    return result;
}
void MarkdownEditor::characterFormat(const QTextCharFormat &format) {
    if (!document_)
        return;
    auto selection = cursor();
    if (selection.hasSelection()) {
        selection.mergeCharFormat(format);
    } else if (edit_) {
        // QQuickTextSelection owns the control's insertion format; a separate
        // QTextCursor would change the document but not subsequent typing.
        QObject *selectionObject = edit_->property("cursorSelection").value<QObject *>();
        if (selectionObject) {
            QTextCharFormat merged = selection.charFormat();
            merged.merge(format);
            selectionObject->setProperty("font", merged.font());
        }
    }
}
void MarkdownEditor::bold() {
    if (!document_) return;
    QTextCharFormat format;
    format.setFontWeight(cursor().charFormat().fontWeight() == QFont::Bold ? QFont::Normal : QFont::Bold);
    characterFormat(format);
}
void MarkdownEditor::italic() {
    if (!document_) return;
    QTextCharFormat format;
    format.setFontItalic(!cursor().charFormat().fontItalic());
    characterFormat(format);
}
void MarkdownEditor::heading(int level) {
    if (!document_) return;
    auto selection = cursor();
    auto format = selection.blockFormat();
    level = qBound(0, level, 3);
    format.setHeadingLevel(level);
    selection.beginEditBlock();
    selection.setBlockFormat(format);
    QTextCharFormat text;
    text.setFontWeight(level ? QFont::Bold : QFont::Normal);
    text.setFontPointSize(level ? 20 - level * 2 : document_->defaultFont().pointSizeF());
    selection.select(QTextCursor::BlockUnderCursor);
    selection.mergeCharFormat(text);
    selection.endEditBlock();
}
void MarkdownEditor::list(bool ordered) {
    if (!document_) return;
    auto selection = cursor();
    selection.beginEditBlock();
    auto *existing = selection.currentList();
    const auto style = ordered ? QTextListFormat::ListDecimal : QTextListFormat::ListDisc;
    if (existing && existing->format().style() == style) {
        auto block = document_->findBlock(selection.selectionStart());
        while (block.isValid() && block.position() <= selection.selectionEnd()) {
            QTextCursor blockCursor(block);
            auto format = block.blockFormat();
            format.setObjectIndex(-1);
            format.setIndent(0);
            blockCursor.setBlockFormat(format);
            block = block.next();
        }
    } else {
        QTextListFormat format;
        format.setStyle(style);
        format.setIndent(1);
        selection.createList(format);
    }
    selection.endEditBlock();
}
void MarkdownEditor::quote() {
    if (!document_) return;
    auto selection = cursor();
    auto format = selection.blockFormat();
    const int level = format.property(QTextFormat::BlockQuoteLevel).toInt() ? 0 : 1;
    format.setProperty(QTextFormat::BlockQuoteLevel, level);
    format.setLeftMargin(level ? 24 : 0);
    selection.mergeBlockFormat(format);
}
void MarkdownEditor::code() {
    if (!document_) return;
    auto selection = cursor();
    const bool enabled = !selection.charFormat().fontFixedPitch();
    QTextCharFormat format;
    format.setFontFixedPitch(enabled);
    format.setFontFamilies(enabled ? QStringList{"monospace"} : document_->defaultFont().families());
    characterFormat(format);
}
bool MarkdownEditor::allowedLink(const QString &value) const {
    const QUrl url(value, QUrl::StrictMode);
    return url.isValid() && url.scheme().compare("https", Qt::CaseInsensitive) == 0 && !url.host().isEmpty() && url.userInfo().isEmpty();
}
void MarkdownEditor::link(const QString &url) {
    if (!document_ || !allowedLink(url)) return;
    auto selection = cursor();
    QTextCharFormat format;
    format.setAnchor(true);
    format.setAnchorHref(url);
    format.setFontUnderline(true);
    selection.beginEditBlock();
    if (selection.hasSelection())
        selection.mergeCharFormat(format);
    else
        selection.insertText(url, format);
    selection.endEditBlock();
}
void MarkdownEditor::pastePlainText() {
    if (!document_) return;
    auto selection = cursor();
    selection.insertText(QGuiApplication::clipboard()->text());
    if (edit_)
        edit_->setProperty("cursorPosition", selection.position());
}
}
