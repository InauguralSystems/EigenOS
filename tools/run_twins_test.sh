#!/bin/bash
# Self-test for tools/run_twins.sh (hq#58, EOS-TWIN-1): a gate that has never
# been seen red proves nothing. Each case copies this tree into a sandbox
# (beside links to the real sibling checkouts), plants ONE fault drawn from
# the defect class, runs the REAL gate there, and requires it to go red with
# the expected message. The unplanted sandbox must be green first, or a red
# plant would prove nothing either.
#
#   bash tools/run_twins_test.sh
# EIGS_BIN and the sibling checkouts resolve as for tools/run_twins.sh.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd -P)
PARENT=$(dirname "$ROOT")
EIGS_BIN=${EIGS_BIN:-../EigenScript/src/eigenscript}
case "$EIGS_BIN" in /*) ;; *) EIGS_BIN="$ROOT/$EIGS_BIN" ;; esac
export EIGS_BIN

W=$(mktemp -d "${TMPDIR:-/tmp}/eigenos-twins-test.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT
fails=0

sandbox() {  # fresh copy of this tree at $W/<case>/EigenOS, siblings linked beside it
    local d="$W/$1"
    rm -rf "$d"; mkdir -p "$d/EigenOS"
    (cd "$ROOT" && tar --exclude=./build --exclude=./.git -cf - .) | tar -x -C "$d/EigenOS"
    for s in EigenScript tidelog EigenMiniSat eigen-edit eigen-sheet; do
        [ -e "$PARENT/$s" ] && ln -s "$PARENT/$s" "$d/$s"
    done
    SB="$d/EigenOS"
}
gate() { bash "$SB/tools/run_twins.sh" > "$W/out" 2>&1; echo $? > "$W/rc"; }
expect_red() {  # case-name expected-substring
    local rc; rc=$(cat "$W/rc")
    if [ "$rc" -ne 0 ] && grep -qF -- "$2" "$W/out"; then
        echo "PLANT $1: RED as required (rc=$rc; matched: $2)"
    else
        echo "GATE DEFECT: plant $1 was NOT caught (rc=$rc; wanted '$2')"
        sed 's/^/    | /' "$W/out" | tail -n 12
        fails=$((fails + 1))
    fi
}
plant() {  # file sed-expression: the edit must change the file
    cp "$SB/$1" "$W/orig"
    sed -i "$2" "$SB/$1"
    if cmp -s "$SB/$1" "$W/orig"; then
        echo "GATE DEFECT: plant on $1 changed nothing ($2)"; fails=$((fails + 1)); return 1
    fi
}

# 0. baseline: the unplanted copy is green.
sandbox base; gate
if [ "$(cat "$W/rc")" -eq 0 ]; then
    echo "BASELINE: green ($(grep -c ': OK' "$W/out") twins OK)"
else
    echo "GATE DEFECT: the unplanted sandbox is not green; plants prove nothing"
    tail -n 20 "$W/out"; exit 1
fi

# 1. the bool-migration break, re-planted: desktop compares the observer's
#    bool verdict to a number again (EOS-BUNDLE-1, demos/desktop.eigs).
sandbox desktop; plant demos/desktop.eigs 's/^    if stalled:$/    if stalled == 1:/' && { gate; expect_red desktop-bool "desktop: FAILED"; }

# 2. a corrupted scancode row in the .eigs keyboard driver (the a..; row:
#    the twin asserts a, shift-8 and shift-[ but not every key of every row).
sandbox kbd; plant demos/kbd.eigs 's/\[0x1E,97, 0x1F,115/[0x1E,98, 0x1F,115/' && { gate; expect_red kbd-scancode "kbd: FAILED"; }

# 3. a twin deleted: the population pin must notice.
sandbox delete; rm "$SB/tools/spkr_twin_hosted.eigs"; gate; expect_red delete-twin "pinned twin(s) missing from tools/: spkr"

# 4. a twin nobody enrolled.
sandbox extra; cp "$SB/tools/rtc_twin_hosted.eigs" "$SB/tools/rtc2_twin_hosted.eigs"; gate; expect_red unenrolled-twin "not in the pinned list (enrol them in tools/run_twins.sh): rtc2"

# 5. a twin that reports a failure but exits 0.
sandbox fail0; printf '\nprint of "  FAIL: planted"\n' >> "$SB/tools/net_twin_hosted.eigs"; gate; expect_red fail-line-exit0 "net: FAILED (1 FAIL line(s))"

# 6. a twin that checks nothing and exits 0.
sandbox vacuous; printf 'print of "nothing checked"\n' > "$SB/tools/rtc_twin_hosted.eigs"; gate; expect_red vacuous-twin "rtc: FAILED (no PASS line)"

# 7. the module-isolation seam dropped: inet's stubs never reach the module.
sandbox seam; plant tools/inet_twin_hosted.eigs '/^seam of inet$/d' && { gate; expect_red seam-dropped "inet: FAILED"; }

# 8. a regression inside an imported (staged) module: wrong IPv4 TTL.
sandbox ttl; plant demos/inet.eigs 's/buf_set of \[f, 22, 64\]/buf_set of [f, 22, 63]/' && { gate; expect_red inet-ttl "inet: FAILED"; }

if [ $fails -ne 0 ]; then
    echo "run_twins_test: FAIL: $fails plant(s) not caught"
    exit 1
fi
echo "run_twins_test: PASS: baseline green, every plant red"
