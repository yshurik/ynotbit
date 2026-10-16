#!/usr/bin/env python3
"""Write the built-in sound theme.

The two clips are synthesized here rather than taken from someone else's
recording, so the theme can be redistributed with the app. Run this after
changing the tones, then commit the regenerated assets/sounds/retro.zip:

    python3 scripts/make-default-theme.py

Only the standard library is used, and the output is deterministic: the same
tones always produce the same bytes.
"""
import io
import math
import os
import struct
import wave
import zipfile

RATE = 48000
PEAK = 0.85
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "sounds", "retro.zip")

# A lower two-note fall for a new chat, and a brighter ascending blip for new
# mail, so the two events are told apart by ear. Each note is
# (frequency, milliseconds).
THEME = {
    "new-chat.wav": {"kind": "bell", "decay": 5.0, "gap": 26,
                     "notes": [(784.0, 150), (587.3, 240)]},
    "new-mail.wav": {"kind": "square", "decay": 9.0, "gap": 22,
                     "notes": [(1046.5, 85), (1568.0, 110)]},
}
MANIFEST = '{\n  "id": "retro",\n  "name": "Retro"\n}\n'


def waveform(kind, phase):
    """One sample of the oscillator, phase in turns."""
    if kind == "square":
        return 1.0 if phase % 1.0 < 0.5 else -1.0
    # A struck bell: the fundamental with two quiet inharmonic partials.
    return (math.sin(2 * math.pi * phase)
            + 0.35 * math.sin(2 * math.pi * 2.01 * phase)
            + 0.12 * math.sin(2 * math.pi * 3.02 * phase))


def note(kind, freq, ms, decay):
    """A note with a short attack and an exponential decay, no click at either end."""
    frames = int(RATE * ms / 1000)
    attack = max(1, int(RATE * 0.004))
    release = max(1, int(RATE * 0.006))
    out = []
    for i in range(frames):
        t = i / RATE
        amp = min(1.0, i / attack, (frames - i) / release)
        out.append(amp * math.exp(-decay * t) * waveform(kind, freq * t))
    return out


def clip(spec):
    """The notes and the silence between them."""
    samples = []
    for index, (freq, ms) in enumerate(spec["notes"]):
        if index:
            samples.extend([0.0] * int(RATE * spec["gap"] / 1000))
        samples.extend(note(spec["kind"], freq, ms, spec["decay"]))
    peak = max(abs(s) for s in samples) or 1.0
    return [s / peak * PEAK for s in samples]


def wav_bytes(samples):
    """16-bit PCM mono, which is what the theme reader accepts."""
    pcm = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in samples)
    stream = io.BytesIO()
    with wave.open(stream, "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(RATE)
        handle.writeframes(pcm)
    return stream.getvalue()


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    clips = {name: wav_bytes(clip(spec)) for name, spec in THEME.items()}
    # A fixed timestamp keeps the archive byte-for-byte reproducible.
    stamp = (2026, 1, 1, 0, 0, 0)
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(clips):
            info = zipfile.ZipInfo(name, stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, clips[name])
        manifest = zipfile.ZipInfo("theme.json", stamp)
        manifest.compress_type = zipfile.ZIP_DEFLATED
        manifest.external_attr = 0o644 << 16
        archive.writestr(manifest, MANIFEST)
    print("wrote", OUT)
    for name in sorted(clips):
        print("  %-14s %6d bytes" % (name, len(clips[name])))


if __name__ == "__main__":
    main()
