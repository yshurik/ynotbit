#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:?Usage: package-linux.sh BUILD_DIR OUTPUT_APPIMAGE QT_PREFIX}"
output="${2:?An absolute output .AppImage path is required}"
qt_prefix="${3:?Qt installation prefix is required}"
case "$output" in /*) ;; *) echo 'Use an absolute output path' >&2; exit 1;; esac

stage_dir="$(mktemp -d /tmp/ynotbit-package.XXXXXX)"
trap 'rm -rf "$stage_dir"' EXIT

tools_dir="$stage_dir/tools"
mkdir -p "$tools_dir"
curl -sL -o "$tools_dir/linuxdeploy-x86_64.AppImage" \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -sL -o "$tools_dir/linuxdeploy-plugin-qt-x86_64.AppImage" \
  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod +x "$tools_dir"/linuxdeploy-x86_64.AppImage "$tools_dir"/linuxdeploy-plugin-qt-x86_64.AppImage

cp "$project_dir/assets/icons/application/256.png" "$stage_dir/ynotbit.png"
cp "$project_dir/assets/ynotbit.desktop" "$stage_dir/ynotbit.desktop"

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

# Preserve the authored small-size artwork in desktop menus and launchers.
for size in 16 20 24 32 40 48 64 96 128 256 512 1024; do
  icon_dir="$app_dir/usr/share/icons/hicolor/${size}x${size}/apps"
  mkdir -p "$icon_dir"
  cp "$project_dir/assets/icons/application/$size.png" "$icon_dir/ynotbit.png"
done
mkdir -p "$app_dir/usr/share/icons/hicolor/scalable/apps"
cp "$project_dir/assets/ynotbit.svg" "$app_dir/usr/share/icons/hicolor/scalable/apps/"

mkdir -p "$app_dir/usr/share/licenses"
cp "$project_dir"/licenses/* "$app_dir/usr/share/licenses/"
cp "$project_dir/third_party/notbit/COPYING" "$app_dir/usr/share/licenses/notbit.txt"
cp "$project_dir/LICENSE" "$project_dir/THIRD_PARTY.md" "$app_dir/usr/share/licenses/"

# Second pass: the AppDir is complete (licenses included), so squash it into
# one self-contained, directly runnable file.
mkdir -p "$(dirname "$output")"
(cd "$stage_dir" && OUTPUT="$output" linuxdeploy-x86_64.AppImage --appdir "$app_dir" --output appimage)
echo "$output"
