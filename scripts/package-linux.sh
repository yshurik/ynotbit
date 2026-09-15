#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:?Usage: package-linux.sh BUILD_DIR OUTPUT_TARBALL QT_PREFIX}"
output_tar="${2:?An absolute output tar.gz path is required}"
qt_prefix="${3:?Qt installation prefix is required}"
case "$output_tar" in /*) ;; *) echo 'Use an absolute output path' >&2; exit 1;; esac

stage_dir="$(mktemp -d /tmp/ynotbit-package.XXXXXX)"
trap 'rm -rf "$stage_dir"' EXIT

tools_dir="$stage_dir/tools"
mkdir -p "$tools_dir"
curl -sL -o "$tools_dir/linuxdeploy-x86_64.AppImage" \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -sL -o "$tools_dir/linuxdeploy-plugin-qt-x86_64.AppImage" \
  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod +x "$tools_dir"/linuxdeploy-x86_64.AppImage "$tools_dir"/linuxdeploy-plugin-qt-x86_64.AppImage

rsvg-convert -w 256 -h 256 "$project_dir/assets/ynotbit.svg" -o "$stage_dir/ynotbit.png"
cat > "$stage_dir/ynotbit.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ynotbit
Exec=ynotbit
Icon=ynotbit
Categories=Network;Chat;
EOF

app_dir="$stage_dir/AppDir"
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$qt_prefix/bin/qmake6"
export PATH="$tools_dir:$PATH"
linuxdeploy-x86_64.AppImage \
  --appdir "$app_dir" \
  --executable "$build_dir/ynotbit" \
  --desktop-file "$stage_dir/ynotbit.desktop" \
  --icon-file "$stage_dir/ynotbit.png" \
  --plugin qt

mkdir -p "$app_dir/usr/share/licenses"
cp "$project_dir"/licenses/* "$app_dir/usr/share/licenses/"
cp "$project_dir/third_party/notbit/COPYING" "$app_dir/usr/share/licenses/notbit.txt"
cp "$project_dir/LICENSE" "$project_dir/THIRD_PARTY.md" "$app_dir/usr/share/licenses/"

mkdir -p "$(dirname "$output_tar")"
tar -C "$stage_dir" -czf "$output_tar" AppDir
echo "$output_tar"
