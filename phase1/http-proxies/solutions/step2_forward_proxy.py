#!/usr/bin/env python3
"""Step 2 solution: a forward proxy, the kind a company puts between its staff and the web.

    python3 step2_forward_proxy.py PORT [--block HOST ...]
    curl -x http://127.0.0.1:PORT http://127.0.0.1:8001/headers

Two kinds of request arrive at a forward proxy:
  GET http://host:port/path HTTP/1.1   (absolute-form) for plain HTTP: the proxy makes
                                        the request itself and relays the response.
  CONNECT host:443 HTTP/1.1            (authority-form) for HTTPS: the proxy opens a TCP
                                        connection, says 200, and from then on blindly copies
                                        encrypted bytes both ways. It never sees the content.
"""

from __future__ import annotations

import argparse
import contextlib
import socket
import socketserver
import sys
from pathlib import Path
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from httpwire import Head, pipe, read_body, read_head, relay, send_error, strip_hop_by_hop  # noqa: E402

BLOCKED: list[str] = []


def blocked(host: str) -> bool:
    host = host.lower().rstrip(".")
    return any(host == b or host.endswith("." + b) for b in BLOCKED)


def split_host_port(authority: str, default_port: int) -> tuple[str, int]:
    host, sep, port = authority.rpartition(":")
    if not sep or not port.isdigit():
        return authority, default_port
    return host.strip("[]"), int(port)


def connect_upstream(client: socket.socket, host: str, port: int) -> socket.socket | None:
    try:
        return socket.create_connection((host, port), timeout=5)
    except OSError as e:
        # The proxy couldn't reach the server: that's a 502 from the proxy, not an error from the site.
        send_error(client, 502, "Bad Gateway", f"proxy could not connect to {host}:{port} ({e})")
        return None


def handle(client: socket.socket, peer: str) -> None:
    buf = bytearray()
    try:
        head = read_head(client, buf)
    except (ValueError, OSError) as e:
        send_error(client, 400, "Bad Request", str(e))
        return
    if head is None:
        return
    method, target, version = (head.start_line.split(" ", 2) + ["", ""])[:3]
    print(f"{peer} {method} {target}", file=sys.stderr, flush=True)

    if method == "CONNECT":
        host, port = split_host_port(target, 443)
        if blocked(host):
            send_error(client, 403, "Forbidden", f"{host} is blocked by policy")
            return
        upstream = connect_upstream(client, host, port)
        if upstream is None:
            return
        with upstream:
            upstream.settimeout(None)
            client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            if buf:  # the client may have sent the start of the TLS handshake already
                upstream.sendall(bytes(buf))
            pipe(client, upstream)
        return

    url = urlsplit(target)
    if url.scheme != "http" or not url.hostname:
        send_error(client, 400, "Bad Request", "this is a proxy: send absolute-form requests (GET http://host/path)")
        return
    if blocked(url.hostname):
        send_error(client, 403, "Forbidden", f"{url.hostname} is blocked by policy")
        return
    try:
        body = read_body(client, buf, head)
    except ValueError as e:
        send_error(client, 400, "Bad Request", str(e))
        return

    # Rewrite for the origin: origin-form request line, no hop-by-hop headers, a Via header
    # announcing that a proxy handled it, and one request per connection to keep this simple.
    path = (url.path or "/") + (f"?{url.query}" if url.query else "")
    headers = [(k, v) for k, v in strip_hop_by_hop(head.headers) if k.lower() != "host"]
    host_header = url.hostname + (f":{url.port}" if url.port else "")
    outgoing = Head(
        f"{method} {path} {version}",
        [("Host", host_header), *headers, ("Via", "1.1 netlab-proxy"), ("Connection", "close")],
    )

    upstream = connect_upstream(client, url.hostname, url.port or 80)
    if upstream is None:
        return
    with upstream:
        upstream.settimeout(30)
        upstream.sendall(outgoing.encode() + body)
        relay(upstream, client)  # the origin closes after its response (Connection: close)


class Handler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        self.request.settimeout(30)
        handle(self.request, f"{self.client_address[0]}:{self.client_address[1]}")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True  # one thread per client: a slow download doesn't hold up anyone else


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("port", type=int)
    parser.add_argument("--block", action="append", default=[], help="hostname to refuse (and its subdomains)")
    args = parser.parse_args()
    BLOCKED.extend(b.lower() for b in args.block)
    with Server(("127.0.0.1", args.port), Handler) as server:
        print(f"forward proxy on 127.0.0.1:{args.port}, blocking {BLOCKED or 'nothing'}", file=sys.stderr, flush=True)
        with contextlib.suppress(KeyboardInterrupt):
            server.serve_forever()


if __name__ == "__main__":
    main()
