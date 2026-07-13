#!/usr/bin/env python3
"""M13/M13-2 external frame verification — the OUTSIDE half of the net
gates.

The boot self-tests prove the seam and the tower from inside the guest
(rings up, PHY loopback byte-exact, ARP round-trip, DHCP lease, ICMP
echo to the leased gateway). This harness proves it from OUTSIDE the
machine, where the guest cannot cheat:

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

  Phase C (M13-2, the tower vs slirp): user-net boot with a pcap on
    the netdev. Assert the m13-2 serial gates (lease + gateway echo),
    then validate the tower's TX bytes EXTERNALLY against
    net_tower_vectors.py (an independent Python implementation):
    DISCOVER and REQUEST byte-exact (xid read off the wire, idents
    known by construction), the ICMP echo requests byte-exact with
    slirp's replies beside them, and a REPL-driven udp.probe whose
    datagram reaches a real host UDP socket through slirp (payload
    exact, every checksum recomputed) and whose reply string surfaces
    on the guest serial.

  Phase D (M13-2, byte-exact serve on injection): dgram-pair boot
    (the boot log must report the honest no-offer verdict). Configure
    the tower by REPL, run icmp.serve then udp.echo_serve, and inject
    crafted frames: an ARP who-has must come back as a byte-exact ARP
    reply, an ICMP echo request as a checksum-valid echo reply, a UDP
    datagram echoed with ports mirrored and payload intact — each
    reply's IP ident is the guest's counter (read from the frame) and
    every checksum must verify.

Run from the repo root (builds nothing — `make iso` first):
  python3 tools/net_frames.py
One QEMU at a time; ~9 minutes total under TCG. Exits nonzero on FAIL.
"""
import os
import socket
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_tower_vectors as V

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, "build", "EigenOS.iso")
MAC = "52:54:00:12:34:56"
MACB = bytes.fromhex(MAC.replace(":", ""))
BOOT_SECS = 100           # TCG boot-ladder time before the REPL is up

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




# ---------------- Phase C: the tower vs slirp, verified outside ----------------
def ip_frames(frames, proto, sport=None, dport=None):
    """IPv4 frames of the given protocol (optionally UDP port-filtered)."""
    out = []
    for f in frames:
        if len(f) < 34 or f[12:14] != b"\x08\x00" or f[23] != proto:
            continue
        if proto == 17 and len(f) >= 42:
            sp, dp = struct.unpack(">HH", f[34:38])
            if sport is not None and sp != sport:
                continue
            if dport is not None and dp != dport:
                continue
        out.append(f)
    return out


def ip_payload(f):
    hl = (f[14] & 0xF) * 4
    tot = struct.unpack(">H", f[16:18])[0]
    return f[14 + hl:14 + tot]


def phase_c():
    print("=== phase C: the script tower vs slirp (pcap + host UDP echo) ===")
    log = os.path.join(REPO, "build", "netframes_c.log")
    pcap = os.path.join(REPO, "build", "netframes_c.pcap")
    for f in (log, pcap):
        if os.path.exists(f):
            os.unlink(f)

    # a real host UDP echo server, reached by the guest via 10.0.2.2
    esock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    esock.bind(("127.0.0.1", 0))
    eport = esock.getsockname()[1]
    esock.settimeout(0.5)
    got_probe = []

    stop = threading.Event()

    def echo_server():
        while not stop.is_set():
            try:
                d, a = esock.recvfrom(4096)
            except socket.timeout:
                continue
            got_probe.append(d)
            esock.sendto(b"M13-2-REPLY", a)

    t = threading.Thread(target=echo_server, daemon=True)
    t.start()

    p = start_qemu(["-netdev", "user,id=n0",
                    "-device", f"e1000,netdev=n0,mac={MAC}",
                    "-object", f"filter-dump,id=fd0,netdev=n0,file={pcap}"], log)
    time.sleep(BOOT_SECS)
    type_line(p, "import udp")
    time.sleep(3)
    type_line(p, f"udp.probe of [[10, 0, 2, 2], {eport}, 5000]")
    time.sleep(20)
    mon(p, "quit")
    p.wait(timeout=30)
    stop.set()
    t.join(timeout=3)
    esock.close()

    s = serial_text(log)
    check("serial: dhcp lease acquired through the script tower",
          "[m13-2] dhcp: lease 10.0.2.15 mask 255.255.255.0 gw 10.0.2.2 "
          "dns 10.0.2.3 server 10.0.2.2" in s)
    check("serial: icmp echo to the LEASED gateway",
          "[m13-2] icmp: echo 10.0.2.2 rtt" in s and "loss 0" in s)
    check("host echo server got the guest's datagram payload-exact",
          b"EIGENOS-M13-2-UDP" in got_probe,
          f"{len(got_probe)} datagrams seen")
    check("serial: the reply string surfaced at the guest REPL",
          "M13-2-REPLY" in s)

    frames = parse_pcap(pcap)

    # DHCP: read the xid off the wire, then demand byte-exact frames.
    # A client packet's BOOTP body sits after the 8-byte UDP header;
    # the magic cookie at body[236:240] and option 53 right behind it.
    def dhcp_mtype(f):
        seg = ip_payload(f)
        body = seg[8:]
        if len(body) < 243 or body[0] != 1 or body[236:240] != V.COOKIE:
            return 0
        if body[240] != 53 or body[241] != 1:
            return 0
        return body[242]

    client = ip_frames(frames, 17, sport=68, dport=67)
    disc = [f for f in client if dhcp_mtype(f) == 1]
    req = [f for f in client if dhcp_mtype(f) == 3]
    check("pcap: DISCOVER captured", len(disc) >= 1, f"{len(frames)} frames")
    check("pcap: REQUEST captured", len(req) >= 1)
    if disc and req:
        xid = struct.unpack(">I", ip_payload(disc[0])[8 + 4:8 + 8])[0]
        want_d = V.guest_dhcp_discover(xid, ident_ip=1)
        want_r = V.guest_dhcp_request(xid, ident_ip=2)
        check("pcap: DISCOVER byte-exact vs net_tower_vectors",
              disc[0] == want_d, disc[0].hex())
        check("pcap: REQUEST byte-exact (option 50 + 54)",
              req[0] == want_r, req[0].hex())
    offers = [f for f in frames if ip_frames([f], 17, sport=67, dport=68)]
    check("pcap: slirp's OFFER/ACK on the wire", len(offers) >= 2)

    # ICMP: the guest's echo requests are fully deterministic
    echoreq = [f for f in frames if ip_frames([f], 1)
               and ip_payload(f)[0:1] == b"\x08"]
    check("pcap: 3 ICMP echo requests", len(echoreq) == 3, f"{len(echoreq)}")
    ok = len(echoreq) == 3
    for i, f in enumerate(echoreq):
        if f != V.guest_icmp_echo_frame(ident_ip=3 + i, seq=1 + i):
            ok = False
    check("pcap: echo requests byte-exact (ident 3..5, seq 1..3)", ok)
    echorep = [f for f in frames if ip_frames([f], 1)
               and ip_payload(f)[0:1] == b"\x00"]
    check("pcap: slirp's echo replies beside them", len(echorep) >= 3)

    # UDP probe: payload exact + both checksums verify
    probes = [f for f in frames if ip_frames([f], 17, sport=5000, dport=eport)]
    check("pcap: udp.probe datagram on the wire", len(probes) >= 1)
    if probes:
        f = probes[0]
        seg = ip_payload(f)
        check("pcap: probe payload exact", seg[8:] == b"EIGENOS-M13-2-UDP")
        check("pcap: probe IP header checksum verifies",
              V.csum_ok(f[14:34]))
        check("pcap: probe UDP checksum verifies",
              V.udp_ok(f[26:30], f[30:34], seg))


# ---------------- Phase D: byte-exact serve on an injected wire ----------------
def phase_d():
    print("=== phase D: tower serves injected ARP/ICMP/UDP byte-exact ===")
    log = os.path.join(REPO, "build", "netframes_d.log")
    if os.path.exists(log):
        os.unlink(log)
    hport = free_udp_port()
    qport = free_udp_port()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", hport))
    sock.settimeout(0.3)

    PEER_MAC = bytes.fromhex("02aabbccdd01")
    PEER_IP = (10, 0, 2, 2)
    US_IP = (10, 0, 2, 15)

    p = start_qemu(["-netdev",
                    ("dgram,id=n0,local.type=inet,local.host=127.0.0.1,"
                     f"local.port={qport},remote.type=inet,"
                     f"remote.host=127.0.0.1,remote.port={hport}"),
                    "-device", f"e1000,netdev=n0,mac={MAC}"], log)

    def drain():
        got = []
        try:
            while True:
                d, _ = sock.recvfrom(4096)
                got.append(d)
        except socket.timeout:
            return got

    def inject_until(frame, want_pred, secs):
        deadline = time.time() + secs
        while time.time() < deadline:
            sock.sendto(frame, ("127.0.0.1", qport))
            for d in drain():
                if want_pred(d):
                    return d
        return None

    time.sleep(BOOT_SECS + 15)          # silent-wire boot runs the full bound
    type_line(p, "import inet")
    time.sleep(3)
    type_line(p, "inet.configure of [[10, 0, 2, 15], [255, 255, 255, 0], [10, 0, 2, 2]]")
    time.sleep(2)
    type_line(p, "import icmp")
    time.sleep(3)
    type_line(p, "icmp.serve of 30000")
    serve_t0 = time.time()
    time.sleep(2)

    # ARP: who-has 10.0.2.15 -> byte-exact reply (fully deterministic)
    whohas = V.eth(b"\xff" * 6, PEER_MAC, 0x0806,
                   V.arp(1, PEER_MAC, PEER_IP, b"\x00" * 6, US_IP))
    want_arp = V.eth(PEER_MAC, MACB, 0x0806,
                     V.arp(2, MACB, US_IP, PEER_MAC, PEER_IP))
    got = inject_until(whohas, lambda d: d[:len(want_arp)] == want_arp
                       and set(d[len(want_arp):]) <= {0}, 12)
    check("injected who-has answered byte-exact (script ARP reply)",
          got is not None)

    # ICMP: crafted echo request -> checksum-valid echo reply
    ping = V.eth(MACB, PEER_MAC, 0x0800,
                 V.ipv4(PEER_IP, US_IP, 1,
                        V.icmp_echo(8, 0xBEEF, 7, b"PING-THE-GUEST"),
                        ident=900))
    def is_echo_reply(d):
        if len(d) < 34 or d[:6] != PEER_MAC or d[12:14] != b"\x08\x00":
            return False
        if d[23] != 1 or ip_payload(d)[0:1] != b"\x00":
            return False
        seg = ip_payload(d)
        return (V.csum_ok(d[14:34]) and V.csum_ok(seg)
                and seg[4:8] == struct.pack(">HH", 0xBEEF, 7)
                and seg[8:] == b"PING-THE-GUEST"
                and d[26:30] == bytes(US_IP) and d[30:34] == bytes(PEER_IP))
    got = inject_until(ping, is_echo_reply, 12)
    check("injected echo request answered (checksums verified outside)",
          got is not None)

    # UDP: echo_serve mirrors ports, payload intact, checksums valid
    time.sleep(max(0, serve_t0 + 34 - time.time()))  # icmp.serve expires
    type_line(p, "import udp")
    time.sleep(3)
    type_line(p, "udp.echo_serve of [47613, 30000]")
    time.sleep(2)
    dgram = V.eth(MACB, PEER_MAC, 0x0800,
                  V.ipv4(PEER_IP, US_IP, 17,
                         V.udp(PEER_IP, US_IP, 9999, 47613, b"M13-2-INJECT"),
                         ident=901))
    def is_udp_echo(d):
        if len(d) < 42 or d[:6] != PEER_MAC or d[12:14] != b"\x08\x00":
            return False
        if d[23] != 17:
            return False
        seg = ip_payload(d)
        sp, dp = struct.unpack(">HH", seg[0:4])
        return (sp == 47613 and dp == 9999 and seg[8:] == b"M13-2-INJECT"
                and V.csum_ok(d[14:34])
                and V.udp_ok(d[26:30], d[30:34], seg)
                and d[26:30] == bytes(US_IP) and d[30:34] == bytes(PEER_IP))
    got = inject_until(dgram, is_udp_echo, 30)
    check("injected datagram echoed (ports mirrored, checksums verified)",
          got is not None)

    time.sleep(1)
    mon(p, "quit")
    p.wait(timeout=30)
    sock.close()

    s = serial_text(log)
    check("serial: honest no-offer verdict on the slirp-less backend",
          "no offer within bound" in s)


def main():
    if not os.path.exists(ISO):
        print("build/EigenOS.iso missing — run `make iso` first")
        return 1
    phase_a()
    phase_b()
    phase_c()
    phase_d()
    print()
    if fails:
        print(f"net frames: {len(fails)} FAILURE(S): {fails}")
        return 1
    print("net frames: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
