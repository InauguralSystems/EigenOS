#!/usr/bin/env python3
"""M13 tape gate — prove net_recv REPLAYS from the M11 journal.

The oracle separates tape from wire by changing the world under the
replay: boot 1 records an ARP-probe session against slirp (the only
backend that answers for 10.0.2.2) into the journal on a scratch
store; boot 2 attaches the SAME disk to a responderless -netdev dgram
pair — a backend that cannot produce slirp's MAC — and replays. If the
replayed `net.arp_probe` still yields 52:55:0A:00:02:02, every frame
it parsed came off the tape, not the wire; the same boot's own
self-test independently proves the live wire is silent ("no ARP
responder").

Run from the repo root after `make iso`:
  python3 tools/net_tape_replay.py
Two sequential QEMU boots, ~4 minutes under TCG. Exits nonzero on FAIL.
"""
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, "build", "EigenOS.iso")
DISK = os.path.join(REPO, "build", "nettape.img")
MAC = "52:54:00:12:34:56"
RESPONDER = "52:55:0A:00:02:02"
BOOT_SECS = 80

fails = []


def check(tag, ok):
    print(f"  {'PASS' if ok else 'FAIL'}: {tag}")
    if not ok:
        fails.append(tag)


def start_qemu(extra, log):
    cmd = ["qemu-system-x86_64", "-m", "1024", "-cdrom", ISO,
           "-drive", f"file={DISK},format=raw,index=0,media=disk", "-boot", "d",
           "-display", "none", "-no-reboot",
           "-serial", f"file:{log}", "-monitor", "stdio"] + extra
    return subprocess.Popen(cmd, stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, text=True)


def mon(p, cmd):
    p.stdin.write(cmd + "\n")
    p.stdin.flush()


KEYMAP = {
    " ": "spc", ".": "dot", ",": "comma", "-": "minus", "=": "equal",
    "[": "bracket_left", "]": "bracket_right", "*": "shift-8",
    "+": "shift-equal", "!": "shift-1", '"': "shift-apostrophe",
    ":": "shift-semicolon", "_": "shift-minus", "(": "shift-9",
    ")": "shift-0", "<": "shift-comma", ">": "shift-dot",
}


def type_line(p, line, settle=2.0):
    for ch in line:
        key = KEYMAP.get(ch)
        if key is None and (ch.islower() or ch.isdigit()):
            key = ch
        if key is None:
            raise ValueError(f"no keymap entry for {ch!r}")
        mon(p, f"sendkey {key}")
        time.sleep(0.11)
    mon(p, "sendkey ret")
    time.sleep(settle)


def serial_text(log):
    try:
        with open(log, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def main():
    if not os.path.exists(ISO):
        print("build/EigenOS.iso missing — run `make iso` first")
        return 1
    subprocess.run(["qemu-img", "create", "-f", "raw", DISK, "64M"],
                   check=True, stdout=subprocess.DEVNULL)

    # ---- boot 1: record against slirp ----
    print("=== boot 1: record an ARP session into the journal (slirp) ===")
    log1 = os.path.join(REPO, "build", "nettape_1.log")
    if os.path.exists(log1):
        os.unlink(log1)
    p = start_qemu(["-netdev", "user,id=n0",
                    "-device", f"e1000,netdev=n0,mac={MAC}"], log1)
    time.sleep(BOOT_SECS)
    type_line(p, "blk_unlock of []")
    type_line(p, "fs_format of []", settle=4)
    type_line(p, "journal_on of []")
    type_line(p, "import net", settle=3)
    type_line(p, "r is net.arp_probe of [10, 0, 2, 2]", settle=6)
    type_line(p, "r", settle=2)
    type_line(p, "journal_off of []", settle=4)
    mon(p, "quit")
    p.wait(timeout=30)
    s1 = serial_text(log1)
    check("boot 1: live probe answered by slirp", RESPONDER in s1)
    check("boot 1: journal written to the store",
          "journal: written to the store" in s1)

    # ---- boot 2: replay against a backend that cannot answer ----
    print("=== boot 2: replay the session on a responderless backend ===")
    log2 = os.path.join(REPO, "build", "nettape_2.log")
    if os.path.exists(log2):
        os.unlink(log2)
    sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # discard-only
    sink.bind(("127.0.0.1", 0))
    hport = sink.getsockname()[1]
    qport = None
    qs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    qs.bind(("127.0.0.1", 0))
    qport = qs.getsockname()[1]
    qs.close()
    p = start_qemu(["-netdev",
                    ("dgram,id=n0,local.type=inet,local.host=127.0.0.1,"
                     f"local.port={qport},remote.type=inet,"
                     f"remote.host=127.0.0.1,remote.port={hport}"),
                    "-device", f"e1000,netdev=n0,mac={MAC}"], log2)
    time.sleep(BOOT_SECS)
    boot2_pre = serial_text(log2)
    type_line(p, "journal_replay of []", settle=25)
    mon(p, "quit")
    p.wait(timeout=30)
    sink.close()
    s2 = serial_text(log2)
    check("boot 2: the live wire is provably silent (self-test found "
          "no responder)", "no ARP responder within bound" in boot2_pre)
    replay = s2[len(boot2_pre):]
    check("boot 2: replay re-drove the probe line",
          "net.arp_probe of [10, 0, 2, 2]" in replay)
    check("boot 2: replayed probe yields the RECORDED responder — the "
          "frames came from the tape", RESPONDER in replay)

    print()
    if fails:
        print(f"net tape replay: {len(fails)} FAILURE(S): {fails}")
        return 1
    print("net tape replay: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
