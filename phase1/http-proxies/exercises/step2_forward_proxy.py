#!/usr/bin/env python3
"""Step 2: a forward proxy, the kind a company puts between its staff and the web.

    python3 step2_forward_proxy.py PORT [--block HOST ...]
    curl -v -x http://127.0.0.1:PORT http://127.0.0.1:8001/headers     (with origin.py running)
    curl -v -x http://127.0.0.1:PORT https://example.com

Check it:  python3 check.py 2

What to build, in handle():
  - CONNECT host:port   → blocked? 403. Otherwise connect, answer "HTTP/1.1 200 Connection
                          Established\\r\\n\\r\\n", then pipe() bytes both ways (HTTPS goes
                          through encrypted: you never see inside).
  - GET http://host:port/path (absolute-form) → blocked? 403. Otherwise rewrite to origin-form
                          ("GET /path HTTP/1.1"), drop hop-by-hop headers, add "Via: 1.1 netlab-proxy"
                          and "Connection: close", send it (plus any body), relay() the response back.
  - anything else       → 400 (someone talked to the proxy as if it were a website)
  - can't connect upstream → 502 Bad Gateway
Read httpwire.py first: read_head, read_body, strip_hop_by_hop, send_error, relay and pipe are done for you.
"""

from __future__ import annotations

import argparse
import contextlib
import socket
import socketserver
import sys
from pathlib import Path
from urllib.parse import urlsplit  # noqa: F401 (urlsplit("http://h:1/p?q").hostname / .port / .path / .query)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from httpwire import Head, pipe, read_body, read_head, relay, send_error, strip_hop_by_hop  # noqa: E402,F401

BLOCKED: list[str] = []


def blocked(host: str) -> bool:
    # TODO 1: True if host is in BLOCKED *or is a subdomain of one*: blocking example.org must
    #         also block www.example.org. Compare lowercased.
    return False


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

    # TODO 2: CONNECT (target is "host:port"). Remember that `buf` may already hold the first
    #         bytes the client sent after its CONNECT head: forward them before pipe().
    # TODO 3: absolute-form GET/POST/...: parse with urlsplit, check the blocklist, read_body(),
    #         build the outgoing Head, connect (socket.create_connection, timeout=5), send, relay.
    # TODO 4: errors: 400 for origin-form requests, 502 when the upstream connect fails.
    send_error(client, 501, "Not Implemented", "step 2 is not written yet")


class Handler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        self.request.settimeout(30)
        handle(self.request, f"{self.client_address[0]}:{self.client_address[1]}")


# TODO 5: a TCPServer serves one client at a time, so one slow download blocks everyone.
#         The check's concurrency test will tell you. socketserver has a threading variant.
class Server(socketserver.TCPServer):
    allow_reuse_address = True


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
