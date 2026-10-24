#!/usr/bin/env python3
"""Step 1 solution: an HTTP/1.0 client with Python's socket module.

    python3 step1_client.py HOST PORT [PATH]

Exit codes: 0 ok, 1 usage, 2 resolve/connect/IO error.
"""

import socket
import sys


def fetch(host: str, port: int, path: str) -> bytes:
    # create_connection() runs getaddrinfo and tries each address in turn, which
    # is exactly the loop you write by hand in step 2.
    with socket.create_connection((host, port), timeout=10) as sock:
        request = f"GET {path} HTTP/1.0\r\nHost: {host}\r\n\r\n".encode()
        sock.sendall(request)  # send() may write only part; sendall() loops for you

        chunks: list[bytes] = []
        while True:
            chunk = sock.recv(4096)  # returns whatever has arrived: 1 byte up to 4096
            if not chunk:  # b"" means the server closed its side (FIN): we're done
                break
            chunks.append(chunk)
        return b"".join(chunks)


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(f"usage: {sys.argv[0]} HOST PORT [PATH]", file=sys.stderr)
        return 1
    host, port = sys.argv[1], int(sys.argv[2])
    path = sys.argv[3] if len(sys.argv) == 4 else "/"
    try:
        response = fetch(host, port, path)
    except OSError as e:  # socket.gaierror, ConnectionRefusedError and timeouts are all OSErrors
        print(f"{host}:{port}: {e}", file=sys.stderr)
        return 2
    sys.stdout.buffer.write(response)  # raw bytes: the body may not be valid text
    return 0


if __name__ == "__main__":
    sys.exit(main())
