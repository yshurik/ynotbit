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
  ca-certificates curl git file tcl passwd libssl-dev libsodium-dev >/dev/null
if dpkg -l | grep -qi "qt6\|libqt\|qtbase"; then echo "Qt is installed in the container"; exit 1; fi
# SQLCipher is built here rather than taken from the distribution: Debian bookworm ships 3.4.1,
# whose SQLite has no "INSERT ... ON CONFLICT ... DO UPDATE" and whose key derivation is not the
# one of SQLCipher 4, so every mailbox test would fail on the library rather than test the storage
# layer. This is the version the project itself builds (scripts/build-dependencies.sh).
# It is a shared library here, not the static one the project ships, so that linking it does not
# also need the private dependencies of SQLite (-lm -ldl -lpthread) on the link line.
git clone --depth 1 --branch v4.6.1 https://github.com/sqlcipher/sqlcipher /sqlcipher
test "$(git -C /sqlcipher rev-parse HEAD)" = c5bd336ece77922433aaf6d6fe8cf203b0c299d5
(
  cd /sqlcipher
  ./configure --prefix=/usr/local --enable-shared --enable-tempstore=yes \
    CFLAGS="-DSQLITE_HAS_CODEC $(pkg-config --cflags libcrypto)" \
    LDFLAGS="$(pkg-config --libs libcrypto)" >/dev/null
  make -j"$(nproc)" >/dev/null
  make install >/dev/null
)
ldconfig
mkdir /work && cd /work && tar -x
# The tests run as a normal user, not as root: a container is root by default, and root is not
# bound by the permission bits, so a test of a failed write would pass without testing anything.
useradd -m tester
chmod -R a+rX /work
mkdir /build && chown tester /build
cat > /run-tests <<RUN
set -e
export HOME=/home/tester
cmake -S /work -B /build -G Ninja -DYNOTBIT_QT=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build /build
ctest --test-dir /build --output-on-failure
RUN
chmod 755 /run-tests
su tester -c /run-tests
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
