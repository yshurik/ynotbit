#!/usr/bin/env bash
# Builds the bot image (docker/Dockerfile) from the tree as it is, uncommitted changes included.
# Usage: scripts/build-image.sh [ssh-host] [tag] [target]
# target: responder (the default), nickbot or sdk (the image a bot in another repository builds on;
# the labels come from bmbot/bmbot.h and from $YNOTBIT_VERSION, default dev).
# With a host the tree is streamed there and the image is built on that machine (docker needed
# there): a Mac builds for the machine that runs the container this way, whatever its CPU.
set -euo pipefail
cd "$(dirname "$0")/.."
host="${1:--}"
tag="${2:-ynotbit-bmbot:dev}"
target="${3:-responder}"
args=()
if [ "$target" = "sdk" ]; then
    abi="$(sed -n 's/^#define BMBOT_ABI_VERSION \([0-9][0-9]*\).*/\1/p' bmbot/bmbot.h)"
    [ -n "$abi" ] || { echo "no BMBOT_ABI_VERSION in bmbot/bmbot.h" >&2; exit 1; }
    args=(--build-arg "BMBOT_ABI=$abi" --build-arg "YNOTBIT_VERSION=${YNOTBIT_VERSION:-dev}")
fi
# COPYFILE_DISABLE=1 stops macOS bsdtar from storing extended attributes as AppleDouble `._*`
# members. Files deleted from the tree but still in the index are left out.
pack() {
    git ls-files -z --cached --others --exclude-standard |
        while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done |
        COPYFILE_DISABLE=1 tar --null -T - -cf -
}
if [ "$host" != "-" ]; then
    pack | ssh "$host" docker build -f docker/Dockerfile --target "$target" ${args[@]+"${args[@]}"} -t "$tag" -
else
    pack | docker build -f docker/Dockerfile --target "$target" ${args[@]+"${args[@]}"} -t "$tag" -
fi
