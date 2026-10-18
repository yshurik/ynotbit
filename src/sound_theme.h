#pragma once
#include <QString>
#include <QVector>
namespace bm {
// A place a theme can put a sound: the id the settings page and the arrivals
// report, and the file name the theme zip must use.
struct SoundEvent {
    QString id;
    QString file;
};
// A sound theme installed on disk. The built-in "retro" is copied out of the
// compiled-in zip on first use, so it is an ordinary theme everywhere else.
struct SoundTheme {
    QString id;
    QString name;
    QString dir;
    bool builtin = false;
};
// What the output device will render. Qt does not resample a sound effect, so a
// file outside this range is silent rather than wrong: it is refused at import.
// An unknown device (known() false) leaves only the file's own format checked.
struct AudioLimits {
    int minRate = 0;
    int maxRate = 0;
    int minChannels = 0;
    int maxChannels = 0;
    bool known() const { return minRate > 0 && maxRate >= minRate && minChannels > 0; }
};
// The events, in the order the settings page lists them. Labels are the
// settings page's business (it is the tr() context), so none live here.
QVector<SoundEvent> soundEvents();
// The file an event expects in a theme, or empty when there is no such event.
QString soundEventFile(const QString &event);
// Where themes live: <baseDir>/sound-themes, or the app data location when
// baseDir is empty.
QString soundThemesDir(const QString &baseDir = QString());
// The themes installed under that directory, the built-in one first and the
// rest by name. A directory without any known sound is not a theme.
QVector<SoundTheme> installedSoundThemes(const QString &baseDir = QString());
// The theme with this id from the installed list, or the first one, or an
// empty theme when none are installed.
SoundTheme findSoundTheme(const QString &id, const QString &baseDir = QString());
// The sound a theme gives an event, or empty when it does not provide one.
QString soundFileFor(const SoundTheme &theme, const QString &event);
// Whether a theme's sound can be played: a WAV, uncompressed PCM, 16-bit, and
// inside the output device's range. False, with *why set, when it cannot.
bool soundFileIsPlayable(const QByteArray &data, const QString &name, const AudioLimits &limits,
                         QString *why);
// Unpack a theme zip under <baseDir>/sound-themes/<id>. The known file names
// are taken from the zip's root or a folder inside it; other entries are
// ignored. A theme of the same id is replaced. False, with *error set, when the
// zip is unreadable, holds none of the known files, or a sound is one the
// output device could not play.
bool importSoundTheme(const QString &zipPath, const QString &baseDir, SoundTheme *out,
                      QString *error, const AudioLimits &limits = AudioLimits{});
// Writes the built-in "retro" theme out of the compiled-in zip. A copy already
// on disk is left alone unless it came from an older build; returns its record
// either way. "retro" is reserved: a user theme of that id is replaced.
SoundTheme ensureBuiltinTheme(const QString &baseDir = QString());
// The built-in theme's zip inside the Qt resource.
QString builtinThemeResource();
} // namespace bm
