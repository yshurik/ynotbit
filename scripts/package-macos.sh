#!/usr/bin/env bash
set -euo pipefail
# Package outside synced folders: file-provider xattrs can invalidate signatures.
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:?Usage: package-macos.sh BUILD_DIR OUTPUT_ZIP QT_PREFIX}"
output_zip="${2:?An absolute output zip path is required}"
qt_prefix="${3:?Qt installation prefix is required}"
case "$output_zip" in /*) ;; *) echo 'Use an absolute output path' >&2; exit 1;; esac
stage_dir="$(mktemp -d /tmp/ynotbit-package.XXXXXX)"
trap 'rm -rf "$stage_dir"' EXIT
app_dir="$stage_dir/ynotbit.app"
ditto --norsrc "$build_dir/ynotbit.app" "$app_dir"
"$qt_prefix/bin/macdeployqt" "$app_dir" -qmldir="$project_dir/ui" -always-overwrite
# ynotbit uses SQLCipher directly, not Qt SQL drivers or QML LocalStorage.
rm -rf "$app_dir/Contents/PlugIns/sqldrivers" "$app_dir/Contents/Resources/qml/QtQuick/LocalStorage"
mkdir -p "$app_dir/Contents/Resources/licenses"
cp "$project_dir"/licenses/* "$app_dir/Contents/Resources/licenses/"
cp "$project_dir/third_party/notbit/COPYING" "$app_dir/Contents/Resources/licenses/notbit.txt"
cp "$project_dir/LICENSE" "$project_dir/THIRD_PARTY.md" "$app_dir/Contents/Resources/licenses/"
python3 - "$app_dir" <<'PY'
import pathlib, subprocess, sys
app=pathlib.Path(sys.argv[1])
count=0
for p in app.rglob('*'):
    if not p.is_file() or p.is_symlink(): continue
    if 'Mach-O' not in subprocess.check_output(['file','-b',str(p)],text=True): continue
    count+=1
    arches=subprocess.check_output(['lipo','-archs',str(p)],text=True).split()
    if 'arm64' not in arches: raise SystemExit(f'Missing arm64 architecture: {p}')
    if len(arches)>1:
        temp=p.with_name(p.name+'.thin')
        subprocess.run(['lipo',str(p),'-thin','arm64','-output',str(temp)],check=True)
        temp.replace(p)
    for line in subprocess.check_output(['otool','-L',str(p)],text=True).splitlines()[1:]:
        name=line.strip().split(' (')[0]
        if name.startswith('/') and not name.startswith(('/System/','/usr/lib/')):
            raise SystemExit(f'Unbundled dependency: {p}: {name}')
print(f'Checked {count} arm64 Mach-O files and their runtime dependencies')
PY
xattr -cr "$app_dir"
codesign --force --deep --sign - "$app_dir"
codesign --verify --deep --strict "$app_dir"
mkdir -p "$(dirname "$output_zip")"
ditto -c -k --norsrc --keepParent "$app_dir" "$output_zip"
echo "$output_zip"
