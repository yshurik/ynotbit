# Sound themes

A theme is a ZIP holding the WAV files the app plays for an event:

- `new-chat.wav` — a new chat message
- `new-mail.wav` — a new email

Optionally a `theme.json` with `{"id": "...", "name": "..."}` sets the folder
name and the label the Settings page shows. Files may sit at the ZIP's top
level or in one folder inside it. Themes are unpacked into
`<app data>/sound-themes/<id>/`.

`retro.zip` is the built-in theme. Its two clips are synthesized by
`scripts/make-default-theme.py` (48 kHz 16-bit PCM WAV): a lower two-note fall
for a new chat and a brighter ascending two-note blip for new mail. Run that
script after changing the tones and commit the regenerated archive; it is ours,
so it ships under the same terms as the rest of the project.

Nothing in this directory is a third party's recording, so the built-in theme
can be distributed with the app.
