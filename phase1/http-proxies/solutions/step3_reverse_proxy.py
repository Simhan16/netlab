#!/usr/bin/env python3
"""Step 3 solution: a reverse proxy / load balancer, the kind that sits in front of a website.

    python3 step3_reverse_proxy.py PORT --backend /=127.0.0.1:8001 --backend /=127.0.0.1:8002 \\
                                        --backend /api=127.0.0.1:8003 [--timeout 2]

Clients think *it* is the website. For each request it:
  1. picks a route by the longest matching path prefix,
  2. picks a backend for that route in round-robin order, skipping ones that refuse connections,
  3. tells the backend who the real client was (X-Forwarded-For/-Proto/-Host),
  4. relays the response, or answers 502 (no backend reachable) or 504 (backend too slow) itself.
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
from httpwire import Head, read_body, read_head, send_error, strip_hop_by_hop  # noqa: E402

ROUTES: dict[str, list[tuple[str, int]]] = {}
COUNTERS: dict[str, itertools.count[int]] = {}
LOCK = threading.Lock()
TIMEOUT = 2.0


def route_for(path: str) -> str | None:
    matches = [
        prefix for prefix in ROUTES if path == prefix or path.startswith(prefix.rstrip("/") + "/") or prefix == "/"
    ]
    return max(matches, key=len) if matches else None


def backends_in_order(prefix: str) -> list[tuple[str, int]]:
    """This request's preferred backend first, then the others: round robin with failover."""
    pool = ROUTES[prefix]
    with LOCK:
        start = next(COUNTERS[prefix]) % len(pool)
    return pool[start:] + pool[:start]


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
    if not target.startswith("/"):
        send_error(client, 400, "Bad Request", "expected an origin-form request (GET /path)")
        return
    prefix = route_for(target.split("?", 1)[0])
    if prefix is None:
        send_error(client, 404, "Not Found", f"no route for {target}")
        return

    # The backend only sees the proxy's address as its peer, so pass the real client along.
    # Append, don't overwrite: an earlier proxy (a CDN, say) may already have added its client.
    headers = strip_hop_by_hop(head.headers)
    previous = [v for k, v in headers if k.lower() == "x-forwarded-for"]
    headers = [
        (k, v) for k, v in headers if k.lower() not in ("x-forwarded-for", "x-forwarded-proto", "x-forwarded-host")
    ]
    headers += [
        ("X-Forwarded-For", ", ".join(previous + [client_ip])),
        ("X-Forwarded-Proto", "http"),
        ("X-Forwarded-Host", head.get("Host", "")),
        ("Connection", "close"),
    ]
    request = Head(f"{method} {target} {version}", headers).encode() + body

    for host, port in backends_in_order(prefix):
        try:
            upstream = socket.create_connection((host, port), timeout=1)
        except OSError:
            # Nothing was sent, so trying the next backend is safe even for POST.
            print(f"backend {host}:{port} refused, trying the next", file=sys.stderr, flush=True)
            continue
        with upstream:
            upstream.sendall(request)
            upstream.settimeout(TIMEOUT)
            relayed = 0
            try:
                while chunk := upstream.recv(65536):
                    client.sendall(chunk)
                    relayed += len(chunk)
            except TimeoutError:
                if relayed == 0:
                    # Connected and sent, but no answer in time. Not retried: the backend may be
                    # processing the request, and doing it twice could charge a card twice.
                    send_error(client, 504, "Gateway Timeout", f"{host}:{port} did not answer within {TIMEOUT:g} s")
            except OSError:
                pass
        return
    send_error(client, 502, "Bad Gateway", f"no backend for {prefix} accepted the connection")


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
