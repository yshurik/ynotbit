// The embedded translations load and translate, per language.
#include "i18n.h"
#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static QString translated(const QString &code, const char *context, const char *source) {
    QTranslator translator;
    require(translator.load(":/i18n/ynotbit_" + code), "a shipped language has a compiled file");
    return translator.translate(context, source);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        // Every shipped language translates the folder names, a Session
        // message and a storage error -- all three translation contexts.
        for (const auto &language : bm::languages()) {
            if (language.code == "en")
                continue;
            for (auto [context, source] : {std::pair{"bm::DesktopWindow", "Inbox"},
                                           {"bm::Session", "Open a mailbox first"},
                                           {"bm::Errors", "Vault is locked"}}) {
                const auto text = translated(language.code, context, source);
                require(!text.isEmpty() && text != source,
                        "every language translates every context");
            }
            QTranslator qt;
            require(qt.load(":/i18n/qtbase_" + language.code),
                    "Qt's own translations ship for standard buttons and dialogs");
        }
        require(translated("ru", "bm::DesktopWindow", "Inbox") == "Входящие",
                "Russian names the Inbox");
        {
            QTranslator ru;
            require(ru.load(":/i18n/ynotbit_ru"), "Russian loads");
            require(ru.translate("bm::DesktopWindow", "Sent") !=
                        ru.translate("bm::DesktopWindow", "Sent", "delivery state"),
                    "the Sent folder and the \"sent\" delivery state stay separate strings");
        }
        // The system language picks the closest shipped translation.
        const struct {
            const char *locale, *expect;
        } cases[] = {{"ru_RU", "ru"},       {"uk_UA", "uk"},      {"ja_JP", "ja"},
                     {"ko_KR", "ko"},       {"zh_CN", "zh_CN"},   {"zh_SG", "zh_CN"},
                     {"zh_TW", "zh_TW"},    {"zh_HK", "zh_TW"},   {"zh_Hant_US", "zh_TW"},
                     {"de_DE", "en"},       {"en_US", "en"}};
        for (const auto &c : cases) {
            QLocale::setDefault(QLocale(c.locale));
            require(bm::resolveLanguage({}) == c.expect,
                    "the system language maps to the right translation");
        }
        require(bm::resolveLanguage("ko") == "ko", "an explicit choice wins over the system");
        const QDateTime when(QDate(2026, 9, 29), QTime(10, 55, 7));
        QLocale::setDefault(QLocale("en_US"));
        require(bm::formatDateTime(when) == "29 Sep 2026 · 10:55" &&
                    bm::formatDateTime(when, true) == "29 Sep 2026 · 10:55:07",
                "English keeps the established date format");
        QLocale::setDefault(QLocale("ru_RU"));
        require(bm::formatDateTime(when) == "29.09.2026 · 10:55",
                "other languages use their own short date");
        std::cout << "PASS: translations load for every language; system language mapping\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
