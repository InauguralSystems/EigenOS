#!/bin/sh
# Boot gate (EigenOS#5): boot build/EigenOS.iso headless with a fresh disk
# holding a one-file regionfs store (tools/mkstore.eigs), capture COM1, and
# pass only when ALL hold:
#   - the REPL prompt `eigs>` is reached;
#   - no line reports a failure (FAIL/WRONG/FATAL/PANIC/abort() -> halt, or
#     an m10 line saying the store or the disk is missing);
#   - m10 mounted the store and listed exactly its one file;
#   - the number of OK verdict lines equals tools/boot_verdicts.expected.
# The disk carries a store because a blank one makes m10 print a line that is
# neither OK nor FAIL: the storage stack's only boot verdict would vanish and
# a broken mount would pass.
# The count pins the verdict POPULATION: a milestone that adds a self-test
# raises the number in the same commit, and a verdict that silently stops
# printing turns the gate red instead of passing on the survivors.
# A gate that cannot run fails: no qemu, no ISO, no hosted EigenScript to lay
# down the store, or an empty serial log.
#
#   sh tools/boot_gate.sh [serial-log-path]
# Uses KVM when /dev/kvm is writable (about 40 s to the prompt), else TCG
# (about 3 min on the dev box). BOOT_TIMEOUT overrides the limit in seconds.
set -u
cd "$(dirname "$0")/.."
ISO=build/EigenOS.iso
DISK=build/boot_gate.img
LOG=${1:-build/boot_gate.serial.log}
EXPECTED_FILE=tools/boot_verdicts.expected
QEMU=${QEMU:-qemu-system-x86_64}
EIGS_BIN=${EIGS_BIN:-../EigenScript/src/eigenscript}
die() { echo "boot-gate: FAIL: $*"; exit 1; }

command -v "$QEMU" >/dev/null 2>&1 || die "$QEMU not found"
command -v qemu-img >/dev/null 2>&1 || die "qemu-img not found"
[ -f "$ISO" ] || die "$ISO missing (run make iso first)"
[ -x "$EIGS_BIN" ] || die "hosted EigenScript $EIGS_BIN missing (make -C ../EigenScript)"
expected=$(grep -v '^#' "$EXPECTED_FILE" 2>/dev/null | tr -d ' \t' | grep -E '^[0-9]+$' | head -1)
[ -n "$expected" ] || die "no verdict count in $EXPECTED_FILE"

if [ -w /dev/kvm ]; then accel="-enable-kvm"; mode=KVM; timeout_s=${BOOT_TIMEOUT:-240}
else accel=""; mode=TCG; timeout_s=${BOOT_TIMEOUT:-900}; fi

# A fresh disk every run, with the store written into it: the m10 verdict
# depends on what the disk holds, and a guest mount may write to it.
rm -f "$DISK" "$LOG" "$DISK.sectors"
qemu-img create -q -f raw "$DISK" 64M || die "qemu-img create failed"
"$EIGS_BIN" tools/mkstore.eigs > "$DISK.sectors" || die "tools/mkstore.eigs failed"
python3 - "$DISK" "$DISK.sectors" <<'PY' || die "writing the store into $DISK failed"
import sys
disk, src = sys.argv[1], sys.argv[2]
n = 0
with open(disk, "r+b") as d, open(src) as f:
    for line in f:
        lba, hexdata = line.split()
        data = bytes.fromhex(hexdata)
        assert len(data) == 512, lba
        d.seek(int(lba) * 512)
        d.write(data)
        n += 1
if n == 0:
    sys.exit("mkstore produced no sectors")
print(f"boot-gate: wrote a {n}-sector regionfs store into {disk}")
PY
: > "$LOG"

echo "boot-gate: booting $ISO under $mode (limit ${timeout_s}s)"
# shellcheck disable=SC2086
"$QEMU" $accel -m 1024 -cdrom "$ISO" \
    -drive file="$DISK",format=raw,index=0,media=disk -boot d \
    -serial file:"$LOG" -display none -no-reboot &
qpid=$!

# Poll until the prompt appears, a fatal marker appears, QEMU exits, or time
# runs out. The self-tests all print before the prompt, so stopping at the
# prompt loses nothing. Fatal markers stop early: a halted kernel never
# prints the prompt and would otherwise burn the whole limit.
FATAL_RE='FATAL|PANIC|abort\(\) -> halt'
elapsed=0
outcome=timeout
while [ "$elapsed" -lt "$timeout_s" ]; do
    if grep -q 'eigs>' "$LOG"; then outcome=prompt; break; fi
    if grep -qE "$FATAL_RE" "$LOG"; then sleep 1; outcome=fatal; break; fi
    if ! kill -0 "$qpid" 2>/dev/null; then outcome=exited; break; fi
    sleep 1; elapsed=$((elapsed + 1))
done
kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
echo "boot-gate: stopped after ${elapsed}s ($outcome)"

echo "----- serial log ($LOG) -----"
cat "$LOG"
echo "----- end serial log -----"

fail=0
bad() { echo "boot-gate: FAIL: $*"; fail=1; }
[ -s "$LOG" ] || die "serial log is empty (the kernel never wrote COM1)"
[ "$outcome" = prompt ] || bad "the REPL prompt 'eigs>' was not reached ($outcome)"
BAD_RE="FAIL|WRONG|$FATAL_RE|^\\[m10\\] fs: no (store|disk)"
nbad=$(grep -cE "$BAD_RE" "$LOG")
if [ "$nbad" -ne 0 ]; then
    bad "$nbad failure line(s):"
    grep -E "$BAD_RE" "$LOG"
fi
# COM1 lines end in CRLF, so the exact match allows the \r.
grep -qE '^\[m10\] fs: regionfs mounted, 1 file\(s\) OK'"$(printf '\r')"'?$' "$LOG" ||
    bad "m10 did not mount the one-file store (want '[m10] fs: regionfs mounted, 1 file(s) OK')"
# A verdict line is a tagged self-test line carrying the word OK.
nok=$(grep -cE '^\[[a-z0-9.-]+\] .*\bOK\b' "$LOG")
if [ "$nok" -ne "$expected" ]; then
    bad "$nok OK verdict lines, expected $expected ($EXPECTED_FILE)"
else
    echo "boot-gate: OK: $nok OK verdict lines == $expected"
fi
[ "$fail" -eq 0 ] || exit 1
echo "boot-gate: PASS (prompt reached, no failures, $nok verdicts)"
