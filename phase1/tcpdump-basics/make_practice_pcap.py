#!/usr/bin/env python3
"""Write practice.pcap: a small, hand-built capture for learning tcpdump and Wireshark.

It is what a capture on a laptop (192.168.10.10) might show while it browses one page,
mistypes a hostname, and hits a closed port. Every packet is built field by field here,
with correct checksums, so you can compare the bytes with the code. Addresses come from
documentation ranges (203.0.113.0/24, example.com) and the MACs are locally administered,
so nothing in it points at a real machine.

Stdlib only:  python3 make_practice_pcap.py   (writes practice.pcap next to this file)
"""

from __future__ import annotations

import struct
from pathlib import Path

# Layer 2: MAC addresses. The laptop only ever talks to MACs on its own LAN.
LAPTOP_MAC = bytes.fromhex("02000a00000a")
GATEWAY_MAC = bytes.fromhex("02000a000001")
BROADCAST = b"\xff" * 6

# Layer 3: IP addresses. These are end to end.
LAPTOP, GATEWAY, RESOLVER, WEB = "192.168.10.10", "192.168.10.1", "1.1.1.1", "203.0.113.10"

FIN, SYN, RST, PSH, ACK = 0x01, 0x02, 0x04, 0x08, 0x10


def ip_bytes(addr: str) -> bytes:
    return bytes(int(x) for x in addr.split("."))


def checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def ethernet(dst: bytes, src: bytes, ethertype: int, payload: bytes) -> bytes:
    return dst + src + struct.pack("!H", ethertype) + payload


def arp(op: int, sender_mac: bytes, sender_ip: str, target_mac: bytes, target_ip: str) -> bytes:
    return struct.pack("!HHBBH", 1, 0x0800, 6, 4, op) + sender_mac + ip_bytes(sender_ip) + target_mac + ip_bytes(
        target_ip)


class IPv4:
    def __init__(self) -> None:
        self.ident = 0x1c46

    def packet(self, src: str, dst: str, proto: int, l4: bytes, ttl: int) -> bytes:
        self.ident += 1
        header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(l4), self.ident, 0x4000, ttl, proto, 0,
                             ip_bytes(src), ip_bytes(dst))
        return header[:10] + struct.pack("!H", checksum(header)) + header[12:] + l4


def pseudo_header(src: str, dst: str, proto: int, length: int) -> bytes:
    return ip_bytes(src) + ip_bytes(dst) + struct.pack("!BBH", 0, proto, length)


def udp(src: str, dst: str, sport: int, dport: int, payload: bytes) -> bytes:
    segment = struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload
    csum = checksum(pseudo_header(src, dst, 17, len(segment)) + segment) or 0xFFFF
    return segment[:6] + struct.pack("!H", csum) + segment[8:]


def tcp(src: str, dst: str, sport: int, dport: int, seq: int, ack: int, flags: int, window: int,
        payload: bytes = b"", mss: int | None = None) -> bytes:
    options = struct.pack("!BBH", 2, 4, mss) if mss else b""
    header = struct.pack("!HHIIBBHHH", sport, dport, seq, ack, (20 + len(options)) // 4 << 4, flags, window, 0, 0)
    segment = header + options + payload
    csum = checksum(pseudo_header(src, dst, 6, len(segment)) + segment)
    return segment[:16] + struct.pack("!H", csum) + segment[18:]


def dns_name(name: str) -> bytes:
    return b"".join(bytes([len(label)]) + label.encode() for label in name.split(".")) + b"\0"


def dns_query(qid: int, name: str) -> bytes:
    return struct.pack("!HHHHHH", qid, 0x0100, 1, 0, 0, 0) + dns_name(name) + struct.pack("!HH", 1, 1)


def dns_answer(qid: int, name: str, address: str) -> bytes:
    question = dns_name(name) + struct.pack("!HH", 1, 1)
    answer = struct.pack("!HHHIH", 0xC00C, 1, 1, 300, 4) + ip_bytes(address)  # 0xC00C: pointer to the qname
    return struct.pack("!HHHHHH", qid, 0x8180, 1, 1, 0, 0) + question + answer


def dns_nxdomain(qid: int, name: str, zone_offset: int) -> bytes:
    """NXDOMAIN plus the zone's SOA, whose last field is how long to cache "no such name"."""
    question = dns_name(name) + struct.pack("!HH", 1, 1)
    zone = struct.pack("!H", 0xC000 | zone_offset)  # pointer to "example.com" inside the qname
    rdata = (b"\x02ns" + zone + b"\x0ahostmaster" + zone
             + struct.pack("!IIIII", 2026102401, 7200, 3600, 1209600, 3600))
    soa = zone + struct.pack("!HHIH", 6, 1, 3600, len(rdata)) + rdata
    return struct.pack("!HHHHHH", qid, 0x8183, 1, 0, 1, 0) + question + soa


HTTP_REQUEST = (b"GET / HTTP/1.1\r\nHost: www.example.com\r\nUser-Agent: curl/8.5.0\r\nAccept: */*\r\n\r\n")
HTML = b"<!doctype html>\n<html><head><title>netlab</title></head>\n<body><p>Hello from the practice capture.</p></body></html>\n"
HTTP_RESPONSE = (b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + str(len(HTML)).encode()
                 + b"\r\nConnection: close\r\n\r\n" + HTML)


class Capture:
    def __init__(self, start: float = 1_761_300_000.0) -> None:
        self.t = start
        self.frames: list[tuple[float, bytes]] = []
        self.ip = IPv4()

    def wait(self, seconds: float) -> Capture:
        self.t += seconds
        return self

    def frame(self, frame: bytes) -> None:
        self.frames.append((self.t, frame))

    def outbound(self, dst: str, proto: int, l4: bytes) -> None:
        """Laptop → anything off the LAN: the IP destination is far away, the MAC destination is the gateway."""
        self.frame(ethernet(GATEWAY_MAC, LAPTOP_MAC, 0x0800, self.ip.packet(LAPTOP, dst, proto, l4, ttl=64)))

    def inbound(self, src: str, proto: int, l4: bytes, ttl: int) -> None:
        self.frame(ethernet(LAPTOP_MAC, GATEWAY_MAC, 0x0800, self.ip.packet(src, LAPTOP, proto, l4, ttl=ttl)))

    def save(self, path: Path) -> None:
        with path.open("wb") as f:
            f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 262144, 1))  # classic pcap, Ethernet
            for ts, frame in self.frames:
                sec, usec = int(ts), round((ts - int(ts)) * 1e6)
                f.write(struct.pack("<IIII", sec, usec, len(frame), len(frame)) + frame)


def build() -> Capture:
    cap = Capture()
    rtt_dns, rtt_web = 0.011, 0.024

    # 1. ARP: the laptop knows the gateway's IP (from DHCP) but needs its MAC.
    cap.frame(ethernet(BROADCAST, LAPTOP_MAC, 0x0806, arp(1, LAPTOP_MAC, LAPTOP, b"\0" * 6, GATEWAY)))
    cap.wait(0.0008).frame(ethernet(LAPTOP_MAC, GATEWAY_MAC, 0x0806, arp(2, GATEWAY_MAC, GATEWAY, LAPTOP_MAC, LAPTOP)))

    # 2. DNS over UDP: what is www.example.com's address?
    cap.wait(0.0004).outbound(RESOLVER, 17, udp(LAPTOP, RESOLVER, 53124, 53, dns_query(0x4D2A, "www.example.com")))
    cap.wait(rtt_dns).inbound(RESOLVER, 17, udp(RESOLVER, LAPTOP, 53, 53124,
                                                dns_answer(0x4D2A, "www.example.com", WEB)), ttl=57)

    # 3. TCP to port 80: handshake, one HTTP request and response, then teardown.
    c_isn, s_isn, cport = 3_090_211_548, 1_207_533_001, 51244

    def c2s(seq: int, ack: int, flags: int, payload: bytes = b"", mss: int | None = None) -> None:
        cap.outbound(WEB, 6, tcp(LAPTOP, WEB, cport, 80, seq, ack, flags, 64240 if mss else 502, payload, mss))

    def s2c(seq: int, ack: int, flags: int, payload: bytes = b"", mss: int | None = None) -> None:
        cap.inbound(WEB, 6, tcp(WEB, LAPTOP, 80, cport, seq, ack, flags, 65160 if mss else 509, payload, mss),
                    ttl=52)

    cap.wait(0.0006)
    c2s(c_isn, 0, SYN, mss=1460)
    cap.wait(rtt_web)
    s2c(s_isn, c_isn + 1, SYN | ACK, mss=1400)
    cap.wait(0.0001)
    c2s(c_isn + 1, s_isn + 1, ACK)
    cap.wait(0.0002)
    c2s(c_isn + 1, s_isn + 1, PSH | ACK, HTTP_REQUEST)
    c_next = c_isn + 1 + len(HTTP_REQUEST)
    cap.wait(rtt_web)
    s2c(s_isn + 1, c_next, ACK)
    cap.wait(0.0031)
    s2c(s_isn + 1, c_next, PSH | ACK, HTTP_RESPONSE)
    s_next = s_isn + 1 + len(HTTP_RESPONSE)
    cap.wait(0.0001)
    c2s(c_next, s_next, ACK)
    cap.wait(0.0003)
    s2c(s_next, c_next, FIN | ACK)  # the server closes first (Connection: close)
    cap.wait(0.0002)
    c2s(c_next, s_next + 1, FIN | ACK)
    cap.wait(rtt_web)
    s2c(s_next + 1, c_next + 1, ACK)

    # 4. A typo: wwww.example.com does not exist.
    cap.wait(1.8).outbound(RESOLVER, 17, udp(LAPTOP, RESOLVER, 61877, 53, dns_query(0x91F0, "wwww.example.com")))
    cap.wait(rtt_dns).inbound(RESOLVER, 17, udp(RESOLVER, LAPTOP, 53, 61877,
                                                dns_nxdomain(0x91F0, "wwww.example.com", 12 + 5)), ttl=57)

    # 5. A closed port: the host is up, but nothing listens on 8443, so its kernel answers with RST.
    cap.wait(2.4)
    cap.outbound(WEB, 6, tcp(LAPTOP, WEB, 51250, 8443, 2_811_004_370, 0, SYN, 64240, mss=1460))
    cap.wait(rtt_web).inbound(WEB, 6, tcp(WEB, LAPTOP, 8443, 51250, 0, 2_811_004_371, RST | ACK, 0), ttl=52)
    return cap


if __name__ == "__main__":
    out = Path(__file__).resolve().parent / "practice.pcap"
    build().save(out)
    print(f"wrote {out}")
