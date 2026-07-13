#!/usr/bin/env python3
"""M13 external frame verification — the OUTSIDE half of the net gates.

The boot self-tests prove the seam from inside the guest (rings up, PHY
loopback byte-exact, ARP round-trip against slirp). This harness proves
it from OUTSIDE the machine, where the guest cannot cheat:

  Phase A (TX capture): boot with the user-net backend and a
    filter-dump pcap on the netdev; assert the serial gates AND that
    the guest's ARP who-has appears in the externally captured pcap
    with exactly the bytes demos/net.eigs is specified to emit
    (broadcast dst, guest MAC src, ethertype 0x0806, opcode 1, target
    10.0.2.2, zero-padded to the 60-byte wire minimum).

  Phase B (RX injection): boot with a -netdev dgram UDP pair (no
    slirp, no responder — the boot log must report the honest "no ARP
    responder"), drive the REPL over the monitor (sendkey) to run
    net.echo_wait, inject a crafted 0x88B5 probe frame as a UDP
    datagram, and assert the guest echoes it back BYTE-EXACT with the
    MACs swapped — the injected bytes crossed the wire, the RX ring,
    the tape-participating net_recv seam, script parsing, and net_send
    back out, unmodified.

Run from the repo root (builds nothing — `make iso` first):
  python3 tools/net_frames.py
One QEMU at a time; ~4 minutes total under TCG. Exits nonzero on FAIL.
"""
import os
import socket
import struct
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, "build", "EigenOS.iso")
MAC = "52:54:00:12:34:56"
MACB = bytes.fromhex(MAC.replace(":", ""))
BOOT_SECS = 80            # TCG boot-ladder time before the REPL is up

fails = []


def check(tag, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}: {tag}" + (f" ({detail})" if detail and not ok else ""))
    if not ok:
        fails.append(tag)


# ---- the guest's expected ARP request (mirrors tools/net_twin_hosted) ----
def arp_request_bytes():
    f = b"\xff" * 6 + MACB + b"\x08\x06"
    f += b"\x00\x01\x08\x00\x06\x04\x00\x01"          # eth/ip, 6/4, request
    f += MACB + bytes([10, 0, 2, 15])                  # sender
    f += b"\x00" * 6 + bytes([10, 0, 2, 2])            # target
    return f                                           # 42 bytes pre-padding


def parse_pcap(path):
    with open(path, "rb") as fh:
        d = fh.read()
    frames = []
    off = 24
    while off + 16 <= len(d):
        _, _, caplen, _ = struct.unpack("<IIII", d[off:off + 16])
        frames.append(d[off + 16:off + 16 + caplen])
        off += 16 + caplen
    return frames


# ---- QEMU driving ----
def start_qemu(extra, log):
    cmd = ["qemu-system-x86_64", "-m", "1024", "-cdrom", ISO,
           "-display", "none", "-no-reboot",
           "-serial", f"file:{log}", "-monitor", "stdio"] + extra
    return subprocess.Popen(cmd, stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, text=True)


def mon(p, cmd):
    p.stdin.write(cmd + "\n")
    p.stdin.flush()


# The full char->keyname map (a missing case silently drops the char —
# this trap has bitten twice; keep the WHOLE list when copying).
KEYMAP = {
    " ": "spc", ".": "dot", ",": "comma", "-": "minus", "=": "equal",
    "[": "bracket_left", "]": "bracket_right", "*": "shift-8",
    "+": "shift-equal", "!": "shift-1", '"': "shift-apostrophe",
    ":": "shift-semicolon", "_": "shift-minus", "(": "shift-9",
    ")": "shift-0", "<": "shift-comma", ">": "shift-dot",
}


def type_line(p, line):
    for ch in line:
        if ch in KEYMAP:
            key = KEYMAP[ch]
        elif ch.isalnum() and ch.islower() or ch.isdigit():
            key = ch
        else:
            raise ValueError(f"no keymap entry for {ch!r}")
        mon(p, f"sendkey {key}")
        time.sleep(0.11)
    mon(p, "sendkey ret")
    time.sleep(0.3)


def serial_text(log):
    try:
        with open(log, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# ---------------- Phase A: TX capture ----------------
def phase_a():
    print("=== phase A: guest TX captured externally (user-net + pcap) ===")
    log = os.path.join(REPO, "build", "netframes_a.log")
    pcap = os.path.join(REPO, "build", "netframes_a.pcap")
    for f in (log, pcap):
        if os.path.exists(f):
            os.unlink(f)
    p = start_qemu(["-netdev", "user,id=n0",
                    "-device", f"e1000,netdev=n0,mac={MAC}",
                    "-object", f"filter-dump,id=fd0,netdev=n0,file={pcap}"], log)
    time.sleep(BOOT_SECS)
    mon(p, "quit")
    p.wait(timeout=30)

    s = serial_text(log)
    check("serial: NIC up with the expected MAC",
          f"[m13] e1000: 8254x up, MAC {MAC}" in s)
    check("serial: PHY loopback byte-exact",
          "PHY loopback 60-byte frame TX->RX byte-exact OK" in s)
    check("serial: script ARP round-trip against slirp",
          "arp who-has 10.0.2.2 -> reply from 52:55:0A:00:02:02" in s)

    frames = parse_pcap(pcap)
    want = arp_request_bytes()
    req = [f for f in frames if f[:6] == b"\xff" * 6 and f[6:12] == MACB
           and f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x01"]
    check("pcap: guest ARP request captured on the wire", len(req) >= 1,
          f"{len(frames)} frames total")
    if req:
        f = req[0]
        check("pcap: request bytes exact", f[:42] == want,
              f"got {f[:42].hex()}")
        check("pcap: zero-padded to the 60-byte wire minimum",
              len(f) == 60 and f[42:] == b"\x00" * 18, f"len={len(f)}")
    reply = [f for f in frames if f[:6] == MACB and f[12:14] == b"\x08\x06"
             and f[20:22] == b"\x00\x02"]
    check("pcap: slirp's ARP reply also on the wire", len(reply) >= 1)
    lb = [f for f in frames if f[12:14] == b"\x88\xb5"]
    check("pcap: loopback frame never leaked to the backend", len(lb) == 0)


# ---------------- Phase B: RX injection + echo ----------------
def phase_b():
    print("=== phase B: injected RX bytes echoed byte-exact (dgram pair) ===")
    log = os.path.join(REPO, "build", "netframes_b.log")
    if os.path.exists(log):
        os.unlink(log)
    hport = free_udp_port()
    qport = free_udp_port()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", hport))
    sock.settimeout(0.3)

    probe = MACB + bytes.fromhex("02aabbccdd01") + b"\x88\xb5"
    probe += b"EIGENOS-M13-RX-INJECTION-PROOF"
    probe += b"\x00" * (60 - len(probe))
    expect = probe[6:12] + MACB + probe[12:]          # MACs swapped

    p = start_qemu(["-netdev",
                    ("dgram,id=n0,local.type=inet,local.host=127.0.0.1,"
                     f"local.port={qport},remote.type=inet,"
                     f"remote.host=127.0.0.1,remote.port={hport}"),
                    "-device", f"e1000,netdev=n0,mac={MAC}"], log)

    def driver():
        time.sleep(BOOT_SECS)
        type_line(p, "import net")
        time.sleep(2)
        type_line(p, "net.echo_wait of 25000")

    t = threading.Thread(target=driver, daemon=True)
    t.start()
    t.join()

    got = None
    others = 0
    deadline = time.time() + 27
    while time.time() < deadline and got is None:
        sock.sendto(probe, ("127.0.0.1", qport))
        try:
            while True:
                d, _ = sock.recvfrom(4096)
                if d == expect:
                    got = d
                    break
                others += 1
        except socket.timeout:
            pass
    time.sleep(1)
    mon(p, "quit")
    p.wait(timeout=30)
    sock.close()

    s = serial_text(log)
    check("serial: honest no-responder verdict on a slirp-less backend",
          "no ARP responder within bound" in s)
    check("echo received, byte-exact with MACs swapped", got is not None,
          f"{others} other datagrams seen")


def main():
    if not os.path.exists(ISO):
        print("build/EigenOS.iso missing — run `make iso` first")
        return 1
    phase_a()
    phase_b()
    print()
    if fails:
        print(f"net frames: {len(fails)} FAILURE(S): {fails}")
        return 1
    print("net frames: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
