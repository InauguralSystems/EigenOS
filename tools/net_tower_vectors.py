#!/usr/bin/env python3
"""M13-2 protocol-tower vectors — the Python side of the byte-level
contract.

Importable library: an independent RFC 1071 checksum and ethernet/
IPv4/ICMP/UDP/DHCP frame crafting + validation, used by
tools/net_frames.py (external pcap/injection verification) and by
this file's __main__ to PRINT the known-good byte lists pinned inside
the tools/*_twin_hosted.eigs twins (each pinned literal cites this
generator). Nothing here shares code with the .eigs tower — that is
the point: two implementations, one wire contract.
"""
import struct

GUEST_MAC = bytes.fromhex("525400123456")
GW_MAC = bytes.fromhex("52550A000202")


def csum(data: bytes) -> int:
    """RFC 1071: one's-complement 16-bit checksum (the value to store)."""
    if len(data) % 2:
        data += b"\x00"
    s = sum(struct.unpack(f">{len(data)//2}H", data))
    while s > 0xFFFF:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def csum_ok(data: bytes) -> bool:
    """Fold-sum over data (stored checksum included) verifies to 0xFFFF."""
    if len(data) % 2:
        data += b"\x00"
    s = sum(struct.unpack(f">{len(data)//2}H", data))
    while s > 0xFFFF:
        s = (s & 0xFFFF) + (s >> 16)
    return s == 0xFFFF


def eth(dst: bytes, src: bytes, et: int, payload: bytes) -> bytes:
    return dst + src + struct.pack(">H", et) + payload


def ipv4(src, dst, proto: int, payload: bytes, ident: int = 1,
         ttl: int = 64) -> bytes:
    hdr = struct.pack(">BBHHHBBH4B4B", 0x45, 0, 20 + len(payload), ident,
                      0, ttl, proto, 0, *src, *dst)
    hdr = hdr[:10] + struct.pack(">H", csum(hdr)) + hdr[12:]
    return hdr + payload


def icmp_echo(kind: int, ident: int, seq: int, payload: bytes) -> bytes:
    seg = struct.pack(">BBHHH", kind, 0, 0, ident, seq) + payload
    return seg[:2] + struct.pack(">H", csum(seg)) + seg[4:]


def udp(src, dst, sport: int, dport: int, payload: bytes) -> bytes:
    ul = 8 + len(payload)
    seg = struct.pack(">HHHH", sport, dport, ul, 0) + payload
    ph = bytes(src) + bytes(dst) + struct.pack(">BBH", 0, 17, ul)
    c = csum(ph + seg)
    if c == 0:
        c = 0xFFFF
    return seg[:6] + struct.pack(">H", c) + seg[8:]


def udp_ok(src, dst, seg: bytes) -> bool:
    if struct.unpack(">H", seg[6:8])[0] == 0:
        return True
    ph = bytes(src) + bytes(dst) + struct.pack(">BBH", 0, 17, len(seg))
    return csum_ok(ph + seg)


def arp(op: int, smac, sip, tmac, tip) -> bytes:
    return (struct.pack(">HHBBH", 1, 0x0800, 6, 4, op)
            + smac + bytes(sip) + tmac + bytes(tip))


# ---- DHCP (RFC 2131) ----
COOKIE = bytes.fromhex("63825363")


def bootp_reply(xid: int, yiaddr, opts: bytes, siaddr=(0, 0, 0, 0),
                chaddr: bytes = GUEST_MAC) -> bytes:
    b = struct.pack(">BBBBIHH4B4B4B4B", 2, 1, 6, 0, xid, 0, 0x8000,
                    0, 0, 0, 0, *yiaddr, *siaddr, 0, 0, 0, 0)
    b += chaddr + b"\x00" * 10 + b"\x00" * 64 + b"\x00" * 128
    b += COOKIE + opts
    if len(b) < 300:
        b += b"\x00" * (300 - len(b))
    return b


def opt(code: int, val: bytes) -> bytes:
    return bytes([code, len(val)]) + val


def dhcp_offer(xid: int, yiaddr=(10, 0, 2, 15), server=(10, 0, 2, 2),
               mask=(255, 255, 255, 0), gw=(10, 0, 2, 2),
               dns=(10, 0, 2, 3), lease=86400, mtype: int = 2,
               cookie: bytes = COOKIE) -> bytes:
    o = (opt(53, bytes([mtype])) + opt(54, bytes(server))
         + opt(1, bytes(mask)) + opt(3, bytes(gw)) + opt(6, bytes(dns))
         + opt(51, struct.pack(">I", lease)) + b"\xff")
    b = bootp_reply(xid, yiaddr, o)
    return b[:236] + cookie + b[240:]


def dhcp_server_frame(xid: int, mtype: int = 2, cookie: bytes = COOKIE,
                      **kw) -> bytes:
    pay = dhcp_offer(xid, mtype=mtype, cookie=cookie, **kw)
    seg = udp((10, 0, 2, 2), (255, 255, 255, 255), 67, 68, pay)
    return eth(b"\xff" * 6, GW_MAC,
               0x0800, ipv4((10, 0, 2, 2), (255, 255, 255, 255), 17, seg))


# ---- expected GUEST frames (what the .eigs tower must emit) ----

def guest_icmp_echo_frame(ident_ip: int = 1, seq: int = 1) -> bytes:
    payload = bytes(range(0x20, 0x40))
    seg = icmp_echo(8, 0x4547, seq, payload)
    return eth(GW_MAC, GUEST_MAC, 0x0800,
               ipv4((10, 0, 2, 15), (10, 0, 2, 2), 1, seg, ident=ident_ip))


def guest_udp_probe_frame(ident_ip: int = 1, sport: int = 5000,
                          dport: int = 47000) -> bytes:
    seg = udp((10, 0, 2, 15), (10, 0, 2, 2), sport, dport,
              b"EIGENOS-M13-2-UDP")
    return eth(GW_MAC, GUEST_MAC, 0x0800,
               ipv4((10, 0, 2, 15), (10, 0, 2, 2), 17, seg, ident=ident_ip))


def guest_dhcp_discover(xid: int, ident_ip: int = 1) -> bytes:
    b = struct.pack(">BBBBIHH", 1, 1, 6, 0, xid, 0, 0x8000)
    b += b"\x00" * 16 + GUEST_MAC + b"\x00" * 10 + b"\x00" * 192
    b += COOKIE + opt(53, b"\x01") + opt(55, bytes([1, 3, 6, 51, 54])) + b"\xff"
    b += b"\x00" * (300 - len(b))
    seg = udp((0, 0, 0, 0), (255, 255, 255, 255), 68, 67, b)
    return eth(b"\xff" * 6, GUEST_MAC, 0x0800,
               ipv4((0, 0, 0, 0), (255, 255, 255, 255), 17, seg,
                    ident=ident_ip))


def guest_dhcp_request(xid: int, ident_ip: int = 2,
                       reqip=(10, 0, 2, 15), server=(10, 0, 2, 2)) -> bytes:
    b = struct.pack(">BBBBIHH", 1, 1, 6, 0, xid, 0, 0x8000)
    b += b"\x00" * 16 + GUEST_MAC + b"\x00" * 10 + b"\x00" * 192
    b += (COOKIE + opt(53, b"\x03") + opt(50, bytes(reqip))
          + opt(54, bytes(server)) + opt(55, bytes([1, 3, 6, 51, 54]))
          + b"\xff")
    b += b"\x00" * (300 - len(b))
    seg = udp((0, 0, 0, 0), (255, 255, 255, 255), 68, 67, b)
    return eth(b"\xff" * 6, GUEST_MAC, 0x0800,
               ipv4((0, 0, 0, 0), (255, 255, 255, 255), 17, seg,
                    ident=ident_ip))


def eigs_list(b: bytes, per_line: int = 16, indent: str = "    ") -> str:
    """Render bytes as .eigs extend-of chunks for pasting into a twin."""
    lines = []
    for i in range(0, len(b), per_line):
        chunk = ", ".join(str(x) for x in b[i:i + per_line])
        lines.append(f"{indent}extend of [want, [{chunk}]]")
    return "\n".join(lines)


if __name__ == "__main__":
    print("# RFC 1071 example (00 01 f2 03 f4 f5 f6 f7):")
    print(f"#   checksum = 0x{csum(bytes.fromhex('0001f203f4f5f6f7')):04X}")
    wiki = bytes.fromhex("450000730000400040110000c0a80001c0a800c7")
    print("# known-good IPv4 header (checksum field zeroed):")
    print(f"#   checksum = 0x{csum(wiki):04X}")
    print()
    print("# guest ICMP echo request frame (ident 1, seq 1):")
    print(eigs_list(guest_icmp_echo_frame()))
    print()
    print("# guest UDP probe frame (ident 1, 5000 -> 47000):")
    print(eigs_list(guest_udp_probe_frame()))
    print()
    xid = 50 * 65536 + 0x4553 + 1
    print(f"# guest DHCP DISCOVER (xid 0x{xid:08X} = twin tick schedule):")
    print(eigs_list(guest_dhcp_discover(xid)))
    print()
    print(f"# guest DHCP REQUEST (same xid, ident 2):")
    print(eigs_list(guest_dhcp_request(xid)))
