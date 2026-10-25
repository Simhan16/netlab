#!/usr/bin/env python3
"""Tests for the http-proxies steps. Each one targets a classic mistake.

    python3 check.py 1               # test your exercises/step1_client.py
    python3 check.py 2 --solution    # test the reference forward proxy
    python3 check.py all --solution

Stdlib only; everything runs on 127.0.0.1.
"""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import json
import socket
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Iterator
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
STEPS = {1: "step1_client", 2: "step2_forward_proxy", 3: "step3_reverse_proxy"}


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_listening(port: int, proc: subprocess.Popen[bytes] | None = None) -> None:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if proc is not None and proc.poll() is not None:
            err = proc.stderr.read().decode(errors="replace").strip() if proc.stderr else ""
            raise Failure(f"process exited at startup (code {proc.returncode}): {err[-400:] or '(no output)'}")
        with contextlib.suppress(OSError), socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return
        time.sleep(0.05)
    raise Failure(f"nothing started listening on 127.0.0.1:{port}")


@contextlib.contextmanager
def process(*args: str) -> Iterator[subprocess.Popen[bytes]]:
    proc = subprocess.Popen([sys.executable, *args], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        yield proc
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        if proc.stderr:
            proc.stderr.close()


@contextlib.contextmanager
def origin(name: str = "origin") -> Iterator[int]:
    port = free_port()
    with process(str(HERE / "origin.py"), "--port", str(port), "--name", name) as proc:
        wait_listening(port, proc)
        yield port


def exchange(port: int, raw: bytes, timeout: float = 8) -> bytes:
    """Sends a raw request and reads until the other side closes."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(raw)
        out = bytearray()
        try:
            while chunk := s.recv(65536):
                out += chunk
        except TimeoutError:
            raise Failure(f"no complete response within {timeout:g} s (got {len(out)} bytes)") from None
        except ConnectionResetError:
            pass
        return bytes(out)


def split_response(raw: bytes) -> tuple[int, dict[str, str], bytes]:
    head, sep, body = raw.partition(b"\r\n\r\n")
    expect(bool(sep), f"response has no complete head: {raw[:80]!r}")
    lines = head.decode("latin-1").split("\r\n")
    parts = lines[0].split(" ")
    expect(len(parts) >= 2 and parts[1].isdigit(), f"bad status line: {lines[0]!r}")
    headers = {}
    for line in lines[1:]:
        key, _, value = line.partition(":")
        headers[key.strip().lower()] = value.strip()
    return int(parts[1]), headers, body


def seen_headers(body: bytes) -> tuple[str, dict[str, str]]:
    """Parses origin.py's /headers JSON into (request line, {lowercased name: value})."""
    data = json.loads(body)
    return data["request_line"], {h.split(":", 1)[0].lower(): h.split(":", 1)[1].strip() for h in data["headers"]}


# --- step 1: the keep-alive client --------------------------------------------------------------

CHUNKED_BODY = b"0123456789abcdefghijklmnopqrstuvwxyz" * 3


class KeepAliveServer:
    """Serves any number of requests per connection, deliberately dribbling the bytes, and counts
    connections. Responses: /cl (Content-Length), /chunked, /empty (204), /notmod (304), /big."""

    def __init__(self) -> None:
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.connections = 0
        self.problems: list[str] = []
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self) -> None:
        with contextlib.suppress(OSError):
            while True:
                conn, _ = self.sock.accept()
                self.connections += 1
                threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _send(self, conn: socket.socket, data: bytes) -> None:
        # The first 400 bytes (the head and the chunk framing) go out 7 at a time, so no recv()
        # ever sees a whole line or message. The rest, if any, goes in one piece.
        for i in range(0, min(len(data), 400), 7):
            conn.sendall(data[i : i + 7])
            time.sleep(0.0005)
        conn.sendall(data[400:])

    def _serve(self, conn: socket.socket) -> None:
        buf = b""
        with conn, contextlib.suppress(OSError):
            conn.settimeout(10)
            while True:
                while b"\r\n\r\n" not in buf:
                    chunk = conn.recv(4096)
                    if not chunk:
                        return
                    buf += chunk
                head, _, buf = buf.partition(b"\r\n\r\n")
                lines = head.decode("latin-1").split("\r\n")
                if not lines[0].endswith("HTTP/1.1"):
                    self.problems.append(f"request line {lines[0]!r} isn't HTTP/1.1")
                if not any(line.lower().startswith("host:") for line in lines[1:]):
                    self.problems.append("request without a Host header")
                path = lines[0].split(" ")[1] if " " in lines[0] else ""
                self._send(conn, self._response(path))

    @staticmethod
    def _response(path: str) -> bytes:
        if path == "/cl":
            return b"HTTP/1.1 200 OK\r\nContent-Length: 18\r\nContent-Type: text/plain\r\n\r\nexactly 18 bytes!\n"
        if path == "/chunked":
            # Uneven chunks, upper- and lowercase hex, a chunk extension and a trailer: all legal.
            pieces = [CHUNKED_BODY[:10], CHUNKED_BODY[10:36], CHUNKED_BODY[36:]]
            body = b"%X\r\n%s\r\n" % (len(pieces[0]), pieces[0])
            body += b"%x;note=hi\r\n%s\r\n" % (len(pieces[1]), pieces[1])
            body += b"%x\r\n%s\r\n" % (len(pieces[2]), pieces[2])
            body += b"0\r\nX-Trailer: done\r\n\r\n"
            return b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + body
        if path == "/empty":
            return b"HTTP/1.1 204 No Content\r\nX-Note: no body follows\r\n\r\n"
        if path == "/notmod":
            return b'HTTP/1.1 304 Not Modified\r\nETag: "v1"\r\nContent-Length: 999\r\n\r\n'
        if path == "/big":
            return b"HTTP/1.1 200 OK\r\nContent-Length: 300000\r\n\r\n" + b"b" * 300000
        return b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"

    def close(self) -> None:
        self.sock.close()


def load_client(solution: bool) -> ModuleType:
    path = HERE / ("solutions" if solution else "exercises") / "step1_client.py"
    spec = importlib.util.spec_from_file_location("step1_client_under_test", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses look their module up here
    try:
        spec.loader.exec_module(module)
    except Exception as e:  # noqa: BLE001 (a syntax error in the exercise is a test failure, not a crash)
        raise Failure(f"importing {path.name} failed: {type(e).__name__}: {e}") from None
    return module


def step1_tests(solution: bool) -> list[tuple[str, Callable[[], None]]]:
    def fetch(paths: list[str]) -> tuple[list[object], KeepAliveServer]:
        client = load_client(solution)
        server = KeepAliveServer()
        result: list[object] = []
        error: list[BaseException] = []

        def run() -> None:
            try:
                result.extend(client.fetch_all("127.0.0.1", server.port, paths))
            except BaseException as e:  # noqa: BLE001 (report anything, including NotImplementedError)
                error.append(e)

        worker = threading.Thread(target=run, daemon=True)
        worker.start()
        worker.join(timeout=15)
        server.close()
        expect(
            not worker.is_alive(), "fetch_all is still running after 15 s: is it waiting for a body that never comes?"
        )
        if error:
            raise Failure(f"fetch_all raised {type(error[0]).__name__}: {error[0]}")
        expect(not server.problems, "; ".join(server.problems[:2]))
        return result, server

    def content_length() -> None:
        (r,), _ = fetch(["/cl"])
        expect(r.status == 200 and r.body == b"exactly 18 bytes!\n", f"got status {r.status}, body {r.body!r}")
        expect(r.headers.get("content-type") == "text/plain", "headers should be a dict with lowercased names")

    def chunked() -> None:
        (r,), _ = fetch(["/chunked"])
        expect(r.body == CHUNKED_BODY, f"decoded chunked body is wrong: {r.body[:40]!r}... ({len(r.body)} bytes)")

    def no_body() -> None:
        rs, _ = fetch(["/empty", "/notmod", "/cl"])
        expect([r.status for r in rs] == [204, 304, 200], f"statuses {[r.status for r in rs]}")
        expect(rs[1].body == b"", "a 304 has no body, even when it carries a Content-Length")
        expect(rs[2].body == b"exactly 18 bytes!\n", "the response after 204/304 came out wrong: framing drifted")

    def keep_alive() -> None:
        paths = ["/cl", "/chunked", "/empty", "/notmod", "/big", "/cl"]
        rs, server = fetch(paths)
        expect(len(rs) == len(paths), f"{len(rs)} responses for {len(paths)} requests")
        expect(
            server.connections == 1, f"used {server.connections} TCP connections for {len(paths)} requests: reuse one"
        )
        expect(
            len(rs[4].body) == 300000 and rs[5].body == b"exactly 18 bytes!\n",
            "large body or the one after it is wrong",
        )

    return [
        ("Content-Length body, bytes arriving 7 at a time", content_length),
        ("chunked body (hex sizes, extension, trailer)", chunked),
        ("204 and 304 have no body", no_body),
        ("six requests over one keep-alive connection", keep_alive),
    ]


# --- step 2: the forward proxy --------------------------------------------------------------------


@contextlib.contextmanager
def proxy(solution: bool, step: int, *args: str) -> Iterator[int]:
    port = free_port()
    script = HERE / ("solutions" if solution else "exercises") / f"{STEPS[step]}.py"
    with process(str(script), str(port), *args) as proc:
        wait_listening(port, proc)
        yield port


def step2_tests(solution: bool) -> list[tuple[str, Callable[[], None]]]:
    def absolute_form() -> None:
        with origin() as op, proxy(solution, 2) as pp:
            raw = exchange(
                pp,
                (
                    f"GET http://127.0.0.1:{op}/headers HTTP/1.1\r\nHost: 127.0.0.1:{op}\r\n"
                    "Proxy-Connection: keep-alive\r\nUser-Agent: check\r\n\r\n"
                ).encode(),
            )
            status, _, body = split_response(raw)
            expect(status == 200, f"status {status}: {body[:100]!r}")
            line, headers = seen_headers(body)
            expect(line == "GET /headers HTTP/1.1", f"origin saw {line!r}: rewrite absolute-form to origin-form")
            expect("via" in headers, "no Via header reached the origin")
            expect("proxy-connection" not in headers, "Proxy-Connection is hop-by-hop: don't forward it")

    def connection_listed() -> None:
        with origin() as op, proxy(solution, 2) as pp:
            raw = exchange(
                pp,
                (
                    f"GET http://127.0.0.1:{op}/headers HTTP/1.1\r\nHost: 127.0.0.1:{op}\r\n"
                    "Connection: X-Secret\r\nX-Secret: hop-only\r\n\r\n"
                ).encode(),
            )
            status, _, body = split_response(raw)
            expect(status == 200, f"status {status}: {body[:80]!r}")
            _, headers = seen_headers(body)
            expect("x-secret" not in headers, "a header named in Connection is hop-by-hop too: strip it")

    def post_body() -> None:
        with origin() as op, proxy(solution, 2) as pp:
            payload = b"name=netlab&step=2"
            raw = exchange(
                pp,
                (
                    f"POST http://127.0.0.1:{op}/echo HTTP/1.1\r\nHost: 127.0.0.1:{op}\r\n"
                    f"Content-Type: text/plain\r\nContent-Length: {len(payload)}\r\n\r\n"
                ).encode()
                + payload,
            )
            status, _, body = split_response(raw)
            expect(status == 200 and body == payload, f"status {status}, echoed {body!r}")

    def connect_tunnel() -> None:
        with origin() as op, proxy(solution, 2) as pp, socket.create_connection(("127.0.0.1", pp), timeout=8) as s:
            s.sendall(f"CONNECT 127.0.0.1:{op} HTTP/1.1\r\nHost: 127.0.0.1:{op}\r\n\r\n".encode())
            reply = b""
            while b"\r\n\r\n" not in reply:
                chunk = s.recv(4096)
                expect(bool(chunk), "proxy closed the connection instead of answering CONNECT")
                reply += chunk
            expect(reply.split(b" ")[1] == b"200", f"CONNECT answered {reply[:40]!r}")
            # Inside the tunnel the proxy is just a pipe: speak to the origin directly.
            s.sendall(f"GET /hello HTTP/1.1\r\nHost: 127.0.0.1:{op}\r\nConnection: close\r\n\r\n".encode())
            rest = bytearray()
            while chunk := s.recv(65536):
                rest += chunk
            expect(rest.endswith(b"hello from origin\n"), f"through the tunnel got {bytes(rest[-60:])!r}")

    def blocked() -> None:
        with proxy(solution, 2, "--block", "blocked.test") as pp:
            status, _, _ = split_response(
                exchange(pp, b"GET http://www.blocked.test/ HTTP/1.1\r\nHost: www.blocked.test\r\n\r\n")
            )
            expect(status == 403, f"GET to a blocked host (subdomain) answered {status}, not 403")
            status, _, _ = split_response(
                exchange(pp, b"CONNECT blocked.test:443 HTTP/1.1\r\nHost: blocked.test:443\r\n\r\n")
            )
            expect(status == 403, f"CONNECT to a blocked host answered {status}, not 403")

    def upstream_down() -> None:
        dead = free_port()
        with proxy(solution, 2) as pp:
            status, _, _ = split_response(
                exchange(pp, f"GET http://127.0.0.1:{dead}/ HTTP/1.1\r\nHost: x\r\n\r\n".encode())
            )
            expect(status == 502, f"unreachable server answered {status}, not 502 Bad Gateway")

    def origin_form() -> None:
        with proxy(solution, 2) as pp:
            status, _, _ = split_response(exchange(pp, b"GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"))
            expect(status == 400, f"an origin-form request (not meant for a proxy) answered {status}, not 400")

    def concurrent() -> None:
        with origin() as op, proxy(solution, 2) as pp:
            slow = threading.Thread(
                target=exchange,
                daemon=True,
                args=(pp, f"GET http://127.0.0.1:{op}/slow?s=3 HTTP/1.1\r\nHost: 127.0.0.1:{op}\r\n\r\n".encode()),
            )
            slow.start()
            time.sleep(0.2)
            started = time.monotonic()
            raw = exchange(pp, f"GET http://127.0.0.1:{op}/hello HTTP/1.1\r\nHost: 127.0.0.1:{op}\r\n\r\n".encode())
            took = time.monotonic() - started
            expect(raw.endswith(b"hello from origin\n"), f"the fast request failed: {raw[:60]!r}")
            expect(took < 1.5, f"a fast request waited {took:.1f} s behind a slow one: serve clients concurrently")
            slow.join()

    return [
        ("absolute-form GET rewritten, Via added, hop-by-hop dropped", absolute_form),
        ("headers named in Connection are dropped", connection_listed),
        ("POST body forwarded", post_body),
        ("CONNECT tunnel carries raw bytes", connect_tunnel),
        ("blocked hosts get 403 (GET and CONNECT)", blocked),
        ("unreachable server → 502", upstream_down),
        ("origin-form request → 400", origin_form),
        ("slow client doesn't block others", concurrent),
    ]


# --- step 3: the reverse proxy --------------------------------------------------------------------


def get(port: int, path: str, extra: str = "", timeout: float = 8) -> tuple[int, dict[str, str], bytes]:
    return split_response(exchange(port, f"GET {path} HTTP/1.1\r\nHost: shop.test\r\n{extra}\r\n".encode(), timeout))


def step3_tests(solution: bool) -> list[tuple[str, Callable[[], None]]]:
    def routing_and_balancing() -> None:
        with (
            origin("a") as a,
            origin("b") as b,
            origin("api") as c,
            proxy(
                solution,
                3,
                "--backend",
                f"/=127.0.0.1:{a}",
                "--backend",
                f"/=127.0.0.1:{b}",
                "--backend",
                f"/api=127.0.0.1:{c}",
            ) as rp,
        ):
            served = [get(rp, "/hello")[1].get("x-served-by") for _ in range(4)]
            expect(
                sorted(str(x) for x in served) == ["a", "a", "b", "b"],
                f"4 requests to / were served by {served}: round-robin a and b",
            )
            expect(get(rp, "/api/orders")[1].get("x-served-by") == "api", "/api/orders didn't go to the /api backend")
            expect(
                get(rp, "/apix")[1].get("x-served-by") in ("a", "b"),
                "/apix matched the /api route: match whole segments",
            )

    def forwarded_headers() -> None:
        with origin("a") as a, proxy(solution, 3, "--backend", f"/=127.0.0.1:{a}") as rp:
            status, _, body = get(rp, "/headers", "X-Forwarded-For: 203.0.113.7\r\n")
            expect(status == 200, f"status {status}")
            _, headers = seen_headers(body)
            expect(
                headers.get("x-forwarded-for") == "203.0.113.7, 127.0.0.1",
                f"X-Forwarded-For was {headers.get('x-forwarded-for')!r}: append the client to the existing list",
            )
            expect(headers.get("x-forwarded-proto") == "http", "missing X-Forwarded-Proto: http")
            expect(
                headers.get("x-forwarded-host") == "shop.test",
                "missing X-Forwarded-Host (the Host the client asked for)",
            )
            expect(
                headers.get("host") == "shop.test",
                "keep the client's Host header, as nginx's proxy_set_header Host $host does",
            )

    def failover() -> None:
        dead = free_port()
        with (
            origin("a") as a,
            proxy(solution, 3, "--backend", f"/=127.0.0.1:{dead}", "--backend", f"/=127.0.0.1:{a}") as rp,
        ):
            results = [get(rp, "/hello") for _ in range(4)]
            expect(
                all(r[0] == 200 and r[1].get("x-served-by") == "a" for r in results),
                f"with one backend down got {[r[0] for r in results]}: skip backends that refuse connections",
            )

    def all_down() -> None:
        with proxy(solution, 3, "--backend", f"/=127.0.0.1:{free_port()}") as rp:
            expect(get(rp, "/hello")[0] == 502, "no reachable backend should be 502 Bad Gateway")

    def slow_backend() -> None:
        with origin("a") as a, proxy(solution, 3, "--backend", f"/=127.0.0.1:{a}", "--timeout", "1") as rp:
            started = time.monotonic()
            status = get(rp, "/slow?s=4")[0]
            took = time.monotonic() - started
            expect(
                status == 504 and took < 2.5,
                f"backend slower than --timeout gave {status} after {took:.1f} s, not a quick 504",
            )

    def no_route() -> None:
        with origin("a") as a, proxy(solution, 3, "--backend", f"/api=127.0.0.1:{a}") as rp:
            status, headers, _ = get(rp, "/other")
            expect(status == 404 and "x-served-by" not in headers, f"/other with only an /api route gave {status}")

    return [
        ("longest-prefix routing + round robin", routing_and_balancing),
        ("X-Forwarded-For/-Proto/-Host", forwarded_headers),
        ("fails over past a dead backend", failover),
        ("all backends down → 502", all_down),
        ("slow backend → 504", slow_backend),
        ("no matching route → 404 from the proxy", no_route),
    ]


TESTS = {1: step1_tests, 2: step2_tests, 3: step3_tests}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("step", help="1, 2, 3 or 'all'")
    parser.add_argument("--solution", action="store_true", help="test the reference solution instead of yours")
    args = parser.parse_args()
    steps = list(TESTS) if args.step == "all" else [int(args.step)]
    failed = 0
    for step in steps:
        print(f"step {step}: {STEPS[step]}{' (solution)' if args.solution else ''}")
        for name, test in TESTS[step](args.solution):
            try:
                test()
                print(f"  PASS  {name}")
            except Failure as e:
                failed += 1
                print(f"  FAIL  {name}: {e}")
            except Exception as e:  # noqa: BLE001 (any crash in a test is that test failing)
                failed += 1
                print(f"  FAIL  {name}: {type(e).__name__}: {e}")
    print("all passed" if failed == 0 else f"{failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
