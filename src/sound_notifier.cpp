#include "sound_notifier.h"
#include <QAudioDevice>
#include <QMediaDevices>
#include <QSettings>
#include <QSoundEffect>
#include <QUrl>
namespace bm {
namespace {
constexpr auto kEnabledKey = "sounds/enabled";
constexpr auto kThemeKey = "sounds/theme";
QString eventKey(const QString &event) {
    return "sounds/events/" + event;
}
} // namespace
SoundNotifier::SoundNotifier(QObject *parent) : QObject(parent) {}
SoundNotifier::~SoundNotifier() = default;
bool SoundNotifier::enabled() const {
    return QSettings().value(kEnabledKey, true).toBool();
}
void SoundNotifier::setEnabled(bool on) {
    QSettings().setValue(kEnabledKey, on);
    emit changed();
}
QString SoundNotifier::theme() const {
    return QSettings().value(kThemeKey).toString();
}
void SoundNotifier::setTheme(const QString &id) {
    QSettings().setValue(kThemeKey, id);
    emit changed();
}
bool SoundNotifier::eventEnabled(const QString &event) const {
    return QSettings().value(eventKey(event), true).toBool();
}
void SoundNotifier::setEventEnabled(const QString &event, bool on) {
    QSettings().setValue(eventKey(event), on);
    emit changed();
}
QVector<SoundTheme> SoundNotifier::themes() const {
    ensureBuiltinTheme(themesDir_);
    return installedSoundThemes(themesDir_);
}
SoundTheme SoundNotifier::currentTheme() const {
    ensureBuiltinTheme(themesDir_);
    return findSoundTheme(theme(), themesDir_);
}
void SoundNotifier::prepare() {
    ensureBuiltinTheme(themesDir_);
}
AudioLimits SoundNotifier::audioLimits() const {
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull())
        return {};
    return {device.minimumSampleRate(), device.maximumSampleRate(), device.minimumChannelCount(),
            device.maximumChannelCount()};
}
void SoundNotifier::notify(const QString &event) {
    if (!enabled() || !eventEnabled(event))
        return;
    play(event);
}
void SoundNotifier::preview(const QString &event) {
    play(event);
}
void SoundNotifier::play(const QString &event) {
    const QString path = soundFileFor(currentTheme(), event);
    if (path.isEmpty())
        return;
    auto *effect = effectFor(path);
    if (effect->status() == QSoundEffect::Ready)
        effect->play();
    else
        pending_.insert(path);
}
QSoundEffect *SoundNotifier::effectFor(const QString &path) {
    if (auto *existing = effects_.value(path))
        return existing;
    auto *effect = new QSoundEffect(this);
    effect->setSource(QUrl::fromLocalFile(path));
    effect->setVolume(1.0);
    // QSoundEffect ignores play() until its source is loaded: remember that one
    // was asked for and start it the moment the load finishes.
    connect(effect, &QSoundEffect::statusChanged, this, [this, path, effect] {
        if (effect->status() == QSoundEffect::Ready && pending_.remove(path))
            effect->play();
    });
    effects_.insert(path, effect);
    return effect;
}
} // namespace bm
