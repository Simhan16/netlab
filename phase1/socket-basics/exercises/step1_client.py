#!/usr/bin/env python3
"""Step 1: an HTTP/1.0 client with Python's socket module.

    python3 step1_client.py HOST PORT [PATH]

Connect to HOST:PORT over TCP, send

    GET PATH HTTP/1.0\\r\\nHost: HOST\\r\\n\\r\\n

and print every byte the server sends back, until it closes the connection.
Exit codes: 0 ok, 1 usage, 2 resolve/connect/IO error (with a message on stderr).

Check it:  python3 check.py 1      Try it:  python3 exercises/step1_client.py example.com 80
"""

import socket  # noqa: F401 (you'll need it)
import sys


def fetch(host: str, port: int, path: str) -> bytes:
    # TODO 1: open a TCP connection. socket.create_connection((host, port), timeout=10)
    #         resolves the name and connects. Use it in a `with` block so it gets closed.
    # TODO 2: build the request (bytes, not str: .encode()) and send ALL of it.
    #         sock.send() may send only part; sock.sendall() loops for you.
    # TODO 3: loop on sock.recv(4096) and collect the chunks. Each call returns whatever
    #         has arrived so far. b"" means the server closed its side: stop then.
    # TODO 4: return all the chunks joined together.
    raise NotImplementedError("step 1 is not written yet")


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(f"usage: {sys.argv[0]} HOST PORT [PATH]", file=sys.stderr)
        return 1
    host, port = sys.argv[1], int(sys.argv[2])
    path = sys.argv[3] if len(sys.argv) == 4 else "/"
    # TODO 5: name lookup failures, refused connections and timeouts all raise OSError
    #         (socket.gaierror, ConnectionRefusedError, TimeoutError). Catch it, print a
    #         message to stderr and return 2.
    response = fetch(host, port, path)
    sys.stdout.buffer.write(response)  # raw bytes: the body may not be valid text
    return 0


if __name__ == "__main__":
    sys.exit(main())
