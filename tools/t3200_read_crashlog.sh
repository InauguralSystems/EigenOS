#!/usr/bin/env bash
# Read the EigenOS persistent-RAM crash log from the resident Linux on the
# target, after a warm reboot out of a hung/crashed EigenOS session.
#
# ONE-TIME SETUP on the target's Linux (then update-grub + reboot once):
#   /etc/default/grub:
#     GRUB_CMDLINE_LINUX_DEFAULT="... memmap=64K\$0x4000000 iomem=relaxed"
#   - memmap keeps Linux's allocator OUT of the buffer (or it's recycled
#     before you can read it). The backslash before $ is REQUIRED — grub
#     expands $... otherwise and the option silently becomes memmap=64K.
#     Verify after reboot: cat /proc/cmdline shows memmap=64K$0x4000000
#   - iomem=relaxed lets /dev/mem read RAM (Ubuntu ships STRICT_DEVMEM).
#
# Also usable against a QEMU dump: pass a file made with the monitor
# command `pmemsave 67108864 65536 /tmp/log.bin`:
#   tools/t3200_read_crashlog.sh /tmp/log.bin
set -euo pipefail

ADDR=$((0x4000000))
SIZE=$((0x10000))
SRC="${1:-}"

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

if [ -n "$SRC" ]; then
    cp "$SRC" "$TMP"
else
    if ! grep -q "iomem=relaxed" /proc/cmdline; then
        echo "warning: kernel cmdline lacks iomem=relaxed — /dev/mem reads" >&2
        echo "         of RAM will likely fail (see header of this script)" >&2
    fi
    sudo dd if=/dev/mem of="$TMP" bs=4096 skip=$((ADDR / 4096)) \
        count=$((SIZE / 4096)) status=none
fi

python3 - "$TMP" <<'EOF'
import struct, sys

raw = open(sys.argv[1], 'rb').read()
HDR = struct.calcsize('<8sIIII')
magic, cap, head, wrapped, boots = struct.unpack_from('<8sIIII', raw, 0)
if magic != b'EIGSLOG1':
    sys.exit("no EIGSLOG1 header at 0x4000000 — either EigenOS never ran, "
             "Linux recycled the region (memmap= missing?), or DRAM did not "
             "survive the reset")
data = raw[HDR:HDR + cap]
log = (data[head:] + data[:head]) if wrapped else data[:head]
sys.stderr.write(f"-- EigenOS crash log: boot #{boots}, "
                 f"{len(log)} bytes{' (ring wrapped)' if wrapped else ''} --\n")
sys.stdout.write(log.decode('ascii', errors='replace'))
EOF
