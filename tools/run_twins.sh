#!/bin/bash
# Twin gate (hq#58, EOS-TWIN-1): run EVERY hosted twin harness
# (tools/*_twin_hosted.eigs) against the hosted EigenScript and pass only
# when all of them do. Before this, the twins were documented as gates
# ("run before touching demos/X") but nothing ran them, and all 13 had gone
# red against EigenScript main without anyone seeing it.
#
# A twin passes only when ALL hold:
#   - the interpreter exits 0 within TWIN_TIMEOUT seconds;
#   - its output has at least one PASS line (a twin that checks nothing
#     is not a pass) and no FAIL line (a twin that reports a failure but
#     exits 0 is not a pass either).
# The twin POPULATION is pinned: the glob must match exactly the PINNED
# list below. A twin that disappears, or a new one nobody enrolled, turns
# the gate red instead of passing on the survivors. Adding a twin means
# adding its name here in the same commit.
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

PINNED="desktop dhcp icmp inet kbd net regionfs rtc spkr tidefs udp win_sat win_sheet"

die() { echo "twin-gate: FAIL: $*"; exit 1; }

case "$EIGS_BIN" in /*) ;; *) EIGS_BIN="$ROOT/$EIGS_BIN" ;; esac
[ -x "$EIGS_BIN" ] || die "no hosted EigenScript at $EIGS_BIN (build it: make -C ../EigenScript)"

found=$(cd tools && ls -1 *_twin_hosted.eigs 2>/dev/null | sed 's/_twin_hosted\.eigs$//' | sort)
pinned=$(printf '%s\n' $PINNED | sort)
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
    npass=$(grep -c 'PASS' "$log")
    nfail=$(grep -c 'FAIL' "$log")
    why=""
    [ $rc -eq 0 ] || why="exit $rc"
    [ $rc -eq 124 ] && why="timed out after ${TWIN_TIMEOUT}s"
    [ "$npass" -gt 0 ] || why="${why:+$why, }no PASS line"
    [ "$nfail" -eq 0 ] || why="${why:+$why, }$nfail FAIL line(s)"
    if [ -n "$why" ]; then
        failed="$failed $t"
        echo "twin-gate: $t: FAILED ($why)"
        grep -E 'FAIL|Error|error' "$log" | head -8 | sed 's/^/    /'
        tail -n 4 "$log" | sed 's/^/    | /'
    else
        echo "twin-gate: $t: OK ($npass PASS$([ $staged -eq 1 ] && echo ', staged by stem'))"
    fi
done

npinned=$(printf '%s\n' $PINNED | wc -l)
echo "twin-gate: ran $ran twin(s); pinned population $npinned"
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
