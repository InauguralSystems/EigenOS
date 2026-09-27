#!/bin/sh
# hal_roots_check.sh <roots-file> <object>...
#
# The other half of EigenScript's freestanding contract (EigenScript#1181).
# Upstream's freestanding-check proves the runtime imports ONLY the HAL roots
# in tools/freestanding_hal_roots.txt. Nothing proved EigenOS DEFINES every
# one of them, so #1142 added pthread_once/pthread_self to the list, called
# them already owned here, and the kernel stopped linking weeks later.
#
# This fails naming every listed root that none of the given objects (the
# kernel, hal.o and the mini-libc; never the runtime's own objects) defines.
# It runs before the link, so a missing root is named here, not reported as
# an ld error, and a root no runtime call reaches yet is still caught.
# Only GLOBAL definitions count (a static function cannot satisfy the link).
# Residual: this proves each root is DEFINED, not that it behaves; a stub
# that returns 0 passes. Behaviour is the boot self-tests' job.
set -eu
roots_file=$1; shift
[ -f "$roots_file" ] || { echo "hal-roots: FAIL: no roots file at $roots_file"; exit 1; }
roots=$(grep -v '^[[:space:]]*#' "$roots_file" | tr -s ' \t' '\n' | sed '/^$/d' | sort -u)
n=$(printf '%s\n' "$roots" | sed '/^$/d' | wc -l)
[ "$n" -gt 0 ] || { echo "hal-roots: FAIL: $roots_file lists 0 roots (examined nothing)"; exit 1; }
defined=$(nm --defined-only "$@" 2>/dev/null | awk 'NF==3 && $2 ~ /^[TWVDBR]$/ {print $3}' | sort -u)
missing=$(printf '%s\n' "$roots" | while read -r r; do
    printf '%s\n' "$defined" | grep -qxF "$r" || printf '%s\n' "$r"
done)
if [ -n "$missing" ]; then
    echo "hal-roots: FAIL: EigenOS does not define $(printf '%s\n' "$missing" | wc -l) of $n HAL root(s) EigenScript requires:"
    printf '  %s\n' $missing
    echo "  (roots: $roots_file; implement each in src/hal.c)"
    exit 1
fi
echo "hal-roots: OK: all $n HAL roots defined"
