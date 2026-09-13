#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
output="${1:-$project_dir/assets/ynotbit.icns}"
work_dir="$(mktemp -d /tmp/ynotbit-icon.XXXXXX)"
trap 'rm -rf "$work_dir"' EXIT
mkdir -p "$work_dir/ynotbit.iconset"
qlmanage -t -s 1024 -o "$work_dir" "$project_dir/assets/ynotbit.svg" >/dev/null 2>&1
rendered="$work_dir/ynotbit.svg.png"
test -f "$rendered"
for size in 16 32 128 256 512; do
  sips -z "$size" "$size" "$rendered" --out "$work_dir/ynotbit.iconset/icon_${size}x${size}.png" >/dev/null
  double=$((size * 2))
  sips -z "$double" "$double" "$rendered" --out "$work_dir/ynotbit.iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$work_dir/ynotbit.iconset" -o "$output"
echo "$output"
