#!/usr/bin/env bash
# Copies the sources slice 1 ports out of Qt into tests/legacy/, as they were at <commit>, in the
# namespace `legacy`, so that bmclient can be compared with the code it replaces. tests/legacy is
# deleted at the end of the slice; tests/golden keeps what the old code answered.
# Usage: scripts/legacy-snapshot.sh <commit that still has the old src/ files>
set -euo pipefail
commit="${1:?usage: scripts/legacy-snapshot.sh <commit>}"
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/tests/legacy"
mkdir -p "$out"
show() { git -C "$root" show "$commit:src/$1"; }
rename() { perl -pe 's/^namespace bm\b/namespace legacy/'; }
for f in protocol.h protocol_wire.h pow.h; do
    show "$f" | rename > "$out/$f"
done
# protocol.cpp: the Protocol class only. What follows `struct WipedBytes` is Vault code.
{
    show protocol.cpp | awk '/^struct WipedBytes \{/ { exit } { print }' | rename
    echo "} // namespace legacy"
} > "$out/protocol.cpp"
show protocol_wire.cpp | rename > "$out/protocol_wire.cpp"
show pow.cpp | rename > "$out/pow.cpp"
# key_search.cpp: the phrase functions, up to the end of zeroBytes().
{
    show key_search.cpp | sed -n '1,128p' | rename
    echo "} // namespace legacy"
} > "$out/phrase.cpp"
echo "$commit" > "$out/COMMIT"
echo "snapshot of $commit in $out"
