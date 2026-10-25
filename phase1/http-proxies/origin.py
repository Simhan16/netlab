#!/usr/bin/env python3
"""A small HTTP/1.1 origin server to experiment against. Stdlib only.

    python3 origin.py                      # http://127.0.0.1:8001
    python3 origin.py --port 8002 --name b # a second backend, for the reverse proxy

Every response carries `X-Served-By: <name>`, so you can tell backends apart.

    GET  /hello           a short text page
    GET  /headers         JSON: the request line and headers exactly as they arrived
    GET  /chunked         a body sent with Transfer-Encoding: chunked, in slow pieces
    GET  /slow?s=N        waits N seconds, then answers
    GET  /status/N        replies with status N
    GET  /cache           cacheable: ETag + Cache-Control; send If-None-Match to get a 304
    GET  /redirect        302 to /hello
    GET  /cookie          sets a cookie; /whoami shows the cookie you send back
    GET  /big?n=BYTES     a body of n bytes
    POST /echo            sends your request body back
"""

from __future__ import annotations

import argparse
import contextlib
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


class Origin(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive by default, like any modern server
    server_version = "netlab-origin/1.0"
    name = "origin"

    def version_string(self) -> str:
        return self.server_version

    def log_message(self, format: str, *args: object) -> None:
        sys.stderr.write(f"[{self.name}] {self.client_address[0]} {format % args}\n")

    def reply(
        self,
        status: int,
        body: bytes = b"",
        ctype: str = "text/plain; charset=utf-8",
        headers: dict[str, str] | None = None,
    ) -> None:
        self.send_response(status)
        self.send_header("X-Served-By", self.name)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        if status not in (204, 304):  # these never have a body, so no length either
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD" and status not in (204, 304):
            self.wfile.write(body)

    def host_ok(self) -> bool:
        # HTTP/1.1 requires Host: it's how one IP address serves many websites.
        if self.request_version == "HTTP/1.1" and "Host" not in self.headers:
            self.reply(400, b"HTTP/1.1 request without a Host header\n")
            return False
        return True

    def do_HEAD(self) -> None:
        self.do_GET()

    def do_GET(self) -> None:
        if not self.host_ok():
            return
        url = urlsplit(self.path)
        query = parse_qs(url.query)
        path = url.path
        if path == "/hello":
            self.reply(200, f"hello from {self.name}\n".encode())
        elif path == "/headers":
            seen = {
                "request_line": self.requestline,
                "client": f"{self.client_address[0]}:{self.client_address[1]}",
                "headers": [f"{k}: {v}" for k, v in self.headers.items()],
            }
            self.reply(200, (json.dumps(seen, indent=2) + "\n").encode(), "application/json")
        elif path == "/chunked":
            self.send_chunked([b"first chunk\n", b"second chunk, a bit later\n", b"third and last\n"])
        elif path == "/slow":
            seconds = float(query.get("s", ["2"])[0])
            time.sleep(seconds)
            self.reply(200, f"slept {seconds:g} s\n".encode())
        elif path.startswith("/status/"):
            code = int(path.rsplit("/", 1)[1])
            self.reply(code, f"status {code}\n".encode())
        elif path == "/cache":
            etag = '"v1"'
            if self.headers.get("If-None-Match") == etag:
                self.reply(304, headers={"ETag": etag, "Cache-Control": "max-age=60"})
            else:
                self.reply(200, b"cacheable content\n", headers={"ETag": etag, "Cache-Control": "max-age=60"})
        elif path == "/redirect":
            self.reply(302, b"moved: go to /hello\n", headers={"Location": "/hello"})
        elif path == "/cookie":
            self.reply(200, b"cookie set\n", headers={"Set-Cookie": "session=abc123; Path=/; HttpOnly"})
        elif path == "/whoami":
            cookie = self.headers.get("Cookie", "(no cookie sent)")
            self.reply(200, f"your cookie: {cookie}\n".encode())
        elif path == "/big":
            self.reply(200, b"x" * int(query.get("n", ["1000000"])[0]), "application/octet-stream")
        else:
            self.reply(404, f"no page at {path} on {self.name}\n".encode())

    def do_POST(self) -> None:
        if not self.host_ok():
            return
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if urlsplit(self.path).path == "/echo":
            self.reply(200, body, self.headers.get("Content-Type", "application/octet-stream"))
        else:
            self.reply(404, b"POST only works on /echo\n")

    def send_chunked(self, chunks: list[bytes]) -> None:
        # No Content-Length: the body is a series of "<size in hex>\r\n<data>\r\n", ended by a 0-size chunk.
        # Servers use this when they don't know the length up front (streaming, generated pages).
        self.send_response(200)
        self.send_header("X-Served-By", self.name)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for chunk in chunks:
            self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            self.wfile.flush()
            time.sleep(0.3)
        self.wfile.write(b"0\r\n\r\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8001)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--name", default="origin")
    args = parser.parse_args()
    Origin.name = args.name
    server = ThreadingHTTPServer((args.bind, args.port), Origin)
    server.daemon_threads = True
    print(f"origin '{args.name}' on http://{args.bind}:{args.port}", file=sys.stderr, flush=True)
    with contextlib.suppress(KeyboardInterrupt):
        server.serve_forever()


if __name__ == "__main__":
    main()
