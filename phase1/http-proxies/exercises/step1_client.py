#!/usr/bin/env python3
"""Step 1: an HTTP/1.1 client that sends several requests over ONE connection.

    python3 step1_client.py HOST PORT PATH [PATH ...]
    python3 step1_client.py 127.0.0.1 8001 /hello /chunked /status/204 /cache     (with origin.py running)

Check it:  python3 check.py 1

The hard part of keep-alive is knowing exactly where each response ends, because the next
response starts right after it on the same stream. Rules (RFC 9112 section 6.3), in order:
  1. 1xx, 204, 304 (and any response to HEAD): no body, whatever the headers say
  2. Transfer-Encoding: chunked  → repeated "<size in hex>[;ext]\\r\\n<data>\\r\\n", then
                                   "0\\r\\n", optional trailer lines, and a blank "\\r\\n"
  3. Content-Length: N           → exactly N bytes
  4. neither                     → the body runs until the server closes
"""

from __future__ import annotations

import socket
import sys
from dataclasses import dataclass, field


@dataclass
class Response:
    status: int
    reason: str
    headers: dict[str, str] = field(default_factory=dict)  # names lowercased: "content-length"
    body: bytes = b""


class Reader:
    """Buffered reads from a socket. recv() returns arbitrary pieces; you need lines and exact sizes."""

    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.buf = bytearray()  # received but not yet consumed

    def until(self, marker: bytes) -> bytes:
        """Returns everything up to and including `marker`, receiving more as needed.
        Bytes after the marker stay in self.buf for the next call."""
        # TODO 1: loop: if marker is in self.buf, cut it out (with the marker) and return it.
        #         Otherwise recv() more; b"" means the server closed: raise ConnectionError.
        raise NotImplementedError("Reader.until")

    def exact(self, n: int) -> bytes:
        """Returns exactly n bytes."""
        # TODO 2: same idea: recv until self.buf holds n bytes, then cut n off the front.
        raise NotImplementedError("Reader.exact")

    def rest(self) -> bytes:
        """Returns everything until the server closes the connection (rule 4)."""
        # TODO 3: recv until b"", return all of it.
        raise NotImplementedError("Reader.rest")


def read_response(reader: Reader) -> Response:
    # TODO 4: the head: reader.until(b"\r\n\r\n"), decode as latin-1, split into lines.
    #         Line 0 is "HTTP/1.1 200 OK": the status code and the reason phrase.
    #         Each other line is "Name: value": store it lowercased in a dict.
    # TODO 5: the body, by the four rules in the docstring at the top. For chunked: read a size
    #         line with until(b"\r\n"), ignore anything after ";", parse the hex with int(x, 16),
    #         read exact(size) and then the 2-byte \r\n after the data. On size 0, read lines
    #         until you get a bare b"\r\n" (that skips any trailers).
    raise NotImplementedError("read_response")


def fetch_all(host: str, port: int, paths: list[str]) -> list[Response]:
    """GETs each path in turn over a single keep-alive connection and returns the responses."""
    # TODO 6: one socket.create_connection((host, port), timeout=10) and one Reader for the
    #         whole loop. For each path send "GET <path> HTTP/1.1\r\nHost: <host>:<port>\r\n\r\n"
    #         (HTTP/1.1 *requires* Host), then read_response(). Stop early if the server replied
    #         with "Connection: close".
    raise NotImplementedError("fetch_all")


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
