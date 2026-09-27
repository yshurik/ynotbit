# Signal + Shield icons

Production assets come from `design/ynotbit-airmail-2026-09-27/final/`.

- `icons.qrc` embeds all application/window PNG optical masters in both the app
  and desktop tests. `appLogo()` supplies the header and application identity;
  `windowLogo()` supplies the main, message and composer window icons.
- `ynotbit.icns` is the macOS bundle/Dock icon. Regenerate with
  `scripts/make-macos-icon.sh`; it uses the authored `ynotbit.iconset`, preserving
  the distinct 1x and 2x versions. The macOS application does not override the
  native bundle icon with a Qt pixmap.
- `ynotbit.ico` is compiled into the Windows EXE through `ynotbit.rc.in`.
  The window PNGs are embedded through Qt, so they require no external files or
  ICO image plugin. `ynotbit-window.ico` is also retained as an export.
- Linux CMake installation and AppImage packaging install application PNGs in
  hicolor size directories, the scalable SVG, and `ynotbit.desktop`.

Small PNGs are independent artwork, not resized versions of the SVG. Preserve
them when updating the identity. The design folder and its ZIP are an archived
design handoff; production integration is maintained here.
