#!/usr/bin/env bash
set -euo pipefail
# Build in caller-selected scratch storage; do not modify system installations.
work_dir="${1:?Usage: build-dependencies.sh WORK_DIRECTORY INSTALL_PREFIX}"
prefix="${2:?An absolute install prefix is required}"
mkdir -p "$work_dir" "$prefix"
case "$prefix" in /*) ;; *) echo 'Use an absolute install prefix' >&2; exit 1;; esac
if [ ! -d "$work_dir/libsodium" ]; then
 git clone --depth 1 --branch 1.0.20-RELEASE https://github.com/jedisct1/libsodium.git "$work_dir/libsodium"
fi
if [ ! -d "$work_dir/sqlcipher" ]; then
 git clone --depth 1 --branch v4.6.1 https://github.com/sqlcipher/sqlcipher.git "$work_dir/sqlcipher"
fi
test "$(git -C "$work_dir/libsodium" rev-parse HEAD)" = 9511c982fb1d046470a8b42aa36556cdb7da15de
test "$(git -C "$work_dir/sqlcipher" rev-parse HEAD)" = c5bd336ece77922433aaf6d6fe8cf203b0c299d5
(
 cd "$work_dir/libsodium"
 ./autogen.sh
 ./configure --prefix="$prefix" --disable-shared --with-pic
 make -j4
 make install
)
(
 cd "$work_dir/sqlcipher"
 ./configure --prefix="$prefix" --disable-shared --with-pic --enable-tempstore=yes \
  CFLAGS="-DSQLITE_HAS_CODEC $(pkg-config --cflags libcrypto)" \
  LDFLAGS="$(pkg-config --libs libcrypto)"
 make -j4
 make install
)
