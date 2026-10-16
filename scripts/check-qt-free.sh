#!/usr/bin/env bash
# Builds and tests the Qt-free part (-DYNOTBIT_QT=OFF) in a plain Debian container that has no Qt.
# Usage: scripts/check-qt-free.sh [ssh-host]
# With a host the tree is streamed there and the container runs on that machine (docker needed).
set -euo pipefail
cd "$(dirname "$0")/.."
inner='
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake ninja-build pkg-config \
  ca-certificates curl libssl-dev libsodium-dev libsqlcipher-dev >/dev/null
if dpkg -l | grep -qi "qt6\|libqt\|qtbase"; then echo "Qt is installed in the container"; exit 1; fi
mkdir /work && cd /work && tar -x
cmake -S . -B /build -G Ninja -DYNOTBIT_QT=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build /build
ctest --test-dir /build --output-on-failure
echo "QT-FREE BUILD OK"
'
# COPYFILE_DISABLE=1 stops macOS bsdtar from storing extended attributes as AppleDouble `._*`
# members, which Linux GNU tar would extract as real files and the `src/*.c` glob would compile.
pack() { git ls-files -z --cached --others --exclude-standard | COPYFILE_DISABLE=1 tar --null -T - -cf -; }
if [ $# -ge 1 ]; then
    pack | ssh "$1" "docker run --rm -i debian:bookworm-slim bash -c '$inner'"
else
    pack | docker run --rm -i debian:bookworm-slim bash -c "$inner"
fi
