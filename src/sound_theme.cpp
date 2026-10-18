#include "sound_theme.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <miniz.h>
namespace bm {
namespace {
// A single sound is small; a theme that claims more than this is not one.
constexpr mz_uint64 kMaxEntryBytes = 20 * 1024 * 1024;
// Raised when the shipped zip changes, so a copy from an older build is
// replaced on the next start. Kept beside the theme, not in it.
constexpr auto kBuiltinVersion = "4";
constexpr auto kBuiltinId = "retro";

// What may name a directory: the zip's own name, or an "id" field, reduced to
// a safe stem. Anything else falls back to "theme".
QString sanitizeId(const QString &raw) {
    QString id;
    for (const QChar c : raw.trimmed().toLower()) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            id += c;
        else if (!id.isEmpty() && !id.endsWith('-'))
            id += '-';
    }
    while (id.endsWith('-'))
        id.chop(1);
    return id.isEmpty() ? QStringLiteral("theme") : id;
}
QString readName(const QString &dir, const QString &fallback) {
    QFile file(dir + "/theme.json");
    if (!file.open(QIODevice::ReadOnly))
        return fallback;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto name = object.value("name").toString().trimmed();
    return name.isEmpty() ? fallback : name;
}
bool dirHasSound(const QString &dir) {
    for (const auto &event : soundEvents())
        if (QFile::exists(dir + '/' + event.file))
            return true;
    return false;
}
// The fields of a WAV that decide whether it can be played. Only the fmt chunk
// matters; the samples are never read here.
struct WavInfo {
    bool ok = false;
    int format = 0; // 1 is uncompressed PCM
    int channels = 0;
    int rate = 0;
    int bits = 0;
};
WavInfo readWavInfo(const QByteArray &data) {
    WavInfo info;
    if (data.size() < 44 || !data.startsWith("RIFF") || data.mid(8, 4) != "WAVE")
        return info;
    for (int pos = 12; pos + 8 <= data.size();) {
        const quint32 size = qFromLittleEndian<quint32>(data.constData() + pos + 4);
        const int body = pos + 8;
        if (data.mid(pos, 4) == "fmt ") {
            if (size < 16 || body + 16 > data.size())
                return info;
            const char *p = data.constData() + body;
            info.format = qFromLittleEndian<quint16>(p);
            info.channels = qFromLittleEndian<quint16>(p + 2);
            info.rate = int(qFromLittleEndian<quint32>(p + 4));
            info.bits = qFromLittleEndian<quint16>(p + 14);
            info.ok = true;
            return info;
        }
        pos = body + int(size) + int(size & 1); // chunks are word aligned
    }
    return info;
}
QString builtinStampPath(const QString &dir) {
    return dir + "/.builtin";
}
QString readStamp(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).trimmed();
}
} // namespace
QVector<SoundEvent> soundEvents() {
    return {
        {QStringLiteral("new-chat"), QStringLiteral("new-chat.wav")},
        {QStringLiteral("new-mail"), QStringLiteral("new-mail.wav")},
    };
}
QString soundEventFile(const QString &event) {
    for (const auto &e : soundEvents())
        if (e.id == event)
            return e.file;
    return {};
}
QString soundThemesDir(const QString &baseDir) {
    const QString root =
        baseDir.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
            : baseDir;
    return root + "/sound-themes";
}
QVector<SoundTheme> installedSoundThemes(const QString &baseDir) {
    QVector<SoundTheme> themes;
    const QString root = soundThemesDir(baseDir);
    const auto entries = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &id : entries) {
        const QString dir = root + '/' + id;
        if (!dirHasSound(dir))
            continue;
        themes.push_back({id, readName(dir, id), dir, id == QLatin1String(kBuiltinId)});
    }
    std::stable_sort(themes.begin(), themes.end(), [](const SoundTheme &a, const SoundTheme &b) {
        if (a.builtin != b.builtin)
            return a.builtin;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return themes;
}
SoundTheme findSoundTheme(const QString &id, const QString &baseDir) {
    const auto themes = installedSoundThemes(baseDir);
    for (const auto &theme : themes)
        if (theme.id == id)
            return theme;
    return themes.isEmpty() ? SoundTheme{} : themes.first();
}
QString soundFileFor(const SoundTheme &theme, const QString &event) {
    const QString file = soundEventFile(event);
    if (theme.dir.isEmpty() || file.isEmpty())
        return {};
    const QString path = theme.dir + '/' + file;
    return QFile::exists(path) ? path : QString();
}
bool soundFileIsPlayable(const QByteArray &data, const QString &name, const AudioLimits &limits,
                         QString *why) {
    const auto fail = [why](const QString &reason) {
        if (why)
            *why = reason;
        return false;
    };
    const WavInfo info = readWavInfo(data);
    if (!info.ok)
        return fail(QStringLiteral("%1 is not a WAV file").arg(name));
    if (info.format != 1)
        return fail(QStringLiteral("%1 is not uncompressed PCM audio").arg(name));
    if (info.bits != 16)
        return fail(QStringLiteral("%1 is %2-bit; 16-bit is required").arg(name).arg(info.bits));
    if (!limits.known())
        return true;
    if (info.rate < limits.minRate || info.rate > limits.maxRate)
        return fail(QStringLiteral("%1 is %2 Hz; the output device plays %3-%4 Hz")
                        .arg(name)
                        .arg(info.rate)
                        .arg(limits.minRate)
                        .arg(limits.maxRate));
    if (info.channels < limits.minChannels || info.channels > limits.maxChannels)
        return fail(QStringLiteral("%1 has %2 channels; the output device plays %3-%4")
                        .arg(name)
                        .arg(info.channels)
                        .arg(limits.minChannels)
                        .arg(limits.maxChannels));
    return true;
}
bool importSoundTheme(const QString &zipPath, const QString &baseDir, SoundTheme *out,
                      QString *error, const AudioLimits &limits) {
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, QFile::encodeName(zipPath).constData(), 0))
        return fail(QStringLiteral("The file is not a readable ZIP archive"));
    QHash<QString, mz_uint> wanted; // expected file name -> entry index
    mz_int64 themeJson = -1;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory)
            continue;
        const QString entry = QString::fromUtf8(stat.m_filename);
        if (entry.contains(QStringLiteral("..")) || entry.startsWith('/'))
            continue;
        const QString base = QFileInfo(entry).fileName().toLower();
        for (const auto &event : soundEvents())
            if (base == event.file)
                wanted.insert(event.file, i);
        if (base == QStringLiteral("theme.json"))
            themeJson = stat.m_file_index;
    }
    if (wanted.isEmpty()) {
        mz_zip_reader_end(&zip);
        return fail(QStringLiteral("The archive holds no new-chat.wav or new-mail.wav"));
    }
    QString id = sanitizeId(QFileInfo(zipPath).completeBaseName());
    QString name;
    if (themeJson >= 0) {
        size_t size = 0;
        if (void *data = mz_zip_reader_extract_to_heap(&zip, mz_uint(themeJson), &size, 0)) {
            const auto object =
                QJsonDocument::fromJson(QByteArray(static_cast<char *>(data), int(size))).object();
            mz_free(data);
            if (const auto raw = object.value("id").toString(); !raw.isEmpty())
                id = sanitizeId(raw);
            name = object.value("name").toString().trimmed();
        }
    }
    // Read every sound the archive provides before writing anything: a theme is
    // installed whole or not at all, and a sound the device cannot play is
    // refused here rather than played into silence later.
    QHash<QString, QByteArray> sounds;
    for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, it.value(), &stat) ||
            stat.m_uncomp_size > kMaxEntryBytes)
            continue;
        size_t size = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, it.value(), &size, 0);
        if (!data)
            continue;
        sounds.insert(it.key(), QByteArray(static_cast<const char *>(data), int(size)));
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    if (sounds.isEmpty())
        return fail(QStringLiteral("None of the archive's sounds could be read"));
    for (auto it = sounds.constBegin(); it != sounds.constEnd(); ++it) {
        QString why;
        if (!soundFileIsPlayable(it.value(), it.key(), limits, &why))
            return fail(why);
    }
    const QString dir = soundThemesDir(baseDir) + '/' + id;
    QDir(dir).removeRecursively();
    if (!QDir().mkpath(dir))
        return fail(QStringLiteral("Cannot write to the sound themes directory"));
    for (auto it = sounds.constBegin(); it != sounds.constEnd(); ++it) {
        QFile target(dir + '/' + it.key());
        if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            target.write(it.value()) != it.value().size()) {
            QDir(dir).removeRecursively();
            return fail(QStringLiteral("Cannot write the theme's sounds"));
        }
    }
    QFile manifest(dir + "/theme.json");
    if (manifest.open(QIODevice::WriteOnly | QIODevice::Truncate))
        manifest.write(
            QJsonDocument(QJsonObject{{"id", id}, {"name", name.isEmpty() ? id : name}}).toJson());
    if (out)
        *out = {id, name.isEmpty() ? id : name, dir, id == QLatin1String(kBuiltinId)};
    return true;
}
QString builtinThemeResource() {
    return QStringLiteral(":/sounds/retro.zip");
}
SoundTheme ensureBuiltinTheme(const QString &baseDir) {
    const QString dir = soundThemesDir(baseDir) + '/' + kBuiltinId;
    const QString fallbackName = QStringLiteral("Retro");
    if (dirHasSound(dir) && readStamp(builtinStampPath(dir)) == QLatin1String(kBuiltinVersion))
        return {QLatin1String(kBuiltinId), readName(dir, fallbackName), dir, true};
    QFile resource(builtinThemeResource());
    if (resource.open(QIODevice::ReadOnly))
        if (QTemporaryDir staging; staging.isValid()) {
            const QString zip = staging.filePath("retro.zip");
            QFile copy(zip);
            if (copy.open(QIODevice::WriteOnly)) {
                const bool written = copy.write(resource.readAll()) >= 0 && copy.flush();
                copy.close();
                SoundTheme imported;
                if (written && importSoundTheme(zip, baseDir, &imported, nullptr)) {
                    QFile stamp(builtinStampPath(imported.dir));
                    if (stamp.open(QIODevice::WriteOnly | QIODevice::Truncate))
                        stamp.write(kBuiltinVersion);
                    return imported;
                }
            }
        }
    return {QLatin1String(kBuiltinId), fallbackName, {}, true};
}
} // namespace bm
