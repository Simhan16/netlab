"""Synthetic pcaps for tests and smoke evals.

Each scenario hand-builds the textbook packet pattern of one failure. They
test the plumbing (C++ parser -> summary -> diagnosers -> API) without root
or a lab, but they are idealized: real captures are messier. Accuracy claims
must come from the real casebook (phase1/casebook), never from these.
"""

from __future__ import annotations

import struct
from collections.abc import Callable
from pathlib import Path

FIN, SYN, RST, PSH, ACK = 0x01, 0x02, 0x04, 0x08, 0x10

CLIENT, SERVER, RESOLVER = "192.168.10.10", "93.184.216.34", "1.1.1.1"


def _ip(addr: str) -> bytes:
    return bytes(int(x) for x in addr.split("."))


def _checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def ipv4(src: str, dst: str, proto: int, payload: bytes, ttl: int = 64) -> bytes:
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0, 0x4000, ttl, proto, 0, _ip(src), _ip(dst))
    header = header[:10] + struct.pack("!H", _checksum(header)) + header[12:]
    return header + payload


def ethernet(packet: bytes) -> bytes:
    return bytes.fromhex("020000000002" "020000000001" "0800") + packet


def tcp(sport: int, dport: int, seq: int, ack: int, flags: int, window: int = 502, payload: bytes = b"",
        mss: int | None = None) -> bytes:
    options = struct.pack("!BBH", 2, 4, mss) if mss else b""
    offset = (20 + len(options)) // 4
    return struct.pack("!HHIIBBHHH", sport, dport, seq & 0xFFFFFFFF, ack & 0xFFFFFFFF, offset << 4, flags,
                       window, 0, 0) + options + payload


def udp(sport: int, dport: int, payload: bytes) -> bytes:
    return struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload


def dns_message(qid: int, name: str, *, response: bool = False, rcode: int = 0, answer: str | None = None,
                qtype: int = 1) -> bytes:
    flags = 0x0100 | (0x8180 if response else 0) | rcode
    qname = b"".join(bytes([len(p)]) + p.encode() for p in name.rstrip(".").split(".")) + b"\0"
    body = qname + struct.pack("!HH", qtype, 1)
    ancount = 1 if answer else 0
    if answer:
        body += struct.pack("!HHHIH", 0xC00C, qtype, 1, 300, 4) + _ip(answer)
    return struct.pack("!HHHHHH", qid, flags, 1, ancount, 0, 0) + body


def tls_client_hello(sni: str) -> bytes:
    name = sni.encode()
    sni_list = b"\0" + struct.pack("!H", len(name)) + name
    ext = struct.pack("!HH", 0, len(sni_list) + 2) + struct.pack("!H", len(sni_list)) + sni_list
    hello = b"\x03\x03" + b"\x11" * 32 + b"\0" + struct.pack("!HH", 2, 0xC02F) + b"\x01\0"
    hello += struct.pack("!H", len(ext)) + ext
    handshake = b"\x01" + len(hello).to_bytes(3, "big") + hello
    return b"\x16\x03\x01" + struct.pack("!H", len(handshake)) + handshake


def tls_server_hello() -> bytes:
    hello = b"\x03\x03" + b"\x22" * 32 + b"\0" + struct.pack("!H", 0xC02F) + b"\0"
    handshake = b"\x02" + len(hello).to_bytes(3, "big") + hello
    cert = b"\x0b" + (900).to_bytes(3, "big") + b"\x30" * 900  # stand-in certificate bytes
    return (b"\x16\x03\x03" + struct.pack("!H", len(handshake)) + handshake
            + b"\x16\x03\x03" + struct.pack("!H", len(cert)) + cert)


def tls_alert(description: int, level: int = 2) -> bytes:
    return b"\x15\x03\x03\x00\x02" + bytes([level, description])


class Capture:
    """A timeline of Ethernet frames written as a classic little-endian pcap."""

    def __init__(self, start: float = 1_700_000_000.0) -> None:
        self.t = start
        self.frames: list[tuple[float, bytes]] = []

    def at(self, dt: float) -> Capture:
        self.t += dt
        return self

    def send(self, src: str, dst: str, proto: int, l4: bytes) -> None:
        self.frames.append((self.t, ethernet(ipv4(src, dst, proto, l4))))

    def save(self, path: Path) -> Path:
        with path.open("wb") as f:
            f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 262144, 1))
            for ts, frame in self.frames:
                sec = int(ts)
                usec = min(999_999, round((ts - sec) * 1e6))
                f.write(struct.pack("<IIII", sec, usec, len(frame), len(frame)) + frame)
        return path


class TcpConn:
    """One TCP connection seen from a capture point next to the client."""

    def __init__(self, cap: Capture, *, client: str = CLIENT, server: str = SERVER, cport: int = 51000,
                 sport: int = 443, rtt: float = 0.020, mss: int = 1448) -> None:
        self.cap, self.client, self.server = cap, client, server
        self.cport, self.sport, self.rtt, self.mss = cport, sport, rtt, mss
        self.cseq, self.sseq = 1000, 50000  # next sequence number each side will send

    def c2s(self, flags: int, payload: bytes = b"", *, seq: int | None = None, ack: int | None = None,
            dt: float = 0.0) -> None:
        self.cap.at(dt).send(self.client, self.server, 6, tcp(
            self.cport, self.sport, self.cseq if seq is None else seq,
            (self.sseq if ack is None else ack) if flags & ACK else 0, flags, 64240 if flags & SYN else 502,
            payload, self.mss + 12 if flags & SYN else None))

    def s2c(self, flags: int, payload: bytes = b"", *, seq: int | None = None, ack: int | None = None,
            dt: float = 0.0) -> None:
        self.cap.at(dt).send(self.server, self.client, 6, tcp(
            self.sport, self.cport, self.sseq if seq is None else seq,
            (self.cseq if ack is None else ack) if flags & ACK else 0, flags, 65160 if flags & SYN else 509,
            payload, self.mss + 12 if flags & SYN else None))

    def handshake(self) -> None:
        self.c2s(SYN)
        self.cseq += 1
        self.s2c(SYN | ACK, dt=self.rtt)
        self.sseq += 1
        self.c2s(ACK, dt=0.0002)

    def client_sends(self, payload: bytes) -> None:
        self.c2s(PSH | ACK, payload, dt=0.001)
        self.cseq += len(payload)

    def server_sends(self, payload: bytes) -> None:
        self.s2c(PSH | ACK, payload, dt=self.rtt)
        self.sseq += len(payload)

    def download(self, total: int, *, lose_every: int = 0) -> None:
        """Server streams `total` bytes; every `lose_every`-th segment is lost before reaching the client,
        which answers with dup ACKs until the server fast-retransmits it."""
        first = True
        hole: tuple[int, int] | None = None
        dup_acks = 0
        index = 0
        while total > 0:
            n = min(self.mss, total)
            seq = self.sseq
            lost = hole is None and lose_every > 0 and index % lose_every == lose_every - 1
            if lost:
                hole = (seq, n)
            else:
                self.s2c(ACK, b"\0" * n, seq=seq, dt=self.rtt if first else 0.0005)
                first = False
            self.sseq += n
            total -= n
            index += 1
            if hole and not lost:
                self.c2s(ACK, ack=hole[0], dt=0.0001)  # duplicate ACK: still waiting for the hole
                dup_acks += 1
                if dup_acks >= 3:
                    self.s2c(ACK, b"\0" * hole[1], seq=hole[0], dt=self.rtt)  # fast retransmit
                    self.c2s(ACK, dt=0.0001)
                    hole, dup_acks = None, 0
            elif index % 2 == 0:
                self.c2s(ACK, dt=0.0001)
        if hole:
            self.s2c(ACK, b"\0" * hole[1], seq=hole[0], dt=0.2)
        self.c2s(ACK, dt=0.0001)

    def close(self) -> None:
        self.c2s(FIN | ACK, dt=0.001)
        self.cseq += 1
        self.s2c(FIN | ACK, dt=self.rtt)
        self.sseq += 1
        self.c2s(ACK, dt=0.0002)


def _dns_ok(cap: Capture, name: str = "example.com", qid: int = 0x1A2B) -> None:
    cap.at(0.001).send(CLIENT, RESOLVER, 17, udp(40000, 53, dns_message(qid, name)))
    cap.at(0.012).send(RESOLVER, CLIENT, 17, udp(53, 40000, dns_message(qid, name, response=True, answer=SERVER)))


# --- scenarios -------------------------------------------------------------------------------

def healthy(cap: Capture) -> None:
    _dns_ok(cap)
    c = TcpConn(cap, sport=80)
    c.handshake()
    c.client_sends(b"GET /big HTTP/1.1\r\nHost: example.com\r\n\r\n")
    c.download(150_000)
    c.close()


def dns_unreachable(cap: Capture) -> None:
    for i in range(4):
        qid = 0x2000 + i
        query = udp(41000 + i, 53, dns_message(qid, "example.com"))
        cap.at(0.0 if i == 0 else 2.0**i).send(CLIENT, "192.0.2.53", 17, query)


def dns_nxdomain(cap: Capture) -> None:
    name = "api.netlab-typo.invalid"
    for i, qtype in enumerate((1, 28)):
        qid = 0x3000 + i
        cap.at(0.001).send(CLIENT, RESOLVER, 17, udp(42000 + i, 53, dns_message(qid, name, qtype=qtype)))
        cap.at(0.010).send(RESOLVER, CLIENT, 17,
                           udp(53, 42000 + i, dns_message(qid, name, response=True, rcode=3, qtype=qtype)))


def firewall_drop(cap: Capture) -> None:
    c = TcpConn(cap, sport=8080)
    for i, gap in enumerate((0.0, 1.0, 2.0, 4.0)):
        c.c2s(SYN, dt=gap if i else 0.0)


def connection_refused(cap: Capture) -> None:
    for attempt in range(2):
        c = TcpConn(cap, sport=9090, cport=52000 + attempt)
        c.c2s(SYN, dt=0.5 if attempt else 0.0)
        c.s2c(RST | ACK, ack=c.cseq + 1, seq=0, dt=0.0004)


def asymmetric_routing(cap: Capture) -> None:
    # Seen from the server: SYNs arrive, SYN-ACKs go out, the handshake never completes.
    c = TcpConn(cap, client=CLIENT, server="192.168.20.10", sport=8080)
    for i, gap in enumerate((0.0, 1.0, 2.0)):
        c.c2s(SYN, dt=gap if i else 0.0)
        c.s2c(SYN | ACK, ack=c.cseq + 1, dt=0.0002)


def mtu_blackhole(cap: Capture) -> None:
    c = TcpConn(cap, sport=8000)
    c.handshake()
    c.client_sends(b"GET /big.bin HTTP/1.1\r\nHost: lab\r\n\r\n")
    c.s2c(ACK, dt=c.rtt)  # small ACK for the request: small packets work
    seq = c.sseq
    for i in range(6):  # the first full-size segment, resent with exponential backoff, never acknowledged
        c.s2c(ACK, b"\0" * c.mss, seq=seq, dt=0.2 * 2**i if i else 0.001)
    c.sseq += c.mss


def packet_loss(cap: Capture) -> None:
    _dns_ok(cap)
    c = TcpConn(cap, sport=80)
    c.handshake()
    c.client_sends(b"GET /big HTTP/1.1\r\nHost: example.com\r\n\r\n")
    c.download(300_000, lose_every=12)
    c.close()


def high_latency(cap: Capture) -> None:
    _dns_ok(cap)
    for i in range(3):
        c = TcpConn(cap, sport=80, cport=53000 + i, rtt=0.6)
        c.handshake()
        c.client_sends(b"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
        c.server_sends(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello")
        c.c2s(ACK, dt=0.0002)
        c.close()


def tls_handshake_failure(cap: Capture) -> None:
    c = TcpConn(cap, server="192.168.20.10", sport=8443)
    c.handshake()
    c.client_sends(tls_client_hello("app.lab"))
    c.server_sends(tls_server_hello())
    c.c2s(ACK, dt=0.0002)
    c.client_sends(tls_alert(42))  # fatal bad_certificate
    c.close()


SCENARIOS: dict[str, Callable[[Capture], None]] = {
    "healthy": healthy,
    "dns_unreachable": dns_unreachable,
    "dns_nxdomain": dns_nxdomain,
    "firewall_drop": firewall_drop,
    "connection_refused": connection_refused,
    "asymmetric_routing": asymmetric_routing,
    "mtu_blackhole": mtu_blackhole,
    "packet_loss": packet_loss,
    "high_latency": high_latency,
    "tls_handshake_failure": tls_handshake_failure,
}


def write_scenario(label: str, path: Path) -> Path:
    cap = Capture()
    SCENARIOS[label](cap)
    return cap.save(path)
