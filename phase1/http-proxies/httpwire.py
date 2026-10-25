"""Finished HTTP plumbing for steps 2 and 3, so the proxy steps can focus on proxying.

Step 1 has you write the response side of this yourself. Read this file once you've
done it: it's the same ideas (read until the blank line, then frame the body), applied to requests.
"""

from __future__ import annotations

import contextlib
import socket
import threading
from dataclasses import dataclass, field

MAX_HEAD = 64 * 1024

# Hop-by-hop headers describe one connection (client↔proxy or proxy↔server), not the
# message, so a proxy must never forward them (RFC 9110 section 7.6.1).
HOP_BY_HOP = {
    "connection",
    "keep-alive",
    "proxy-connection",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
}


@dataclass
class Head:
    """A request or response start line plus its headers, in arrival order."""

    start_line: str
    headers: list[tuple[str, str]] = field(default_factory=list)

    def get(self, name: str, default: str | None = None) -> str | None:
        name = name.lower()
        for key, value in self.headers:
            if key.lower() == name:
                return value
        return default

    def encode(self) -> bytes:
        lines = [self.start_line] + [f"{k}: {v}" for k, v in self.headers]
        return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1")


def read_head(sock: socket.socket, buf: bytearray) -> Head | None:
    """Reads up to the blank line ending the head. Bytes after it stay in `buf` (the start of the body).
    Returns None if the peer closed before sending anything; raises ValueError on garbage."""
    while b"\r\n\r\n" not in buf:
        if len(buf) > MAX_HEAD:
            raise ValueError("request head too large")
        chunk = sock.recv(65536)
        if not chunk:
            if buf:
                raise ValueError("connection closed in the middle of the head")
            return None
        buf += chunk
    end = buf.index(b"\r\n\r\n")
    lines = bytes(buf[:end]).decode("latin-1").split("\r\n")
    del buf[: end + 4]
    head = Head(lines[0])
    for line in lines[1:]:
        key, sep, value = line.partition(":")
        if not sep or not key or key != key.strip():
            raise ValueError(f"malformed header line: {line!r}")
        head.headers.append((key, value.strip()))
    return head


def read_body(sock: socket.socket, buf: bytearray, head: Head) -> bytes:
    """The body of a *request*: Content-Length only (chunked uploads aren't supported here)."""
    if head.get("Transfer-Encoding"):
        raise ValueError("chunked request bodies are not supported")
    length = int(head.get("Content-Length", "0") or "0")
    while len(buf) < length:
        chunk = sock.recv(65536)
        if not chunk:
            raise ValueError("connection closed in the middle of the body")
        buf += chunk
    body = bytes(buf[:length])
    del buf[:length]
    return body


def strip_hop_by_hop(headers: list[tuple[str, str]]) -> list[tuple[str, str]]:
    """Removes the standard hop-by-hop headers plus any the sender listed in its Connection header."""
    listed = set()
    for key, value in headers:
        if key.lower() == "connection":
            listed |= {token.strip().lower() for token in value.split(",")}
    return [(k, v) for k, v in headers if k.lower() not in HOP_BY_HOP | listed]


def send_error(sock: socket.socket, status: int, reason: str, detail: str) -> None:
    """A complete little response for errors the proxy generates itself (400, 403, 502, 504...)."""
    body = f"{status} {reason}: {detail}\n".encode()
    head = Head(
        f"HTTP/1.1 {status} {reason}",
        [
            ("Content-Type", "text/plain; charset=utf-8"),
            ("Content-Length", str(len(body))),
            ("Connection", "close"),
            ("X-Proxy-Error", "netlab"),
        ],
    )
    with contextlib.suppress(OSError):
        sock.sendall(head.encode() + body)


def relay(src: socket.socket, dst: socket.socket) -> int:
    """Copies src → dst until src closes, then half-closes dst. Returns the bytes copied."""
    total = 0
    try:
        while chunk := src.recv(65536):
            dst.sendall(chunk)
            total += len(chunk)
    except OSError:
        pass
    with contextlib.suppress(OSError):
        dst.shutdown(socket.SHUT_WR)  # pass the "I'm done" (FIN) along
    return total


def pipe(a: socket.socket, b: socket.socket) -> None:
    """Relays both directions at once until both sides are done: a CONNECT tunnel."""
    other = threading.Thread(target=relay, args=(b, a), daemon=True)
    other.start()
    relay(a, b)
    other.join()
