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

## Experiments
1. **Feel the blocking problem.** Run `echo_server_blocking`, connect two clients, type in the second one. Nothing comes back until the first one quits. Yet `ss -tn` shows the second connection ESTABLISHED, because the kernel finished its handshake into the listen backlog.
2. **Annotate the handshake and teardown.** In one terminal run:
   ```bash
   sudo tcpdump -ni lo -S 'tcp port 9000'
   ```
   Then connect, send one line and quit. Label each packet in a copy of the output:
   - `[S]`: the client's SYN, with its ISN and options (MSS, wscale, SACK, timestamps).
   - `[S.]`: the server's SYN-ACK. Its ack is the client ISN + 1.
   - `[.]`: the final ACK. The connection is now ESTABLISHED on both sides.
   - `[P.]`: data. The ack number advances by the bytes received.
   - `[F.]` / `[.]`: each side's FIN and its ACK. The side that closes first ends up in TIME_WAIT (`ss -tan state time-wait`).
3. **Watch backpressure.** Write a client that sends without ever reading, or use `cat /dev/zero | nc 127.0.0.1 9000 > /dev/null & kill -STOP $!` to pause the reader. tcpdump will show the window shrink to `win 0`, then zero-window probes.
4. **Scale.** Raise `ulimit -n` and bench with `--conns 5000`. Watch the epoll server's peak-concurrent count. What fails first on the blocking server?

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
