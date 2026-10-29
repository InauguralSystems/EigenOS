#!/bin/sh
# hal_roots_check.sh <roots-file> <provider-object>... -- <runtime-object>...
#
# The other half of EigenScript's freestanding contract (EigenScript#1181).
# Upstream's freestanding-check proves the runtime imports ONLY the HAL roots
# in tools/freestanding_hal_roots.txt. Nothing proved EigenOS DEFINES every
# one of them, so #1142 added pthread_once/pthread_self to the list, called
# them already owned here, and the kernel stopped linking weeks later.
#
# The upstream ledger is cross-target.  The required set is the entries the
# runtime objects built for THIS target actually leave undefined; for example,
# x86-64 emits its atomics inline while RV32 imports fallback libcalls.
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

# An undefined symbol in one object may be defined by another runtime object.
# Subtract the runtime-wide definitions before selecting the HAL namespace.
runtime_undefined=$(nm -u "$@" 2>/dev/null | awk 'NF >= 2 {print $NF}' | sort -u)
runtime_defined=$(nm --defined-only "$@" 2>/dev/null |
    awk 'NF == 3 && $2 ~ /^[TWVDBR]$/ {print $3}' | sort -u)
imports=$(printf '%s\n' "$runtime_undefined" | while read -r symbol; do
    [ -n "$symbol" ] || continue
    printf '%s\n' "$runtime_defined" | grep -qxF "$symbol" || printf '%s\n' "$symbol"
done)
required=$(printf '%s\n' "$imports" | grep -Fxf "$roots_file" || true)
required_n=$(printf '%s\n' "$required" | sed '/^$/d' | wc -l | tr -d ' ')
defined=$(nm --defined-only $providers 2>/dev/null |
    awk 'NF == 3 && $2 ~ /^[TWVDBR]$/ {print $3}' | sort -u)
missing=$(printf '%s\n' "$required" | while read -r r; do
    [ -n "$r" ] || continue
    printf '%s\n' "$defined" | grep -qxF "$r" || printf '%s\n' "$r"
done)
if [ -n "$missing" ]; then
    echo "hal-roots: FAIL: EigenOS does not define $(printf '%s\n' "$missing" | wc -l | tr -d ' ') of $required_n imported HAL root(s):"
    printf '  %s\n' $missing
    echo "  (derived from built runtime objects; ledger: $roots_file)"
    exit 1
fi
echo "hal-roots: OK: all $required_n imported HAL roots defined ($ledger_n cross-target roots in ledger)"
