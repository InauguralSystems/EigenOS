#!/bin/bash
# Stop-hook gate: Claude Code may not end a turn with UNCOMMITTED kernel changes
# that do not build, link or pass the substrate audits. Clean tree (or changes
# outside the gated paths) exits 0 at once, so conversational stops cost nothing.
#
# Dirty src/ linker.ld Makefile grub.cfg tools/ demos/ -> the local half of CI:
#   make (includes tools/hal_roots_check.sh before the link — the check that
#   would have caught #1181's ~52 undefined symbols), make verify, and
#   tools/substrate_audit.sh. ~7 s incremental; ~100 s when ../EigenScript moved.
# The QEMU boot gate stays in CI (it needs minutes and a VM on a 4 GB box).
#
# A red is judged AGAINST HEAD: the same steps run on `git archive HEAD` in a
# temp dir, and a failure HEAD shares (an upstream EigenScript break, the #1181
# shape) is reported, not blocked. Only a regression this change made blocks
# the stop (exit 2). Refuses to start a second make in a tree already building.
#
# Escape hatch if the gate itself misbehaves: touch /tmp/eigenos_stop_gate_off.
set -u
[ -f /tmp/eigenos_stop_gate_off ] && exit 0
payload=$(cat 2>/dev/null)
cd "$(dirname "$0")/.." || exit 0
ROOT=$(pwd -P)
[ -n "$(git status --porcelain -- src linker.ld Makefile grub.cfg tools demos 2>/dev/null)" ] || exit 0

# Siblings (EigenScript, tidelog, EigenMiniSat, eigen-edit, eigen-sheet) are
# ../X from the MAIN checkout; a linked worktree elsewhere (~/src/wt/...) has no
# such neighbours, so resolve them next to the main checkout instead.
MAIN=$(cd "$(git rev-parse --git-common-dir)/.." && pwd -P)
PARENT=$(dirname "$MAIN")
VARS="EIGS=$PARENT/EigenScript TIDELOG=$PARENT/tidelog SAT=$PARENT/EigenMiniSat EDIT=$PARENT/eigen-edit SHEET=$PARENT/eigen-sheet"

for p in $(pgrep -x make 2>/dev/null); do
  if [ "$(readlink "/proc/$p/cwd" 2>/dev/null)" = "$ROOT" ]; then
    if printf '%s' "$payload" | grep -q '"stop_hook_active": *true'; then
      echo '{"systemMessage":"EigenOS stop gate: NOT checked — a make (pid '"$p"') was still running in this tree."}'
      exit 0
    fi
    echo "STOP GATE (EigenOS): a make (pid $p) is already running in this tree; a second one would race it. Wait for it to finish, then stop again." >&2
    exit 2
  fi
done

steps() {  # $1 = tree; prints the first failing step's name + log tail; rc = that step's number (1 make, 2 verify, 3 audit), 0 if all pass
  local t=$1
  ( cd "$t" && timeout 600 make -s $VARS ) > "$W/step.log" 2>&1 || { echo "make"; tail -12 "$W/step.log"; return 1; }
  ( cd "$t" && timeout 60 make -s verify $VARS ) > "$W/step.log" 2>&1 || { echo "make verify"; tail -6 "$W/step.log"; return 2; }
  ( cd "$t" && EIGENOS_MAKEVARS="$VARS" timeout 120 sh tools/substrate_audit.sh ) > "$W/step.log" 2>&1 || { echo "substrate_audit"; grep -v ': OK:' "$W/step.log" | tail -8; return 3; }
  return 0
}

W=$(mktemp -d "${TMPDIR:-/tmp}/eigenos-stopgate.XXXXXX") || exit 0
trap 'rm -rf "$W"' EXIT
new=$(steps "$ROOT"); new_rc=$?
[ $new_rc -eq 0 ] && exit 0
step=$(printf '%s\n' "$new" | head -1)

mkdir "$W/head"
git archive HEAD | tar -x -C "$W/head" 2>/dev/null || { echo "STOP GATE (EigenOS): '$step' failed and HEAD could not be extracted for the baseline:" >&2; printf '%s\n' "$new" | tail -n +2 >&2; exit 2; }
# Regression = HEAD gets PAST the step the working tree failed at (a HEAD that
# fails only at a LATER step still proves this change broke an earlier one).
head=$(steps "$W/head"); head_rc=$?
if [ $head_rc -eq 0 ] || [ $head_rc -gt $new_rc ]; then
  { echo "STOP GATE (EigenOS): '$step' fails on the working tree and passes on HEAD:"
    printf '%s\n' "$new" | tail -n +2
    echo "  (reproduce: make $VARS && make verify $VARS && EIGENOS_MAKEVARS=\"$VARS\" sh tools/substrate_audit.sh)"; } >&2
  exit 2
fi
echo "stop gate (EigenOS): '$step' fails, but HEAD fails at the same or an earlier step ('$(printf '%s\n' "$head" | head -1)') — pre-existing, not blocking." >&2
exit 0
