#!/bin/sh
# Compare truly-external symbols of liblata_core.a with the allowlist.
# Expected external surface: libc only; the QEMU/tcg/helper boundary is
# satisfied by weak defaults in lata/runtime.c.
set -eu

NM=${NM:-/opt/homebrew/Cellar/llvm/23.1.5_1/bin/llvm-nm}
ARCHIVE=${1:-lata/build/liblata_core.a}
ALLOW=${2:-lata/lata_externals.allowlist}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$NM" -A "$ARCHIVE" | awk -F: '
    NF < 2 { next }
    {
        n = split($NF, a, " ")
        if (a[1] == "U") { und[a[2]] = 1; next }
        if (n >= 1) { def[a[n]] = 1 }
    }
    END { for (s in und) if (!(s in def)) print s }
' | sort -u > "$work/external"

[ -f "$ALLOW" ] || { echo "missing allowlist: $ALLOW" >&2; exit 2; }
sort -u "$ALLOW" > "$work/allow"
comm -23 "$work/external" "$work/allow" > "$work/new"
comm -13 "$work/external" "$work/allow" | grep -v '^#' > "$work/stale" || true

if [ -s "$work/new" ]; then
    echo "NEW external symbols:"
    sed 's/^/  /' "$work/new"
    exit 1
fi
if [ -s "$work/stale" ]; then
    echo "STALE allowlist entries:" >&2
    sed 's/^/  /' "$work/stale" >&2
fi
echo "external symbols: $(wc -l < "$work/external" | tr -d ' '), all allowlisted"
