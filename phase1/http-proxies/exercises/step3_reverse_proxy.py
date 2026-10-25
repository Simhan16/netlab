#!/usr/bin/env python3
"""Step 3: a reverse proxy / load balancer, the kind that sits in front of a website.

    python3 origin.py --port 8001 --name a &  python3 origin.py --port 8002 --name b &
    python3 step3_reverse_proxy.py 8080 --backend /=127.0.0.1:8001 --backend /=127.0.0.1:8002
    for i in 1 2 3 4; do curl -s http://127.0.0.1:8080/hello; done

Check it:  python3 check.py 3

What to build:
  - route by the longest matching path prefix, matching whole segments: /api matches /api and
    /api/orders, but not /apix. No match → 404 from the proxy itself.
  - round robin across a route's backends; if a backend refuses the connection, try the next
    one (safe: nothing was sent yet). None reachable → 502.
  - add X-Forwarded-For (append the client's IP to any existing value), X-Forwarded-Proto: http,
    X-Forwarded-Host: <the client's Host>. Keep Host as the client sent it.
  - if the backend sends nothing within TIMEOUT seconds → 504. Don't retry: it may be working on it.
"""

from __future__ import annotations

import argparse
import contextlib
import itertools
import socket
import socketserver
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from httpwire import Head, read_body, read_head, send_error, strip_hop_by_hop  # noqa: E402,F401

ROUTES: dict[str, list[tuple[str, int]]] = {}  # "/api" → [("127.0.0.1", 8003), ...]
COUNTERS: dict[str, itertools.count[int]] = {}  # per-route request counters, for round robin
LOCK = threading.Lock()  # handlers run in parallel threads
TIMEOUT = 2.0


def route_for(path: str) -> str | None:
    """The longest prefix in ROUTES that matches path on segment boundaries, or None."""
    # TODO 1
    return None


def backends_in_order(prefix: str) -> list[tuple[str, int]]:
    """The route's backends, starting with this request's round-robin pick, then the rest (for failover)."""
    # TODO 2: take next(COUNTERS[prefix]) under LOCK, and rotate ROUTES[prefix] by it.
    return list(ROUTES[prefix])


def handle(client: socket.socket, client_ip: str) -> None:
    buf = bytearray()
    try:
        head = read_head(client, buf)
        if head is None:
            return
        body = read_body(client, buf, head)
    except (ValueError, OSError) as e:
        send_error(client, 400, "Bad Request", str(e))
        return
    method, target, version = (head.start_line.split(" ", 2) + ["", ""])[:3]
    # TODO 3: 400 unless target starts with "/". route_for(target without its ?query), 404 if None.
    # TODO 4: headers: strip_hop_by_hop, then the X-Forwarded-* trio, plus "Connection: close".
    # TODO 5: for each backend in backends_in_order(): try socket.create_connection(timeout=1);
    #         on OSError try the next. On success: sendall the request, settimeout(TIMEOUT), and
    #         relay the response to the client chunk by chunk. A TimeoutError before the first
    #         byte → 504. If no backend connected → 502.
    _ = (method, target, version, body)
    send_error(client, 501, "Not Implemented", "step 3 is not written yet")


class Handler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        self.request.settimeout(30)
        handle(self.request, self.client_address[0])


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> None:
    global TIMEOUT
    parser = argparse.ArgumentParser()
    parser.add_argument("port", type=int)
    parser.add_argument("--backend", action="append", required=True, metavar="PREFIX=HOST:PORT")
    parser.add_argument("--timeout", type=float, default=2.0, help="seconds to wait for a backend's response")
    args = parser.parse_args()
    TIMEOUT = args.timeout
    for spec in args.backend:
        prefix, _, addr = spec.partition("=")
        host, _, port = addr.rpartition(":")
        ROUTES.setdefault(prefix, []).append((host, int(port)))
        COUNTERS.setdefault(prefix, itertools.count())
    with Server(("127.0.0.1", args.port), Handler) as server:
        print(f"reverse proxy on 127.0.0.1:{args.port}: {ROUTES}", file=sys.stderr, flush=True)
        with contextlib.suppress(KeyboardInterrupt):
            server.serve_forever()


if __name__ == "__main__":
    main()
