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
# the stop (exit 2), and so does a NEW error on a step HEAD also fails (errors
# compared, make -k on both sides). A pass-through is reported as a visible
# systemMessage. Refuses to start a second make in a tree already building.
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

# One gate at a time per tree: two stops close together (a session's own stop
# and a dispatcher-run stop, or two sessions) both passed the make-scan below
# and raced two builds into one build/ (seen live 2026-09-29). flock holds for
# the gate's whole run; the lock lives in this worktree's own git dir.
exec 9>"$(git rev-parse --git-dir)/stop_gate.lock" && flock -n 9 || {
  if printf '%s' "$payload" | grep -q '"stop_hook_active": *true'; then
    echo '{"systemMessage":"EigenOS stop gate: NOT checked — another stop gate was running in this tree."}'; exit 0
  fi
  echo "STOP GATE (EigenOS): another stop gate is already running in this tree; wait for it, then stop again." >&2; exit 2; }
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

steps() {  # $1 = tree, $2 = log prefix; prints the first failing step's name + log tail; rc = that step (1 make, 2 verify, 3 audit), 0 if all pass
  local t=$1 L=$2
  # -k: on a red, build everything that CAN build, so errors behind the first
  # one are in the log too (a HEAD that is already red must not hide them).
  ( cd "$t" && timeout 300 make -s -k $VARS ) > "$L.1" 2>&1 || { echo "make"; tail -12 "$L.1"; return 1; }
  ( cd "$t" && timeout 60 make -s verify $VARS ) > "$L.2" 2>&1 || { echo "make verify"; tail -6 "$L.2"; return 2; }
  ( cd "$t" && EIGENOS_MAKEVARS="$VARS" timeout 120 sh tools/substrate_audit.sh ) > "$L.3" 2>&1 || { echo "substrate_audit"; grep -v ': OK:' "$L.3" | tail -8; return 3; }
  return 0
}
sig() {  # error lines of step log $1, tree prefix $2 stripped so both trees compare
  grep -E 'error:|FAIL|undefined reference|^ +U mem' "$1" 2>/dev/null | sed "s#$2/##g" | sort -u
}
note() { jq -nc --arg m "EigenOS stop gate: $1" '{systemMessage: $m}'; }

W=$(mktemp -d "${TMPDIR:-/tmp}/eigenos-stopgate.XXXXXX") || exit 0
trap 'rm -rf "$W"' EXIT
new=$(steps "$ROOT" "$W/new"); new_rc=$?
[ $new_rc -eq 0 ] && exit 0
step=$(printf '%s\n' "$new" | head -1)

mkdir "$W/head"
git archive HEAD | tar -x -C "$W/head" 2>/dev/null || { echo "STOP GATE (EigenOS): '$step' failed and HEAD could not be extracted for the baseline:" >&2; printf '%s\n' "$new" | tail -n +2 >&2; exit 2; }
head=$(steps "$W/head" "$W/head"); head_rc=$?
block() {
  { echo "STOP GATE (EigenOS): '$step' $1"
    printf '%s\n' "$2"
    echo "  (reproduce: make $VARS && make verify $VARS && EIGENOS_MAKEVARS=\"$VARS\" sh tools/substrate_audit.sh)"; } >&2
  exit 2
}
# HEAD gets PAST the step the working tree failed at: this change broke it.
if [ $head_rc -eq 0 ] || [ $head_rc -gt $new_rc ]; then
  block "fails on the working tree and passes on HEAD:" "$(printf '%s\n' "$new" | tail -n +2)"
fi
# Same step red on both sides: compare the ERRORS, not the step. A new error
# alongside an inherited one (an upstream break) is still this change's.
if [ $head_rc -eq $new_rc ]; then
  fresh=$(comm -23 <(sig "$W/new.$new_rc" "$ROOT") <(sig "$W/head.$new_rc" "$W/head"))
  [ -n "$fresh" ] && block "fails on HEAD too, but with NEW errors this change added:" "$fresh"
fi
later=""; [ $new_rc -lt 3 ] && later=" Later steps (make verify / substrate_audit) did not run, so those checks did not happen."
note "'$step' fails, but HEAD fails there too with the same errors ('$(printf '%s\n' "$head" | head -1)') — pre-existing, not blocking.$later"
exit 0
