#!/bin/bash
# Self-test for tools/bundle_load_hosted.sh (hq#58, EOS-BUNDLE-1): each case
# copies this tree into a sandbox (beside links to the real sibling
# checkouts), plants ONE fault drawn from the defect class, runs the REAL
# gate there, and requires it to go red with the expected message. The
# unplanted sandbox must be green first.
#
#   bash tools/bundle_load_hosted_test.sh
# Needs GNU sed (-i) and GNU tar (--exclude), as on the ubuntu CI runner; it
# is not written for stock macOS userland.
# EIGS_BIN and the sibling checkouts resolve as for the gate.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd -P)
PARENT=$(dirname "$ROOT")
EIGS_BIN=${EIGS_BIN:-../EigenScript/src/eigenscript}
case "$EIGS_BIN" in /*) ;; *) EIGS_BIN="$ROOT/$EIGS_BIN" ;; esac
export EIGS_BIN

W=$(mktemp -d "${TMPDIR:-/tmp}/eigenos-bundle-test.XXXXXX") || exit 1
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
plant() {  # file sed-expression: the edit must change the file
    cp "$SB/$1" "$SB/../orig"
    sed -i "$2" "$SB/$1"
    if cmp -s "$SB/$1" "$SB/../orig"; then
        echo "plant on $1 changed nothing ($2)" > "$SB/../planterr"; return 1
    fi
}
# case NAME EXPECTED SETUP...: in the background, build sandbox NAME, run the
# SETUP command(s) in it ($SB is the sandbox root), then the real gate.
pids=""
case_() {
    local name=$1 want=$2; shift 2
    (
        sandbox "$name"
        eval "$*" || { echo 99 > "$SB/../rc"; exit 0; }
        bash "$SB/tools/bundle_load_hosted.sh" > "$SB/../out" 2>&1
        echo $? > "$SB/../rc"
    ) &
    pids="$pids $!"
    printf '%s\t%s\n' "$name" "$want" >> "$W/cases"
}

# 0. baseline: the unplanted copy is green.
case_ base "" true

# 1. a module dropped from the lint loop: the examined pin must notice.
case_ droplint "bundle-load: FAIL: lint examined" \
    "plant tools/bundle_load_hosted.sh '/^    stem=\$(basename \"\$f\" .eigs)\$/{n;s/^    \"\$EIGS_BIN\" --lint/    [ \"\$stem\" = life ] \\&\\& continue\\n&/}'"

# 2. a module dropped from the import loop.
case_ dropimp "bundle-load: FAIL: import examined" \
    "plant tools/bundle_load_hosted.sh 's/^    (cd \"\$ST\" \\&\\& timeout/    [ \"\$stem\" = life ] \\&\\& continue\\n&/'"

# 3. a parse error in a demo nothing at boot imports.
case_ parse "life: FAILED (lint: E002" \
    "printf '\\nbroken is (1 +\\n' >> \"\$SB/demos/life.eigs\""

# 4. an undefined name that is NOT a kernel builtin.
case_ undef "sched: FAILED (lint: E003" \
    "printf '\\ndefine planted() as:\\n    return no_such_builtin of []\\n' >> \"\$SB/demos/sched.eigs\""

# 5. a top-level runtime error (bool == number at module top level; layer 2).
case_ runtime "win_counter: FAILED (import: exit 1)" \
    "printf '\\nplanted is (1 < 2) == 1\\n' >> \"\$SB/demos/win_counter.eigs\""

# 5b. THE REAL SHAPE, inside a function body: win_vitals.eigs step() as it
#     was on main before this fix, comparing the observer's bool verdict to
#     a number. Import alone runs no function body, so only layer 3 sees it.
case_ stepbool "win_vitals: FAILED (step: exit 1)" \
    "plant demos/win_vitals.eigs 's/^    if stable of hp:\$/    if (stable of hp) == 1:/'"

# 5c. the same class in another task's step(), reached only after an idle
#     machine's empty key queue is drained (win_bounce draws when w > 0).
case_ stepbody "win_bounce: FAILED (step: exit 1)" \
    "plant demos/win_bounce.eigs 's/^        gfx_clear of \[16, 32, 28\]\$/        if (stable of x) == 1:\\n            x is 12\\n&/'"

# 5d. a task module dropped from the step loop: the step pin must notice.
case_ dropstep "bundle-load: FAIL: step examined" \
    "plant tools/bundle_load_hosted.sh 's/^    nstep=\$((nstep + 1))\$/&\\n    [ \"\$stem\" = win_vitals ] \\&\\& continue/'"

# 5e. lint exits nonzero with no error diagnostic (an empty JSON list): that
#     is not a clean lint. A fake interpreter does it for life.eigs only.
case_ lintexit "life: FAILED (lint: lint exited 1 with no error diagnostic)" \
    "printf '#!/bin/bash\\ncase \"\$*\" in *--lint*life.eigs) echo \"[]\"; exit 1;; esac\\nexec \"%s\" \"\$@\"\\n' \"\$EIGS_BIN\" > \"\$SB/../fake\" && chmod +x \"\$SB/../fake\" && export EIGS_BIN=\"\$SB/../fake\""

# 6. a kernel builtin that hal.c stops registering: its users must go red
#    (the kernel allowlist is derived from hal.c, not copied).
case_ kernel "net: FAILED (lint: E003" \
    "plant src/hal.c '/eigs_register_function(\"net_mac\",/d'"

# 7. a stale allowlist entry.
case_ stale "not in the bundle (stale; remove them): no_such_module" \
    "printf 'no_such_module -- planted\\n' >> \"\$SB/tools/bundle_load_allowlist.txt\""

# 8. an allowlist entry with no reason.
case_ noreason "malformed tools/bundle_load_allowlist.txt line(s)" \
    "printf 'life\\n' >> \"\$SB/tools/bundle_load_allowlist.txt\""

# shellcheck disable=SC2086
wait $pids
while IFS=$'\t' read -r name want; do
    d="$W/$name"
    rc=$(cat "$d/rc" 2>/dev/null || echo missing)
    if [ "$name" = base ]; then
        if [ "$rc" = 0 ]; then
            echo "BASELINE: green ($(grep 'examined' "$d/out"))"
        else
            echo "GATE DEFECT: the unplanted sandbox is not green (rc=$rc); plants prove nothing"
            tail -n 20 "$d/out" 2>/dev/null; exit 1
        fi
    elif [ "$rc" = 99 ]; then
        echo "GATE DEFECT: plant $name did not apply: $(cat "$d/planterr" 2>/dev/null)"
        fails=$((fails + 1))
    elif [ "$rc" != 0 ] && [ "$rc" != missing ] && grep -qF -- "$want" "$d/out"; then
        echo "PLANT $name: RED as required (rc=$rc; matched: $want)"
    else
        echo "GATE DEFECT: plant $name was NOT caught (rc=$rc; wanted '$want')"
        sed 's/^/    | /' "$d/out" 2>/dev/null | tail -n 12
        fails=$((fails + 1))
    fi
done < "$W/cases"

if [ $fails -ne 0 ]; then
    echo "bundle_load_hosted_test: FAIL: $fails plant(s) not caught"
    exit 1
fi
echo "bundle_load_hosted_test: PASS: baseline green, every plant red"
