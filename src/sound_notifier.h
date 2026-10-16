#pragma once
#include "sound_theme.h"
#include <QHash>
#include <QObject>
#include <QSet>
class QSoundEffect;
namespace bm {
// Plays the chosen theme's sound when something arrives. The preferences are
// app-wide and live in QSettings, next to the appearance mode. The themes
// directory is set once at startup so playback never guesses a path.
class SoundNotifier : public QObject {
    Q_OBJECT
  public:
    explicit SoundNotifier(QObject *parent = nullptr);
    ~SoundNotifier() override;

    QString themesDir() const { return themesDir_; }
    void setThemesDir(const QString &dir) { themesDir_ = dir; }

    bool enabled() const;
    void setEnabled(bool on);

    // The chosen theme's id, or the built-in when nothing is chosen.
    QString theme() const;
    void setTheme(const QString &id);

    // Whether one event plays. Both default to on.
    bool eventEnabled(const QString &event) const;
    void setEventEnabled(const QString &event, bool on);

    // Play for an arrival: only when enabled and the event is on.
    void notify(const QString &event);
    // Play for the settings page's Preview button: ignores the enable switch.
    void preview(const QString &event);

    // The themes installed now, the built-in first.
    QVector<SoundTheme> themes() const;
    // The chosen theme, resolved against what is installed.
    SoundTheme currentTheme() const;
    // Make sure the built-in theme is on disk; called at startup.
    void prepare();

    // What the output device will render, for checking a theme at import: Qt
    // does not resample a sound effect, so a file outside this range is silent
    // rather than wrong. Unknown (known() false) when there is no output.
    AudioLimits audioLimits() const;

  signals:
    void changed();

  private:
    void play(const QString &event);
    QSoundEffect *effectFor(const QString &path);
    QString themesDir_;
    QHash<QString, QSoundEffect *> effects_;
    QSet<QString> pending_; // asked for before the effect finished loading
};
} // namespace bm
