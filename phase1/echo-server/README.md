# echo-server

A TCP echo server built twice: first the naive blocking version, then a single-threaded
`epoll` event loop that serves thousands of clients. A client is included, with a
benchmark mode. Phase 1.2.

| Binary | What it shows |
|---|---|
| `echo_server_blocking` | `socket → bind → listen → accept → recv/send`. One client at a time. |
| `echo_server_epoll` (Linux) | Non-blocking sockets, `epoll`, partial writes, backpressure. |
| `echo_client` | Interactive echo, or `--bench` for throughput/latency numbers. |

## Build & run
```bash
cd phase1 && cmake -B build && cmake --build build -j
./build/echo-server/echo_server_epoll 9000          # terminal 1
./build/echo-server/echo_client 127.0.0.1 9000      # terminal 2: type lines
./build/echo-server/echo_client 127.0.0.1 9000 --bench 100000 --conns 500
```

## Exercises
Run from `phase1` after building. Use two or three terminals. Exercises 3-4 need Linux
for `echo_server_epoll`.

### 1. Feel the blocking problem
**Concept:** the kernel, not your program, does the TCP handshake. A listening socket has
an **accept queue** (sized by `listen()`'s backlog) where completed connections wait until
the program calls `accept()`. A single-threaded server stuck in `recv()` for one client
never gets back to `accept()`, or to anyone else.

**Try it:**
```bash
./build/echo-server/echo_server_blocking 9000        # terminal 1
./build/echo-server/echo_client 127.0.0.1 9000       # terminal 2: type a line, stay connected
./build/echo-server/echo_client 127.0.0.1 9000       # terminal 3: type a line
ss -tln 'sport = :9000'                              # terminal 4: Recv-Q of the LISTEN socket
```
**What you should see:** terminal 3 connects instantly, but its line isn't echoed until you
quit terminal 2. For a LISTEN socket, `ss` shows the accept-queue length in `Recv-Q`: it's 1
while client 3 waits.

**Why:** client 3's connection is fully ESTABLISHED inside the kernel, sitting in the accept
queue. The server's only thread is blocked in `recv()` on client 2's socket.

**Check yourself:** what happens to new clients once the accept queue is full?
<details><summary>Answer</summary>By default Linux drops the client's handshake-completing ACK
(or the SYN itself), so the client sees a retransmit and a slow connect, not a fast refusal.
<code>net.ipv4.tcp_abort_on_overflow=1</code> switches to sending RST instead.</details>

### 2. Annotate the handshake and teardown
**Concept:** TCP numbers every *byte* it sends. Each side picks a random **initial
sequence number (ISN)**. `ack = N` means "I've received everything before byte N." SYN and
FIN each consume one sequence number, which is why the handshake's acks are ISN + 1.

**Try it:**
```bash
sudo tcpdump -ni lo -S 'tcp port 9000'              # -S: absolute sequence numbers
./build/echo-server/echo_client 127.0.0.1 9000      # type "hi", then Ctrl-D
ss -tan state time-wait '( sport = :9000 or dport = :9000 )'
```
Copy the output and label every line:
- `[S]`: the client's SYN, its ISN, and options: MSS (largest segment it accepts), wscale (window multiplier), SACK-permitted, timestamps.
- `[S.]`: the server's SYN-ACK: its own ISN, and `ack` = client ISN + 1.
- `[.]`: the client's ACK. Both sides are now ESTABLISHED.
- `[P.]` with `length 3`: "hi\n". The server's ACK for it equals that seq + 3.
- `[F.]` / `[.]`: each side's FIN and the ACK of it.

**Why:** counting bytes, not packets, lets TCP split and merge data freely. It also makes
retransmissions and loss detectable: a gap in sequence numbers is missing data.

**Check yourself:** `ss` shows a TIME_WAIT entry. On which side, and why does TCP keep it
around (60 s on Linux)?
<details><summary>Answer</summary>On the side that closed first, here the client. TIME_WAIT
lets it re-send the final ACK if that ACK is lost, and makes sure stray old segments from this
connection die before the same address/port pair can be reused.</details>

### 3. Watch flow control and backpressure
**Concept:** a TCP receiver advertises a **window**: how much free buffer it has. If the
application stops reading, the buffer fills, the window drops to 0, and the sender must
stop. The epoll server adds **backpressure** on top: once it holds 1 MiB that a client
hasn't read back, it stops reading from that client, so the pressure propagates upstream.

**Try it:** run a client that sends forever but never reads its echoes.
```bash
sudo tcpdump -ni lo 'tcp port 9000 and tcp[14:2] = 0'     # terminal 1: only segments advertising window 0
watch -n1 "ss -tnm '( sport = :9000 or dport = :9000 )'"   # terminal 2: Recv-Q / Send-Q per socket
./build/echo-server/echo_server_epoll 9000 &               # terminal 3
python3 -c 'import socket; s = socket.create_connection(("127.0.0.1", 9000)); [s.sendall(b"x" * 65536) for _ in iter(int, 1)]' &
kill %2 %1                                                  # terminal 3, when done: clean up
```
**What you should see:** within a second, the client's socket `Recv-Q` fills (echoes it never
reads) and it advertises `win 0`. Then the server's socket `Recv-Q` fills too, because the
server stopped reading, and the server advertises `win 0`. The Python process then blocks
forever in `sendall`. tcpdump keeps showing periodic **zero-window probes**.

**Why:** every buffer along the path fills up in turn, and each full buffer stops the stage
before it. Nothing is lost and nothing grows without bound. That's flow control working as
designed.

**Check yourself:** what would happen to the server if it didn't have the `kMaxPending` check?
<details><summary>Answer</summary>It would keep reading and buffering echoes for a client that
never reads them, so its memory would grow as fast as the client could send: a trivial
denial-of-service by a single slow reader.</details>

### 4. Scale: what limits concurrent connections?
**Concept:** every connection is a **file descriptor** plus kernel socket buffers. The
per-process fd limit (`ulimit -n`, often 1024) is usually the first wall, long before CPU.
Handling thousands of connections in one thread is the classic **C10K** problem, and
epoll is Linux's answer.

**Try it:**
```bash
./build/echo-server/echo_server_blocking 9000 &
./build/echo-server/echo_client 127.0.0.1 9000 --bench 10 --conns 2    # hangs. Why? Ctrl-C it
kill %1
ulimit -n 20000; ./build/echo-server/echo_server_epoll 9000 &            # same shell for both
ulimit -n 20000; ./build/echo-server/echo_client 127.0.0.1 9000 --bench 100000 --conns 5000
kill -INT %1                                                             # prints peak concurrent
```
**What you should see:** against the blocking server, the bench stalls on its second round
trip. Against epoll it completes, and the server reports `peak 5000 concurrent`. Without the
`ulimit` raise, connect or accept fails with `Too many open files`.

**Why:** the bench alternates connections. The blocking server is still serving connection 1,
so connection 2's message is never read. The epoll server never waits on any single socket.

**Check yourself:** with unlimited fds, what limits how many connections *one client machine*
can open to one server port?
<details><summary>Answer</summary>Ephemeral source ports: a connection is identified by
(src IP, src port, dst IP, dst port), and Linux's default <code>ip_local_port_range</code> gives
about 28,000 source ports per destination. Memory for socket buffers is the next limit.</details>

## Sample output
<!-- Paste a real --bench run from your EC2 instance. Note the instance type: these numbers are hardware-dependent. -->
```
[paste echo_client --bench output]
```

## Known limitations
- Level-triggered epoll. Edge-triggered (`EPOLLET`) would cut wakeups, but every read loop then has to drain to EAGAIN. Try converting it as an exercise.
- Single-threaded. The next step is one epoll loop per core with `SO_REUSEPORT`.
- The bench measures ping-pong latency on one thread, not maximum server throughput. Run several client processes in parallel to load the server.
- No TLS and no idle timeouts: a client that connects and never sends holds an fd forever.
