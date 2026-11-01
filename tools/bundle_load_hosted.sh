#!/bin/bash
# Hosted bundle load gate (hq#58, EOS-BUNDLE-1): every module baked into the
# ROM bundle must at least load. The boot gate imports or spawns about 14 of
# them; the rest (desktop, draw, life, sched, the win_* windows, most of the
# stdlib) could break at parse or run time and still ship green, because
# tools/mkbundle.py copies bytes and checks nothing.
#
# The module list is DERIVED, never copied: BUNDLE_SRCS from the Makefile
# (the four build/gen/ concatenations are built first), staged flat by stem
# exactly as tools/mkbundle.py serves them on the machine (duplicate stems
# are a hard error there, so here too).
#
# Two layers, both over the whole bundle:
#   1. lint: `eigenscript --lint --json --lint-level error` on every module.
#      Any error fails: a parse error (E002), an undefined name (E003), ...
#      The one exception is E003 on a name the kernel itself provides: the
#      builtins src/hal.c registers with eigs_register_function, derived from
#      hal.c on every run. Those are the kernel seam, stubbed by name.
#   2. import: `import <stem>` in the staging directory, hosted, must exit 0
#      within LOAD_TIMEOUT seconds. A module whose top level cannot run hosted
#      is named in tools/bundle_load_allowlist.txt with its reason, and only
#      there; it is skipped by THIS layer only. An allowlist entry for a stem
#      that is not in the bundle fails the gate.
# The examined counts are pinned to the derived count: each layer must
# examine every module (lint N of N; import N minus allowlisted), so a module
# dropped from a loop turns the gate red instead of passing on the survivors.
# Failures name the module. EigenOS does not pin its siblings, so a red here
# can be an upstream change (EOS-UNPIN-1): the module name says where to look.
#
#   bash tools/bundle_load_hosted.sh
# EIGS_BIN overrides the interpreter (default ../EigenScript/src/eigenscript).
# EIGENOS_MAKEVARS: sibling paths for a checkout without ../EigenScript beside
# it, as for tools/substrate_audit.sh. A gate that cannot run fails.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd -P)
EIGS_BIN=${EIGS_BIN:-../EigenScript/src/eigenscript}
LOAD_TIMEOUT=${LOAD_TIMEOUT:-30}
ALLOW=tools/bundle_load_allowlist.txt

die() { echo "bundle-load: FAIL: $*"; exit 1; }

case "$EIGS_BIN" in /*) ;; *) EIGS_BIN="$ROOT/$EIGS_BIN" ;; esac
[ -x "$EIGS_BIN" ] || die "no hosted EigenScript at $EIGS_BIN (build it: make -C ../EigenScript)"
command -v python3 >/dev/null || die "python3 is required"
[ -f "$ALLOW" ] || die "missing $ALLOW"

# shellcheck disable=SC2086
mk() { env -u MAKEFLAGS -u MAKELEVEL make --no-print-directory -s ${EIGENOS_MAKEVARS:-} "$@"; }
srcs=$(printf 'print-BUNDLE_SRCS:\n\t@echo $(BUNDLE_SRCS)\n' | mk -f Makefile -f - print-BUNDLE_SRCS) \
    || die "could not derive BUNDLE_SRCS from the Makefile"
derived=$(printf '%s\n' $srcs | grep -c .)
[ "$derived" -gt 0 ] || die "derived an EMPTY BUNDLE_SRCS"
gen=$(printf '%s\n' $srcs | grep '^build/gen/')
if [ -n "$gen" ]; then
    # shellcheck disable=SC2086
    mk $gen || die "could not build the generated bundle modules: $(echo $gen)"
fi

kernel=$(sed -n 's/.*eigs_register_function("\([A-Za-z_][A-Za-z0-9_]*\)".*/\1/p' src/hal.c | sort -u)
nkernel=$(printf '%s\n' $kernel | grep -c .)
[ "$nkernel" -gt 0 ] || die "derived no kernel builtins from src/hal.c"

W=$(mktemp -d "${TMPDIR:-/tmp}/eigenos-bundle.XXXXXX") || die "mktemp failed"
trap 'rm -rf "$W"' EXIT
ST="$W/stage"
mkdir -p "$ST"
for f in $srcs; do
    [ -f "$f" ] || die "bundle source $f does not exist"
    stem=$(basename "$f" .eigs)
    [ -e "$ST/$stem.eigs" ] && die "duplicate module name '$stem' ($f)"
    cp "$f" "$ST/$stem.eigs"
done
printf '%s\n' $kernel > "$W/kernel.txt"

# The allowlist: "stem -- reason"; comments and blank lines ignored.
allow=$(sed -e 's/#.*//' "$ALLOW" | awk 'NF')
bad_allow=$(printf '%s\n' "$allow" | awk 'NF && !/^[A-Za-z_][A-Za-z0-9_]* -- [^ ]/')
[ -z "$bad_allow" ] || die "malformed $ALLOW line(s) (want 'stem -- reason'): $bad_allow"
allowed=$(printf '%s\n' "$allow" | awk 'NF {print $1}' | sort)
dup_allow=$(printf '%s\n' "$allowed" | uniq -d)
[ -z "$dup_allow" ] || die "duplicate $ALLOW entr(y/ies): $(echo $dup_allow)"

failed=""
fail() { failed="$failed $1"; echo "bundle-load: $1: FAILED ($2)"; }

# ---- layer 1: lint every module -----------------------------------------
linted=0
for f in "$ST"/*.eigs; do
    stem=$(basename "$f" .eigs)
    "$EIGS_BIN" --lint --json --lint-level error "$f" > "$W/lint.json" 2> "$W/lint.err" < /dev/null
    lrc=$?
    linted=$((linted + 1))
    verdict=$(python3 - "$W/lint.json" "$W/kernel.txt" "$lrc" <<'PY'
import json, re, sys
raw = open(sys.argv[1]).read()
kernel = set(open(sys.argv[2]).read().split())
lrc = int(sys.argv[3])
try:
    diags = json.loads(raw) if raw.strip() else []
except ValueError:
    print("lint output is not JSON (exit %d): %s" % (lrc, raw.strip()[:200]))
    sys.exit()
bad = []
for d in diags:
    if d.get("severity") != "error":
        continue
    m = re.search(r"undefined name '([^']+)'", d.get("message", ""))
    if d.get("code") == "E003" and m and m.group(1) in kernel:
        continue  # the kernel seam: a builtin hal.c registers
    bad.append("%s line %s: %s" % (d.get("code"), d.get("line"), d.get("message")))
if bad:
    print("; ".join(bad[:4]) + (" (+%d more)" % (len(bad) - 4) if len(bad) > 4 else ""))
elif lrc not in (0, 1):
    print("lint exited %d" % lrc)
PY
)
    [ -z "$verdict" ] || fail "$stem" "lint: $verdict"
done

# ---- layer 2: import every module not on the allowlist ------------------
imported=0
skipped=0
for f in "$ST"/*.eigs; do
    stem=$(basename "$f" .eigs)
    if printf '%s\n' "$allowed" | grep -qx "$stem"; then
        skipped=$((skipped + 1))
        continue
    fi
    (cd "$ST" && timeout "$LOAD_TIMEOUT" "$EIGS_BIN" -e "import $stem") > "$W/imp.log" 2>&1 < /dev/null
    irc=$?
    imported=$((imported + 1))
    if [ $irc -ne 0 ]; then
        why="import: exit $irc"
        [ $irc -eq 124 ] && why="import: timed out after ${LOAD_TIMEOUT}s"
        fail "$stem" "$why"
        grep -m1 -E '^Error|error' "$W/imp.log" | cut -c1-240 | sed 's/^/    /'
    fi
done

# ---- pins ----------------------------------------------------------------
rc=0
stale=$(comm -23 <(printf '%s\n' "$allowed") <(ls "$ST" | sed 's/\.eigs$//' | sort))
if [ -n "$stale" ]; then
    echo "bundle-load: FAIL: allowlist entr(y/ies) not in the bundle (stale; remove them): $(echo $stale)"
    rc=1
fi
nallowed=$(printf '%s\n' "$allowed" | grep -c .)
echo "bundle-load: derived $derived bundle module(s) from the Makefile, $nkernel kernel builtin(s) from src/hal.c"
echo "bundle-load: lint examined $linted of $derived; import examined $imported of $((derived - nallowed)) ($skipped allowlisted in $ALLOW)"
if [ "$linted" -ne "$derived" ]; then
    echo "bundle-load: FAIL: lint examined $linted modules, the bundle has $derived"
    rc=1
fi
if [ "$imported" -ne $((derived - nallowed)) ] || [ $((imported + skipped)) -ne "$derived" ]; then
    echo "bundle-load: FAIL: import examined $imported (+$skipped allowlisted) modules, the bundle has $derived"
    rc=1
fi
if [ -n "$failed" ]; then
    echo "bundle-load: FAIL: module(s) that do not load:$failed"
    rc=1
fi
[ $rc -eq 0 ] && echo "bundle-load: PASS: all $derived bundle modules lint clean; $imported import hosted"
exit $rc
