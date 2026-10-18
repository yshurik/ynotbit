#!/bin/sh
# The compatibility gate for the Qt-free core: the previous release must open what this refactor
# writes, and this refactor must open what the previous release wrote -- for the vault and for the
# mailbox, whose on-disk bytes are the previous release's format.
#
# It builds a worktree of $SLICE2_BASE (the pre-slice commit; e65d76e by default) with the Qt app's
# bm::Vault and bm::Mailbox, and runs scripts/compat-open.cpp inside it beside the Qt-free
# tests/bmclient/storage_gen.cpp. Both write the same fixture and print the same report; each
# direction compares the two reports on the same files, and both read the frozen golden files under
# tests/golden the same way.
#
# Usage: SLICE2_BASE=<commit> scripts/compat-check.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SLICE2_BASE=${SLICE2_BASE:-e65d76e}
: "${PKG_CONFIG_PATH:?set PKG_CONFIG_PATH to the dependency install, e.g. <deps>/installed/lib/pkgconfig}"
# The Qt install and a checked-out secp256k1 source, so the worktree builds without a download.
QTDIR=${QTDIR:-/Users/user/w/ynotbit-deps/qt/6.8.3/macos}
if [ -z "${SECP256K1_SRC:-}" ]; then
    for candidate in "$ROOT/build/_deps/secp256k1-src" "$ROOT/build-core/_deps/secp256k1-src"; do
        [ -d "$candidate" ] && SECP256K1_SRC=$candidate
    done
fi
if [ ! -d "${SECP256K1_SRC:-/nonexistent}" ]; then
    echo "compat-check: no secp256k1 source; set SECP256K1_SRC (e.g. <build>/_deps/secp256k1-src)" >&2
    exit 2
fi

CORE_BUILD=${CORE_BUILD:-$ROOT/build-core}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/compat-check.XXXXXX")
BASE=$WORK/base
cleanup() {
    git -C "$ROOT" worktree remove --force "$BASE" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "compat-check: FAIL: $*" >&2
    exit 1
}
step() {
    echo "compat-check: $*"
}

# 1. The Qt-free side: the generator and its report.
step "building the Qt-free side ($CORE_BUILD)"
cmake -S "$ROOT" -B "$CORE_BUILD" -DYNOTBIT_QT=OFF >/dev/null
cmake --build "$CORE_BUILD" --target storage_gen >/dev/null
CORE=$CORE_BUILD/tests/bmclient/storage_gen

# 2. The previous release's side, in a throwaway worktree. Its CMake gets one target beside its
#    own tests.
step "building the previous release ($SLICE2_BASE) in $BASE"
git -C "$ROOT" worktree add --detach "$BASE" "$SLICE2_BASE" >/dev/null
cp "$ROOT/scripts/compat-open.cpp" "$BASE/tests/compat_open.cpp"
cat >>"$BASE/cmake/qt-app.cmake" <<'EOF'

add_executable(compat_open tests/compat_open.cpp)
target_link_libraries(compat_open PRIVATE bmcore)
EOF
cmake -S "$BASE" -B "$BASE/build" -DCMAKE_PREFIX_PATH="$QTDIR" \
    -DFETCHCONTENT_SOURCE_DIR_SECP256K1="$SECP256K1_SRC" >/dev/null
cmake --build "$BASE/build" --target compat_open >/dev/null
OLD=$BASE/build/compat_open

# 3. The same files, read by both. The writer's own report is compared with the reader's.
compare() {
    name=$1
    shift
    "$CORE" open "$@" >"$WORK/core.txt"
    "$OLD" open "$@" >"$WORK/old.txt"
    if ! diff -u "$WORK/core.txt" "$WORK/old.txt" >"$WORK/$name.diff"; then
        cat "$WORK/$name.diff" >&2
        fail "$name: the two sides read different things"
    fi
    step "ok: $name"
}

step "the core writes, the previous release reads"
mkdir -p "$WORK/written"
"$CORE" create "$WORK/written"
compare "core-written" "$WORK/written/dossier.bmvault" slice2-compat \
    "$WORK/written/dossier.bmmail" vault

step "the previous release writes, the core reads"
mkdir -p "$WORK/old-written"
"$OLD" create "$WORK/old-written"
compare "old-written" "$WORK/old-written/dossier.bmvault" slice2-compat \
    "$WORK/old-written/dossier.bmmail" vault

# 4. The frozen golden files: both sides must read them the same way. The mailboxes are keyed with
#    the 32 bytes 00..1f, not with a key in the vault. Opening a v1 vault or mailbox writes it up
#    to date, so each side gets its own copy and the frozen files stay frozen.
GOLDEN=$ROOT/tests/golden
GOLDEN_KEY=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f

compare_golden() {
    name=$1
    vault=$2
    mailbox=$3
    for side in core old; do
        mkdir -p "$WORK/$name-$side"
        cp "$GOLDEN/$vault" "$WORK/$name-$side/$vault"
        cp "$GOLDEN/$mailbox" "$WORK/$name-$side/$mailbox"
    done
    "$CORE" open "$WORK/$name-core/$vault" slice2-golden "$WORK/$name-core/$mailbox" "$GOLDEN_KEY" \
        >"$WORK/core.txt"
    "$OLD" open "$WORK/$name-old/$vault" slice2-golden "$WORK/$name-old/$mailbox" "$GOLDEN_KEY" \
        >"$WORK/old.txt"
    if ! diff -u "$WORK/core.txt" "$WORK/old.txt" >"$WORK/$name.diff"; then
        cat "$WORK/$name.diff" >&2
        fail "$name: the two sides read different things"
    fi
    step "ok: $name"
}

step "both sides read the frozen golden files"
compare_golden golden-vault-v2 vault-v2.bmvault mailbox-v2.bmmail
compare_golden golden-vault-v1 vault-v1.bmvault mailbox-v1.bmmail
compare_golden golden-vault-v2-old-label vault-v2-old-label.bmvault mailbox-v2.bmmail
# Opening those copies writes them up to date; the frozen originals must not have changed.
if ! git -C "$ROOT" diff --quiet -- tests/golden; then
    fail "opening the golden files wrote them: the frozen originals changed"
fi

step "PASS: the previous release and this refactor agree on the vault and the mailbox"
