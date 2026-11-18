#!/bin/sh
# Compare the truly-external symbols of liblatx_core.a with the allowlist.
# "Truly external" = undefined in some member and defined in no member, i.e.
# what a platform layer must supply (glib vocabulary, libm, TCG entry points,
# helper implementations that live in the parts we strip).
set -eu

NM=${NM:-/opt/homebrew/Cellar/llvm/23.1.5_1/bin/llvm-nm}
ARCHIVE=${1:-baremetal/build/liblatx_core.a}
ALLOW=${2:-baremetal/bare_externals.allowlist}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$NM" -A "$ARCHIVE" | awk -F: '
    NF < 2 { next }
    {
        n = split($NF, a, " ")
        if (a[1] == "U") { und[a[2]] = 1; next }
        if (n >= 1) { def[a[n]] = 1 }
    }
    END {
        for (s in und) if (!(s in def)) print s
    }
' | sort -u > "$work/external"

if [ ! -f "$ALLOW" ]; then
    echo "missing allowlist: $ALLOW" >&2
    exit 2
fi

sort -u "$ALLOW" > "$work/allow"
comm -23 "$work/external" "$work/allow" > "$work/new"
comm -13 "$work/external" "$work/allow" | grep -v '^#' > "$work/stale" || true

if [ -s "$work/new" ]; then
    echo "NEW external symbols (add only if intended):"
    sed 's/^/  /' "$work/new"
    exit 1
fi

echo "external symbols: $(wc -l < "$work/external" | tr -d ' '), all allowlisted"
if [ -s "$work/stale" ]; then
    echo "note: allowlist entries no longer needed: $(tr '\n' ' ' < "$work/stale")"
fi
