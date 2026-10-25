#!/usr/bin/env python3
"""Step 1 solution: an HTTP/1.1 client that sends several requests over ONE connection.

    python3 step1_client.py HOST PORT PATH [PATH ...]

Keep-alive only works if the client knows exactly where each response ends, so this
is really an exercise in message framing:
  - 204, 304 (and any response to HEAD) have no body
  - Transfer-Encoding: chunked  → a series of <hex size>\\r\\n<data>\\r\\n, ending with a 0 chunk
  - Content-Length: N           → exactly N bytes
  - neither                     → the body runs until the server closes (so the connection can't be reused)
"""

from __future__ import annotations

import socket
import sys
from dataclasses import dataclass, field


@dataclass
class Response:
    status: int
    reason: str
    headers: dict[str, str] = field(default_factory=dict)  # lowercased names
    body: bytes = b""


class Reader:
    """Buffered reads from a socket: TCP hands us arbitrary pieces, and we need lines and exact sizes."""

    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.buf = bytearray()

    def _fill(self) -> None:
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ConnectionError("server closed the connection mid-response")
        self.buf += chunk

    def until(self, marker: bytes) -> bytes:
        """Everything up to and including `marker`."""
        while (i := self.buf.find(marker)) < 0:
            self._fill()
        out = bytes(self.buf[: i + len(marker)])
        del self.buf[: i + len(marker)]
        return out

    def exact(self, n: int) -> bytes:
        while len(self.buf) < n:
            self._fill()
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def rest(self) -> bytes:
        """Everything until the server closes."""
        while chunk := self.sock.recv(65536):
            self.buf += chunk
        out = bytes(self.buf)
        self.buf.clear()
        return out


def read_response(reader: Reader, method: str = "GET") -> Response:
    head = reader.until(b"\r\n\r\n").decode("latin-1")
    lines = head.split("\r\n")[:-2]
    version, code, *reason = lines[0].split(" ", 2)
    response = Response(int(code), reason[0] if reason else "")
    for line in lines[1:]:
        key, _, value = line.partition(":")
        name = key.strip().lower()
        # Repeated headers combine with commas (Set-Cookie is the famous exception, ignored here).
        response.headers[name] = (
            f"{response.headers[name]}, {value.strip()}" if name in response.headers else value.strip()
        )

    if method == "HEAD" or response.status in (204, 304) or 100 <= response.status < 200:
        return response
    if "chunked" in response.headers.get("transfer-encoding", "").lower():
        parts = []
        while True:
            size_line = reader.until(b"\r\n")
            size = int(size_line.split(b";")[0].strip(), 16)  # hex, optionally followed by ;extensions
            if size == 0:
                while reader.until(b"\r\n") != b"\r\n":  # optional trailer lines, then a blank line
                    pass
                break
            parts.append(reader.exact(size))
            reader.exact(2)  # the \r\n after each chunk's data
        response.body = b"".join(parts)
    elif "content-length" in response.headers:
        response.body = reader.exact(int(response.headers["content-length"]))
    else:
        response.body = reader.rest()
    return response


def fetch_all(host: str, port: int, paths: list[str]) -> list[Response]:
    """GETs each path in turn over a single keep-alive connection."""
    responses = []
    with socket.create_connection((host, port), timeout=10) as sock:
        reader = Reader(sock)
        for path in paths:
            request = f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUser-Agent: netlab-step1\r\nAccept: */*\r\n\r\n"
            sock.sendall(request.encode())
            response = read_response(reader)
            responses.append(response)
            if response.headers.get("connection", "").lower() == "close":
                break  # the server is done with this connection
    return responses


def main() -> int:
    if len(sys.argv) < 4:
        print(f"usage: {sys.argv[0]} HOST PORT PATH [PATH ...]", file=sys.stderr)
        return 1
    for path, r in zip(sys.argv[3:], fetch_all(sys.argv[1], int(sys.argv[2]), sys.argv[3:]), strict=False):
        print(f"--- GET {path}: {r.status} {r.reason}, {len(r.body)} body bytes")
        for key, value in r.headers.items():
            print(f"    {key}: {value}")
        sys.stdout.write(r.body.decode(errors="replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
