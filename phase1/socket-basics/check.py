#!/usr/bin/env python3
"""Tests for the socket-basics steps. Each test is a client (or server) built to
catch one classic mistake: assuming one recv() is one message, forgetting
SO_REUSEADDR, dying when a client resets, trusting a length from the network...

    python3 check.py 3               # test your exercises/step3_server.cpp
    python3 check.py 3 --solution    # test the reference solution
    python3 check.py all --solution

Build first (cd phase1 && cmake -B build && cmake --build build). Stdlib only.
"""

from __future__ import annotations

import argparse
import contextlib
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Iterator
from pathlib import Path

HERE = Path(__file__).resolve().parent
STEP_NAMES = {1: "step1_client", 2: "step2_client", 3: "step3_server", 4: "step4_framed_server",
              5: "step5_udp_server", 6: "step6_chat_server"}


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def free_port(kind: int = socket.SOCK_STREAM) -> int:
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def recv_all(sock: socket.socket, n: int, timeout: float = 5.0) -> bytes:
    """Reads exactly n bytes (or fails): the harness itself never assumes one recv is enough."""
    sock.settimeout(timeout)
    out = bytearray()
    while len(out) < n:
        try:
            chunk = sock.recv(n - len(out))
        except TimeoutError:
            raise Failure(f"timed out after {len(out)} of {n} expected bytes") from None
        except ConnectionError as e:
            raise Failure(f"connection broke after {len(out)} of {n} expected bytes: {e}") from None
        if not chunk:
            raise Failure(f"server closed the connection after {len(out)} of {n} expected bytes")
        out += chunk
    return bytes(out)


def nothing_arrives(sock: socket.socket, wait: float = 0.3) -> bool:
    """True if no data shows up within `wait` seconds."""
    sock.settimeout(wait)
    try:
        return sock.recv(1) == b""  # b"": closed without sending anything
    except TimeoutError:
        return True


def closed_by_peer(sock: socket.socket, timeout: float = 3.0) -> bool:
    sock.settimeout(timeout)
    try:
        while sock.recv(65536):
            pass
        return True
    except ConnectionError:
        return True
    except TimeoutError:
        return False


# --- running the program under test ----------------------------------------------------------

class Program:
    def __init__(self, build_dir: Path, solution: bool) -> None:
        self.build_dir, self.solution = build_dir, solution

    def command(self, step: int) -> list[str]:
        name = STEP_NAMES[step]
        if step == 1:
            return [sys.executable, str(HERE / ("solutions" if self.solution else "exercises") / f"{name}.py")]
        binary = self.build_dir / (f"solution_{name}" if self.solution else name)
        if not binary.exists():
            raise Failure(f"{binary} not found: build first (cd phase1 && cmake -B build && cmake --build build)")
        return [str(binary)]


@contextlib.contextmanager
def server(program: Program, step: int, port: int, udp: bool = False) -> Iterator[subprocess.Popen[bytes]]:
    proc = subprocess.Popen([*program.command(step), str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        wait_ready(proc, port, udp)
        yield proc
    finally:
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        if proc.stderr:
            proc.stderr.close()


def wait_ready(proc: subprocess.Popen[bytes], port: int, udp: bool) -> None:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            err = proc.stderr.read().decode(errors="replace").strip() if proc.stderr else ""
            raise Failure(f"server exited with code {proc.returncode} at startup: {err or '(no output)'}")
        if udp:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.settimeout(0.2)
                s.sendto(b"ping", ("127.0.0.1", port))
                with contextlib.suppress(OSError):
                    if s.recvfrom(100)[0]:
                        return
        else:
            with contextlib.suppress(OSError), socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
            time.sleep(0.05)
    raise Failure(f"server never started accepting on 127.0.0.1:{port}")


def alive(proc: subprocess.Popen[bytes]) -> bool:
    return proc.poll() is None


def connect(port: int) -> socket.socket:
    return socket.create_connection(("127.0.0.1", port), timeout=5)


# --- steps 1 and 2: the HTTP client ----------------------------------------------------------

BODY = b"".join(b"line %04d of the netlab test page\n" % i for i in range(150))
RESPONSE = b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n" % len(BODY) + BODY


class FakeHttpServer:
    """Answers one request, deliberately dribbling the response out in uneven pieces."""

    def __init__(self, host: str = "127.0.0.1") -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind((host, 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.request = b""
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self) -> None:
        self.sock.settimeout(10)
        with contextlib.suppress(OSError):
            conn, _ = self.sock.accept()
            with conn:
                conn.settimeout(5)
                while b"\r\n\r\n" not in self.request:
                    chunk = conn.recv(4096)
                    if not chunk:
                        return
                    self.request += chunk
                pieces = [RESPONSE[:5], RESPONSE[5:6], RESPONSE[6:80], RESPONSE[80:2000], RESPONSE[2000:]]
                for piece in pieces:
                    conn.sendall(piece)
                    time.sleep(0.05)

    def close(self) -> None:
        self.sock.close()


def run_client(program: Program, step: int, *args: str) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run([*program.command(step), *args], check=False, capture_output=True, timeout=15)
    except subprocess.TimeoutExpired:
        raise Failure("client did not exit within 15 s (does it stop reading when recv returns 0?)") from None


def client_tests(step: int) -> list[tuple[str, Callable[[Program], None]]]:
    def full_response(program: Program) -> None:
        srv = FakeHttpServer()
        try:
            result = run_client(program, step, "127.0.0.1", str(srv.port), "/hello")
        finally:
            srv.close()
        expect(result.returncode == 0, f"exit code {result.returncode}, stderr: {result.stderr.decode(errors='replace')}")
        expect(srv.request.startswith(b"GET /hello HTTP/1.0\r\n"), f"request line was {srv.request[:40]!r}")
        expect(b"\r\nHost: 127.0.0.1\r\n" in srv.request, "request has no 'Host: 127.0.0.1' header")
        expect(result.stdout == RESPONSE,
               f"printed {len(result.stdout)} bytes, expected {len(RESPONSE)} (the response arrives in 5 pieces)")

    def localhost(program: Program) -> None:
        srv = FakeHttpServer("127.0.0.1")  # IPv4 only, while "localhost" often resolves to ::1 first
        try:
            result = run_client(program, step, "localhost", str(srv.port), "/")
        finally:
            srv.close()
        expect(result.returncode == 0 and result.stdout == RESPONSE,
               "failed against 'localhost' with an IPv4-only server: try every address getaddrinfo returns")

    def refused(program: Program) -> None:
        result = run_client(program, step, "127.0.0.1", str(free_port()), "/")
        expect(result.returncode == 2, f"expected exit code 2 for a refused connection, got {result.returncode}")
        expect(result.stderr.strip() != b"", "no error message on stderr")

    def bad_name(program: Program) -> None:
        result = run_client(program, step, "no-such-host.invalid", "80", "/")
        expect(result.returncode == 2, f"expected exit code 2 for an unresolvable name, got {result.returncode}")
        expect(result.stderr.strip() != b"", "no error message on stderr")

    return [("whole response, delivered in uneven pieces", full_response),
            ("tries every address for 'localhost'", localhost),
            ("connection refused → exit 2 with a message", refused),
            ("unresolvable host → exit 2 with a message", bad_name)]


# --- step 3: the uppercase server ------------------------------------------------------------

def step3_tests() -> list[tuple[str, Callable[[Program], None]]]:
    def basic(program: Program) -> None:
        port = free_port()
        with server(program, 3, port), connect(port) as c:
            c.sendall(b"hello, world\n")
            expect(recv_all(c, 13) == b"HELLO, WORLD\n", "wrong reply")

    def large(program: Program) -> None:
        port = free_port()
        data = bytes(range(97, 123)) * 40330  # ~1 MiB of a-z
        with server(program, 3, port), connect(port) as c:
            sender = threading.Thread(target=c.sendall, args=(data,), daemon=True)
            sender.start()  # send and receive at the same time, or both sides' buffers fill and deadlock
            reply = recv_all(c, len(data), timeout=15)
            sender.join()
            expect(reply == data.upper(), "1 MiB came back altered")

    def sequential(program: Program) -> None:
        port = free_port()
        with server(program, 3, port), connect(port) as first, connect(port) as second:
            second.sendall(b"second\n")  # queued: the server is still busy with `first`
            first.sendall(b"first\n")
            expect(recv_all(first, 6) == b"FIRST\n", "first client got a wrong reply")
            first.close()
            expect(recv_all(second, 7) == b"SECOND\n", "second client was not served after the first left")

    def reset_mid_write(program: Program) -> None:
        port = free_port()
        with server(program, 3, port) as proc:
            c = connect(port)
            c.setblocking(False)
            with contextlib.suppress(BlockingIOError):
                for _ in range(400):  # far more than fits in both sides' buffers, never reading the replies
                    c.send(b"x" * 65536)
            time.sleep(0.3)
            c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            c.close()  # linger 0 → RST: the server's blocked send fails with ECONNRESET
            time.sleep(0.3)
            expect(alive(proc), f"server died (exit {proc.returncode}): a send/recv error must end the client, not the server")
            with connect(port) as c2:
                c2.sendall(b"still there?\n")
                expect(recv_all(c2, 13) == b"STILL THERE?\n", "server stopped serving after a client reset")

    def restart(program: Program) -> None:
        port = free_port()
        with server(program, 3, port) as proc, connect(port) as c:
            c.sendall(b"x")
            recv_all(c, 1)
            proc.send_signal(signal.SIGTERM)  # the server closes first → TIME_WAIT on its port
            proc.wait(timeout=3)
            closed_by_peer(c)
        try:
            with server(program, 3, port), connect(port) as c:
                c.sendall(b"back\n")
                expect(recv_all(c, 5) == b"BACK\n", "wrong reply after restart")
        except Failure as e:
            raise Failure(f"restart on the same port failed ({e}): set SO_REUSEADDR before bind()") from None

    return [("uppercases a line", basic), ("1 MiB round trip", large),
            ("second client is served after the first", sequential),
            ("survives a client that resets mid-write", reset_mid_write),
            ("restarts immediately on the same port (SO_REUSEADDR)", restart)]


# --- step 4: framing --------------------------------------------------------------------------

def frame(payload: bytes) -> bytes:
    return struct.pack("!I", len(payload)) + payload


def read_frame(sock: socket.socket) -> bytes:
    (length,) = struct.unpack("!I", recv_all(sock, 4))
    return recv_all(sock, length)


def step4_tests() -> list[tuple[str, Callable[[Program], None]]]:
    def one(program: Program) -> None:
        port = free_port()
        with server(program, 4, port), connect(port) as c:
            c.sendall(frame(b"framed hello"))
            expect(read_frame(c) == b"FRAMED HELLO", "wrong reply")

    def dribble(program: Program) -> None:
        port = free_port()
        with server(program, 4, port), connect(port) as c:
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # really send each byte on its own
            try:
                for byte in frame(b"one byte at a time"):
                    c.sendall(bytes([byte]))
                    time.sleep(0.005)
            except OSError:
                raise Failure("server closed the connection mid-message: one recv() is not one message") from None
            expect(read_frame(c) == b"ONE BYTE AT A TIME", "message sent one byte per segment came back wrong")

    def coalesced(program: Program) -> None:
        port = free_port()
        with server(program, 4, port), connect(port) as c:
            c.sendall(frame(b"alpha") + frame(b"") + frame(b"gamma"))  # three messages in one send
            replies = [read_frame(c) for _ in range(3)]
            expect(replies == [b"ALPHA", b"", b"GAMMA"], f"got {replies!r}")

    def big(program: Program) -> None:
        port = free_port()
        payload = b"abcdefgh" * 65536  # 512 KiB
        with server(program, 4, port), connect(port) as c:
            sender = threading.Thread(target=c.sendall, args=(frame(payload),), daemon=True)
            sender.start()
            expect(read_frame(c) == payload.upper(), "512 KiB message came back altered")
            sender.join()

    def oversized(program: Program) -> None:
        port = free_port()
        with server(program, 4, port) as proc:
            with connect(port) as c:
                c.sendall(struct.pack("!I", 0x7FFFFFFF))  # "2 GiB follows": a lie, or an attack
                expect(closed_by_peer(c), "server didn't drop a client announcing a 2 GiB message")
            expect(alive(proc), "server crashed on an oversized length")
            with connect(port) as c:
                c.sendall(frame(b"next"))
                expect(read_frame(c) == b"NEXT", "server stopped serving after an oversized length")

    def truncated(program: Program) -> None:
        port = free_port()
        with server(program, 4, port) as proc:
            with connect(port) as c:
                c.sendall(struct.pack("!I", 100) + b"only ten b")
                c.shutdown(socket.SHUT_WR)  # FIN after 10 of the promised 100 bytes
                expect(closed_by_peer(c), "server never closed a connection that ended mid-message")
            expect(alive(proc), "server crashed on a truncated message")
            with connect(port) as c:
                c.sendall(frame(b"ok"))
                expect(read_frame(c) == b"OK", "server stopped serving after a truncated message")

    return [("one message", one), ("message arriving one byte at a time", dribble),
            ("three messages in one send, including an empty one", coalesced), ("512 KiB message", big),
            ("drops a client claiming a 2 GiB message", oversized),
            ("handles a connection that ends mid-message", truncated)]


# --- step 5: UDP --------------------------------------------------------------------------------

def udp_socket() -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    return s


def recv_datagram(s: socket.socket) -> bytes:
    try:
        return s.recvfrom(65536)[0]
    except TimeoutError:
        raise Failure("no reply datagram within 2 s") from None


def step5_tests() -> list[tuple[str, Callable[[Program], None]]]:
    def basic(program: Program) -> None:
        port = free_port(socket.SOCK_DGRAM)
        with server(program, 5, port, udp=True), udp_socket() as s:
            s.sendto(b"datagram", ("127.0.0.1", port))
            expect(recv_datagram(s) == b"DATAGRAM", "wrong reply")

    def boundaries(program: Program) -> None:
        port = free_port(socket.SOCK_DGRAM)
        with server(program, 5, port, udp=True), udp_socket() as s:
            s.sendto(b"ab", ("127.0.0.1", port))
            s.sendto(b"cd", ("127.0.0.1", port))
            replies = sorted([recv_datagram(s), recv_datagram(s)])  # UDP may reorder; on loopback it won't
            expect(replies == [b"AB", b"CD"], f"got {replies!r}: each datagram should get its own reply")

    def two_clients(program: Program) -> None:
        port = free_port(socket.SOCK_DGRAM)
        with server(program, 5, port, udp=True), udp_socket() as a, udp_socket() as b:
            a.sendto(b"from a", ("127.0.0.1", port))
            b.sendto(b"from b", ("127.0.0.1", port))
            expect(recv_datagram(a) == b"FROM A", "client a got the wrong reply")
            expect(recv_datagram(b) == b"FROM B", "client b got the wrong reply: reply to recvfrom's address")

    def large(program: Program) -> None:
        port = free_port(socket.SOCK_DGRAM)
        payload = b"q" * 8000
        with server(program, 5, port, udp=True), udp_socket() as s:
            s.sendto(payload, ("127.0.0.1", port))
            reply = recv_datagram(s)
            expect(reply == payload.upper(), f"8000-byte datagram came back as {len(reply)} bytes: buffer too small?")

    def empty(program: Program) -> None:
        port = free_port(socket.SOCK_DGRAM)
        with server(program, 5, port, udp=True) as proc, udp_socket() as s:
            s.sendto(b"", ("127.0.0.1", port))
            reply = recv_datagram(s)
            expect(reply == b"", f"expected an empty reply, got {reply!r}")
            expect(alive(proc), "server exited on an empty datagram: in UDP, 0 bytes is not end-of-file")

    return [("uppercases a datagram", basic), ("two datagrams → two replies", boundaries),
            ("replies go back to the right client", two_clients), ("8000-byte datagram", large),
            ("empty datagram gets an empty reply", empty)]


# --- step 6: chat ------------------------------------------------------------------------------

def step6_tests() -> list[tuple[str, Callable[[Program], None]]]:
    def joined(port: int, n: int) -> list[socket.socket]:
        clients = [connect(port) for _ in range(n)]
        time.sleep(0.5)  # give the server time to accept everyone before anyone talks
        return clients

    def broadcast(program: Program) -> None:
        port = free_port()
        with server(program, 6, port):
            a, b, c = joined(port, 3)
            with a, b, c:
                a.sendall(b"hel")
                time.sleep(0.1)
                a.sendall(b"lo\n")  # one line, two sends
                expect(recv_all(b, 6) == b"hello\n", "b didn't get a's line")
                expect(recv_all(c, 6) == b"hello\n", "c didn't get a's line")
                expect(nothing_arrives(a), "a got its own line back")

    def several_lines(program: Program) -> None:
        port = free_port()
        with server(program, 6, port):
            a, b = joined(port, 2)
            with a, b:
                b.sendall(b"one\ntwo\nthr")  # two complete lines and the start of a third
                expect(recv_all(a, 8) == b"one\ntwo\n", "lines sent in one send() weren't all delivered")
                expect(nothing_arrives(a), "an incomplete line was delivered before its newline")
                b.sendall(b"ee\n")
                expect(recv_all(a, 6) == b"three\n", "the line completed by a later send was wrong")

    def disconnect(program: Program) -> None:
        port = free_port()
        with server(program, 6, port) as proc:
            a, b, c = joined(port, 3)
            with a, b:
                c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                c.close()  # abrupt reset
                time.sleep(0.2)
                a.sendall(b"anyone?\n")
                expect(recv_all(b, 8) == b"anyone?\n", "b stopped getting messages after c reset")
                expect(alive(proc), "server died when a client disconnected")

    def many(program: Program) -> None:
        port = free_port()
        with server(program, 6, port):
            clients = joined(port, 50)
            try:
                for i, s in enumerate(clients):
                    s.sendall(b"msg %02d\n" % i)
                for i, s in enumerate(clients):
                    got = recv_all(s, 49 * 7, timeout=10)
                    lines = sorted(got.splitlines())
                    want = sorted(b"msg %02d" % j for j in range(50) if j != i)
                    expect(lines == want, f"client {i} didn't get exactly one line from each of the other 49 "
                                          f"(own line echoed back? lines lost?)")
            finally:
                for s in clients:
                    s.close()

    return [("a line reaches everyone else, not the sender", broadcast),
            ("several lines per send, and partial lines", several_lines),
            ("keeps going when a client resets", disconnect), ("50 clients, 49 lines each", many)]


TESTS: dict[int, Callable[[], list[tuple[str, Callable[[Program], None]]]]] = {
    1: lambda: client_tests(1), 2: lambda: client_tests(2), 3: step3_tests, 4: step4_tests, 5: step5_tests,
    6: step6_tests,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("step", help="1-6 or 'all'")
    parser.add_argument("--solution", action="store_true", help="test the reference solution instead of yours")
    parser.add_argument("--build-dir", type=Path, default=HERE.parent / "build" / "socket-basics")
    args = parser.parse_args()
    steps = list(TESTS) if args.step == "all" else [int(args.step)]
    program = Program(args.build_dir, args.solution)

    failed = 0
    for step in steps:
        print(f"step {step}: {STEP_NAMES[step]}{' (solution)' if args.solution else ''}")
        for name, test in TESTS[step]():
            try:
                test(program)
                print(f"  PASS  {name}")
            except Failure as e:
                failed += 1
                print(f"  FAIL  {name}: {e}")
            except OSError as e:
                failed += 1
                print(f"  FAIL  {name}: {type(e).__name__}: {e}")
    print("all passed" if failed == 0 else f"{failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    sys.exit(main())
