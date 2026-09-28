#!/bin/sh
# Substrate audits on a built kernel (EigenOS#5). Each one pins an invariant
# the hosted EigenScript suite cannot see:
#   1. Kernel objects contain no SSE and no reference to mem* (the ISR
#      trampoline saves GP registers only; the mini-libc mem* are SSE-built).
#   2. The image contains no post-Merom instruction (the T3200 is a Penryn:
#      no RDRAND, no SSE4.1 PTEST, no POPCNT, no SSE4.2 CRC32).
#   3. PT_TLS memsz, rounded to 64 plus the 8-byte TCB, fits g_tls_block.
# Every audit fails when it cannot measure (missing object, no disassembly,
# no PT_TLS header): an audit that examined nothing is not a pass.
set -u
cd "$(dirname "$0")/.."
ELF=build/EigenOS.elf
fail=0
bad() { echo "substrate-audit: FAIL: $*"; fail=1; }

[ -f "$ELF" ] || { echo "substrate-audit: FAIL: $ELF missing (run make all first)"; exit 1; }

# 1. Kernel-flags objects: every object in the Makefile's OBJS (built with
# CFLAGS, -mno-sse) is checked, a superset of the IRQ-reachable ones. The set
# is asked of make, not restated here, so a new kernel object is audited from
# its first build. Two witnesses per object:
#   - no xmm register in the disassembly (the -mno-sse split held);
#   - no undefined reference to memcpy/memset/memmove/memcmp or their
#     eigs_ aliases (`nm -u`): those resolve to the SSE-built mini-libc. A
#     direct call or __builtin_memcpy with a variable size emits one (the
#     plant that proved this arm); -ffreestanding stops GCC 13's loop-idiom
#     rewrite today, but GCC documents that it may still emit these calls.
#     It has to be the symbol table: in an unlinked .o,
#     `objdump -d` prints every call as a jump into the caller itself, so a
#     `grep "call.*mem"` over it can never match.
# Residual: a non-IRQ kernel object that legitimately needs mem* would red
# here; waive it by name with the reason, never by weakening the check.
objs=$(printf 'print-OBJS:\n\t@echo $(OBJS)\n' |
       env -u MAKEFLAGS -u MAKELEVEL make --no-print-directory -s -f Makefile -f - print-OBJS)
nobj=0
for f in $objs; do
    nobj=$((nobj + 1))
    [ -f "$f" ] || { bad "$f missing"; continue; }
    dis=$(objdump -d "$f") || { bad "objdump failed on $f"; continue; }
    ninsn=$(printf '%s\n' "$dis" | grep -cE '^ +[0-9a-f]+:')
    [ "$ninsn" -gt 0 ] || { bad "$f disassembled to 0 instructions"; continue; }
    nx=$(printf '%s\n' "$dis" | grep -c 'xmm')
    nm_out=$(nm -u "$f") || { bad "nm failed on $f"; continue; }
    nmem=$(printf '%s\n' "$nm_out" | grep -cE '^ *U (eigs_)?(memcpy|memset|memmove|memcmp)$')
    if [ "$nx" -ne 0 ] || [ "$nmem" -ne 0 ]; then
        bad "$f: $nx xmm lines, $nmem mem* references (kernel objects stay GP-only)"
        printf '%s\n' "$nm_out" | grep -E '^ *U (eigs_)?(memcpy|memset|memmove|memcmp)$'
    else
        echo "substrate-audit: OK: $f ($ninsn insns, 0 xmm, 0 mem* refs)"
    fi
done
# The Makefile lists 19 kernel objects today; fewer than 10 means the
# derivation broke, not that the kernel shrank.
[ "$nobj" -ge 10 ] || bad "derived only $nobj kernel objects from the Makefile's OBJS"

# 2. Post-Merom instructions anywhere in the image.
dis=$(objdump -d "$ELF") || { bad "objdump failed on $ELF"; dis=; }
ninsn=$(printf '%s\n' "$dis" | grep -cE '^ +[0-9a-f]+:')
if [ "$ninsn" -gt 0 ]; then
    n=$(printf '%s\n' "$dis" | grep -cE '\s(rdrand|ptest|popcnt|crc32)')
    if [ "$n" -ne 0 ]; then
        bad "$ELF has $n post-Merom instructions (T3200 cannot run them):"
        printf '%s\n' "$dis" | grep -E '\s(rdrand|ptest|popcnt|crc32)' | head -5
    else
        echo "substrate-audit: OK: $ELF ($ninsn insns, 0 rdrand/ptest/popcnt/crc32)"
    fi
else
    bad "$ELF disassembled to 0 instructions"
fi

# 3. TLS fits the hand-rolled block. The block size is read from hal.c, not
# restated here, so growing the block does not need an edit in this script.
block=$(sed -n 's/.*g_tls_block\[\([0-9]*\)\].*/\1/p' src/hal.c | head -1)
memsz=$(readelf -lW "$ELF" | awk '$1 == "TLS" { print $6 }')
if [ -z "$block" ]; then
    bad "could not read g_tls_block's size from src/hal.c"
elif [ -z "$memsz" ]; then
    bad "$ELF has no PT_TLS header (readelf -l)"
else
    need=$(( ( ($memsz + 63) / 64 ) * 64 + 8 ))
    if [ "$need" -gt "$block" ]; then
        bad "TLS needs $need bytes (memsz $memsz) but g_tls_block is $block"
    else
        echo "substrate-audit: OK: TLS memsz $memsz -> $need of $block bytes"
    fi
fi

[ "$fail" -eq 0 ] || exit 1
echo "substrate-audit: all audits passed"
