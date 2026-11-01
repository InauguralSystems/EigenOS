#!/bin/bash
# Twin gate (hq#58, EOS-TWIN-1): run EVERY hosted twin harness
# (tools/*_twin_hosted.eigs) against the hosted EigenScript and pass only
# when all of them do. Before this, the twins were documented as gates
# ("run before touching demos/X") but nothing ran them, and all 13 had gone
# red against EigenScript main without anyone seeing it.
#
# A twin passes only when ALL hold:
#   - the interpreter exits 0 within TWIN_TIMEOUT seconds;
#   - its output has no FAIL line (a twin that reports a failure but exits
#     0 is not a pass);
#   - it printed EXACTLY its pinned number of per-check PASS lines. Only a
#     line that starts with PASS (after indentation) and then ':' or ' '
#     counts: that is the per-check line every twin's check() prints. The
#     summary banners ("net twin: ALL PASS", KBD_TWIN_ALL_PASS, ...) print
#     whenever no check failed, including when NO check ran, so they never
#     count. A twin whose checks are deleted, skipped or short-circuited
#     goes red instead of passing on its banner.
# The twin POPULATION is pinned too: the glob must match exactly the PINNED
# list below. A twin that disappears, or a new one nobody enrolled, turns
# the gate red instead of passing on the survivors. Adding a twin, or a
# check to a twin, means updating its entry here in the same commit; the
# pin is the reviewed record of how much each twin examines.
#
# How a twin runs:
#   - load_file twins resolve their subjects file-relative
#     (tools/../demos/X.eigs, tools/../../<sibling>/...), so they run in
#     place. Siblings are the checkouts beside this one, as for the Makefile.
#   - a twin that `import`s a demos module (dhcp/icmp/inet/udp) runs from a
#     staging directory holding demos/*.eigs flat, by stem: the way the ROM
#     bundle serves modules on the machine (tools/mkbundle.py).
#
#   bash tools/run_twins.sh
# EIGS_BIN overrides the interpreter (default ../EigenScript/src/eigenscript,
# like tools/boot_gate.sh). A gate that cannot run fails: no interpreter,
# or no twins.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd -P)
EIGS_BIN=${EIGS_BIN:-../EigenScript/src/eigenscript}
TWIN_TIMEOUT=${TWIN_TIMEOUT:-120}

# name:checks -- the per-check PASS lines each twin must print (measured
# 2026-11-01 against EigenScript 729e268e; hq#58).
PINNED="desktop:58 dhcp:21 icmp:21 inet:38 kbd:22 net:12 regionfs:74 rtc:16 spkr:9 tidefs:21 udp:16 win_sat:11 win_sheet:15"

die() { echo "twin-gate: FAIL: $*"; exit 1; }

case "$EIGS_BIN" in /*) ;; *) EIGS_BIN="$ROOT/$EIGS_BIN" ;; esac
[ -x "$EIGS_BIN" ] || die "no hosted EigenScript at $EIGS_BIN (build it: make -C ../EigenScript)"

found=$(cd tools && ls -1 *_twin_hosted.eigs 2>/dev/null | sed 's/_twin_hosted\.eigs$//' | sort)
pinned=$(printf '%s\n' $PINNED | sed 's/:.*//' | sort)
bad_pin=$(printf '%s\n' $PINNED | grep -vE '^[a-z_][a-z0-9_]*:[1-9][0-9]*$')
[ -z "$bad_pin" ] || die "malformed PINNED entr(y/ies) (want name:checks, checks > 0): $(echo $bad_pin)"
[ -n "$found" ] || die "no tools/*_twin_hosted.eigs found"
missing=$(comm -23 <(printf '%s\n' "$pinned") <(printf '%s\n' "$found"))
extra=$(comm -13 <(printf '%s\n' "$pinned") <(printf '%s\n' "$found"))

W=$(mktemp -d "${TMPDIR:-/tmp}/eigenos-twins.XXXXXX") || die "mktemp failed"
trap 'rm -rf "$W"' EXIT

failed=""
ran=0
for t in $found; do
    src="tools/${t}_twin_hosted.eigs"
    log="$W/$t.log"
    # Staged when the twin imports a module that lives in demos/.
    staged=0
    for m in $(sed -n 's/^import \([A-Za-z_][A-Za-z0-9_]*\)[[:space:]]*$/\1/p' "$src"); do
        [ -f "demos/$m.eigs" ] && staged=1
    done
    if [ $staged -eq 1 ]; then
        st="$W/stage-$t"
        mkdir -p "$st" && cp demos/*.eigs "$st/" && cp "$src" "$st/" || die "staging $t failed"
        (cd "$st" && timeout "$TWIN_TIMEOUT" "$EIGS_BIN" "${t}_twin_hosted.eigs") > "$log" 2>&1 < /dev/null
    else
        timeout "$TWIN_TIMEOUT" "$EIGS_BIN" "$src" > "$log" 2>&1 < /dev/null
    fi
    rc=$?
    ran=$((ran + 1))
    npass=$(grep -cE '^[[:space:]]*PASS[: ]' "$log")
    nfail=$(grep -c 'FAIL' "$log")
    want=$(printf '%s\n' $PINNED | sed -n "s/^$t://p")
    why=""
    [ $rc -eq 0 ] || why="exit $rc"
    [ $rc -eq 124 ] && why="timed out after ${TWIN_TIMEOUT}s"
    [ "$nfail" -eq 0 ] || why="${why:+$why, }$nfail FAIL line(s)"
    if [ -n "$want" ] && [ "$npass" -ne "$want" ]; then
        why="${why:+$why, }$npass check(s) passed, pinned $want"
    fi
    if [ -n "$why" ]; then
        failed="$failed $t"
        echo "twin-gate: $t: FAILED ($why)"
        grep -E 'FAIL|Error|error' "$log" | head -8 | sed 's/^/    /'
        tail -n 4 "$log" | sed 's/^/    | /'
    else
        echo "twin-gate: $t: OK ($npass of $want checks$([ $staged -eq 1 ] && echo ', staged by stem'))"
    fi
done

npinned=$(printf '%s\n' $PINNED | wc -l)
total=$(printf '%s\n' $PINNED | awk -F: '{n += $2} END {print n}')
echo "twin-gate: ran $ran twin(s); pinned population $npinned, $total checks"
rc=0
if [ -n "$missing" ]; then
    echo "twin-gate: FAIL: pinned twin(s) missing from tools/: $(echo $missing)"
    rc=1
fi
if [ -n "$extra" ]; then
    echo "twin-gate: FAIL: twin(s) not in the pinned list (enrol them in tools/run_twins.sh): $(echo $extra)"
    rc=1
fi
if [ -n "$failed" ]; then
    echo "twin-gate: FAIL: failing twin(s):$failed"
    rc=1
fi
[ $rc -eq 0 ] && echo "twin-gate: PASS: all $ran pinned twins green"
exit $rc
