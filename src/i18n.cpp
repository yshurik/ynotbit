#include "i18n.h"
#include <QCoreApplication>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace bm {
const QVector<Language> &languages() {
    static const QVector<Language> all{
        {"en", "English"},        {"zh_CN", "简体中文"}, {"zh_TW", "繁體中文"},
        {"ja", "日本語"},          {"ko", "한국어"},      {"ru", "Русский"},
        {"uk", "Українська"},
    };
    return all;
}
QString resolveLanguage(const QString &choice) {
    for (const auto &language : languages())
        if (language.code == choice)
            return choice;
    const QLocale system;
    if (system.language() == QLocale::Chinese) {
        // Traditional script, or the regions that write it, get zh_TW.
        const auto territory = system.territory();
        const bool traditional = system.script() == QLocale::TraditionalHanScript ||
                                 territory == QLocale::Taiwan || territory == QLocale::HongKong ||
                                 territory == QLocale::Macao;
        return traditional ? "zh_TW" : "zh_CN";
    }
    const auto code = QLocale::languageToCode(system.language(), QLocale::ISO639Part1);
    for (const auto &language : languages())
        if (language.code == code)
            return code;
    return "en";
}
void installTranslations(const QString &choice) {
    const auto code = resolveLanguage(choice);
    // Dates and numbers follow the interface language, not the system's.
    QLocale::setDefault(QLocale(code == "en" ? QString("en_US") : code));
    if (code == "en")
        return; // the source strings are English
    // Qt's own strings (standard buttons, file dialogs) and ynotbit's; both are
    // compiled into the executable under :/i18n.
    for (const auto &name : {QString("qtbase_") + code, QString("ynotbit_") + code}) {
        auto translator = new QTranslator(qApp);
        if (translator->load(":/i18n/" + name))
            QCoreApplication::installTranslator(translator);
        else
            delete translator;
    }
}
QString formatDateTime(const QDateTime &when, bool seconds) {
    const QLocale locale;
    const auto time = seconds ? "HH:mm:ss" : "HH:mm";
    if (locale.language() == QLocale::English || locale.language() == QLocale::C)
        return QLocale::c().toString(when, QString("dd MMM yyyy · ") + time);
    return locale.toString(when.date(), QLocale::ShortFormat) + " · " +
           locale.toString(when.time(), time);
}
QString savedLanguage() {
    return QSettings().value("language").toString();
}
void saveLanguage(const QString &choice) {
    QSettings().setValue("language", choice);
}
} // namespace bm
