#!/bin/sh
# hal_roots_check.sh <roots-file> <provider-object>... -- <runtime-object>...
#
# The other half of EigenScript's freestanding contract (EigenScript#1181).
# Upstream's freestanding-check proves the runtime imports ONLY the HAL roots
# in tools/freestanding_hal_roots.txt. Nothing proved EigenOS DEFINES every
# one of them, so #1142 added pthread_once/pthread_self to the list, called
# them already owned here, and the kernel stopped linking weeks later.
#
# Common roots before the ledger's atomic header are mandatory, whether or not
# this particular runtime build imports them.  Atomic fallbacks after the
# header are target-dependent: x86-64 emits its atomics inline while RV32
# imports fallback libcalls.
set -eu
roots_file=$1; shift
[ -f "$roots_file" ] || { echo "hal-roots: FAIL: no roots file at $roots_file"; exit 1; }
providers=
while [ "$#" -gt 0 ] && [ "$1" != -- ]; do providers="$providers $1"; shift; done
[ "$#" -gt 0 ] || { echo "hal-roots: FAIL: missing -- runtime-object separator"; exit 1; }
shift
[ "$#" -gt 0 ] || { echo "hal-roots: FAIL: no runtime objects (examined nothing)"; exit 1; }
[ -n "$providers" ] || { echo "hal-roots: FAIL: no provider objects (examined nothing)"; exit 1; }

roots=$(grep -v '^[[:space:]]*#' "$roots_file" | tr -s ' \t' '\n' | sed '/^$/d' | sort -u)
ledger_n=$(printf '%s\n' "$roots" | sed '/^$/d' | wc -l | tr -d ' ')
[ "$ledger_n" -gt 0 ] || { echo "hal-roots: FAIL: $roots_file lists 0 roots (examined nothing)"; exit 1; }

atomic_header='# Atomic fallback roots'
header_n=$(awk -v header="$atomic_header" '
    { line=$0; sub(/^[[:space:]]*/, "", line); sub(/[[:space:]]*$/, "", line) }
    index(line, header) == 1 { n++ }
    END { print n+0 }
' "$roots_file")
[ "$header_n" -eq 1 ] || {
    echo "hal-roots: FAIL: expected exactly one '$atomic_header' header in $roots_file (found $header_n)"
    exit 1
}
mandatory=$(awk -v header="$atomic_header" '
    { line=$0; sub(/^[[:space:]]*/, "", line); sub(/[[:space:]]*$/, "", line) }
    index(line, header) == 1 { exit }
    line != "" && line !~ /^#/ { print line }
' "$roots_file" | sort -u)
atomic=$(awk -v header="$atomic_header" '
    { line=$0; sub(/^[[:space:]]*/, "", line); sub(/[[:space:]]*$/, "", line) }
    index(line, header) == 1 { after=1; next }
    after && line != "" && line !~ /^#/ { print line }
' "$roots_file" | sort -u)
mandatory_n=$(printf '%s\n' "$mandatory" | sed '/^$/d' | wc -l | tr -d ' ')
[ "$mandatory_n" -gt 0 ] || { echo "hal-roots: FAIL: ledger lists 0 mandatory roots before atomic header (examined nothing)"; exit 1; }

# An undefined symbol in one object may be defined by another runtime object.
# Subtract the runtime-wide definitions before selecting the HAL namespace.
runtime_undefined=$(nm -u "$@" 2>/dev/null |
    awk 'NF >= 2 && $NF != "_GLOBAL_OFFSET_TABLE_" {print $NF}' | sort -u)
runtime_defined=$(nm --defined-only "$@" 2>/dev/null |
    awk 'NF == 3 && $2 ~ /^[TWVDBR]$/ {print $3}' | sort -u)
imports=$(printf '%s\n' "$runtime_undefined" | while read -r symbol; do
    [ -n "$symbol" ] || continue
    printf '%s\n' "$runtime_defined" | grep -qxF "$symbol" || printf '%s\n' "$symbol"
done)
unexpected=$(printf '%s\n' "$imports" | grep -vxFf "$roots_file" || true)
if [ -n "$unexpected" ]; then
    echo "hal-roots: FAIL: runtime imports symbol(s) absent from $roots_file:"
    printf '  %s\n' $unexpected
    exit 1
fi
imported_atomic=$(printf '%s\n' "$atomic" | while read -r symbol; do
    [ -n "$symbol" ] || continue
    if printf '%s\n' "$imports" | grep -qxF "$symbol"; then printf '%s\n' "$symbol"; fi
done)
required=$(printf '%s\n%s\n' "$mandatory" "$imported_atomic" | sed '/^$/d' | sort -u)
required_n=$(printf '%s\n' "$required" | sed '/^$/d' | wc -l | tr -d ' ')
defined=$(nm --defined-only $providers 2>/dev/null |
    awk 'NF == 3 && $2 ~ /^[TWVDBR]$/ {print $3}' | sort -u)
missing=$(printf '%s\n' "$required" | while read -r r; do
    [ -n "$r" ] || continue
    printf '%s\n' "$defined" | grep -qxF "$r" || printf '%s\n' "$r"
done)
if [ -n "$missing" ]; then
    echo "hal-roots: FAIL: EigenOS does not define $(printf '%s\n' "$missing" | wc -l | tr -d ' ') of $required_n required HAL root(s):"
    printf '  %s\n' $missing
    echo "  (mandatory common roots plus imported atomic fallbacks; ledger: $roots_file)"
    exit 1
fi
echo "hal-roots: OK: all $required_n required HAL roots defined ($mandatory_n mandatory; $ledger_n total in ledger)"
