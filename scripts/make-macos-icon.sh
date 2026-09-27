#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
output="${1:-$project_dir/assets/ynotbit.icns}"
# Keep the hand-tuned 1x/2x optical masters; never shrink the 1024px artwork.
mkdir -p "$(dirname "$output")"
iconutil -c icns "$project_dir/assets/ynotbit.iconset" -o "$output"
echo "$output"
