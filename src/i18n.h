#pragma once
#include <QDateTime>
#include <QString>
#include <QVector>

namespace bm {
struct Language {
    QString code;       // "ru", "zh_CN"; empty = follow the system
    QString nativeName; // as the language names itself, for the menu
};
// Every interface language shipped, English first.
const QVector<Language> &languages();
// The language to use for a saved choice: the choice itself, or for "" the
// shipped language closest to the system's (English when none matches).
QString resolveLanguage(const QString &choice);
// Installs ynotbit's and Qt's own translations for the saved choice on the
// running application. Call once, before any window is created.
void installTranslations(const QString &choice);
// A timestamp in the interface language: "29 Sep 2026 · 10:55" in English,
// the language's own short date elsewhere ("29.09.2026 · 10:55").
QString formatDateTime(const QDateTime &when, bool seconds = false);
// The saved choice ("" = system), and saving a new one (applies on restart).
QString savedLanguage();
void saveLanguage(const QString &choice);
} // namespace bm
