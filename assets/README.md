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
- The notification-area icon has no files: `src/tray_icon.cpp` draws it, in one colour, from the
  logo's envelope, Y fold and shield. Drawn at eight times the size and scaled down, so any size
  a platform asks for is symmetric and crisp; 16 to 24 px have a squarer envelope, thinner lines
  and a solid shield, and the keyhole comes at 28 px, as in the logo's own optical sizes. macOS
  gets a black template image, which the menu bar tints for light and dark; Windows gets white or
  near-black by the taskbar's own theme (not the apps'); GNOME gets white, other Linux desktops
  follow the system's colour scheme.

Small PNGs are independent artwork, not resized versions of the SVG. Preserve
them when updating the identity. The design folder and its ZIP are an archived
design handoff; production integration is maintained here.
