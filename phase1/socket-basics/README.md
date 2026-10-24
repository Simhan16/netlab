# socket-basics: write your own clients and servers

Step 1.0b, between tcpdump-basics and the netns lab. In tcpdump-basics you *read* packets.
Here you **write the programs that send them**, from a blank function up, using the same
sockets API every networked program on Linux and macOS is built on: browsers, nginx,
Postgres, and the echo-server and dnsq later in this repo.

Six steps, each a small program you finish yourself:

| Step | You write | The idea it teaches |
|---|---|---|
| 1 | an HTTP client, in Python | the call sequence, without C++ in the way |
| 2 | the same client, in C++ on the raw API | `getaddrinfo`, `connect`, errors, reading until EOF |
| 3 | a TCP server, one client at a time | `bind`/`listen`/`accept`, `SO_REUSEADDR`, surviving bad clients |
| 4 | a server for length-prefixed messages | TCP is a byte stream: **you** draw the message boundaries |
| 5 | a UDP server | no connections; datagrams keep their boundaries |
| 6 | a chat server for many clients | `poll()`: serving everyone from one thread |

## How to work
```
socket-basics/
├── exercises/   ← you edit these. Each one compiles as-is and tells you it's "not written yet".
├── solutions/   ← reference versions. Look only after your version passes, or after a real attempt.
└── check.py     ← tests for each step. Each test is built to catch one classic mistake.
```
```bash
cd phase1 && cmake -B build && cmake --build build      # builds your exercises and the solutions
python3 socket-basics/check.py 1                         # step 1 (Python, nothing to build)
python3 socket-basics/check.py 3                         # rebuild first, after editing a .cpp
python3 socket-basics/check.py 3 --solution              # what passing looks like
```
Everything runs on the Mac and on the EC2 box. Every test uses 127.0.0.1, so it needs no
root and no network. A failing test names the mistake it's built to catch, like this:
```
  FAIL  restarts immediately on the same port (SO_REUSEADDR): restart on the same port failed
        (server exited with code 2 at startup: bind: Address already in use): set SO_REUSEADDR before bind()
```
Keep `man 2 socket` (and `man 2 connect`, `man 2 recv`, ...) open: the man pages are the documentation.

---

## The model you need first
### A socket is a file descriptor with two kernel buffers behind it
`socket()` returns a small integer, the same kind of handle `open()` returns for files.
Behind it, the kernel keeps a **send buffer** and a **receive buffer**, and runs TCP for you:
handshakes, retransmissions, acknowledgements, flow control.
- `send()` doesn't put anything on the wire itself. It **copies your bytes into the send buffer** and
  returns. The kernel transmits them when it can.
- `recv()` **copies whatever is in the receive buffer** out to you. If it's empty, `recv()` waits
  (it *blocks*) until something arrives.

Almost every surprise in socket programming comes from forgetting that these buffers sit
between your program and the network.

### The calls, and what each puts on the wire
```
        CLIENT                                  SERVER
                                                socket()
                                                bind(0.0.0.0:9000)   claim the port
                                                listen()             start accepting handshakes
        socket()
        connect(server:9000) ── SYN ─────────▶
                             ◀──────── SYN-ACK ─  (the kernel answers; your program isn't involved)
                             ── ACK ─────────▶    connection waits in the accept queue
                                                accept()             returns a NEW socket for this client
        send("hi") ─────────── data ─────────▶  recv() → "hi"
        recv() ◀─────────────── data ─────────  send("HI")
        close() ────────────── FIN ──────────▶  recv() → 0           "end of file": the client is done
                ◀────────────── FIN ──────────  close()
```
Things worth noticing:
- **Only `connect()` and `close()` trigger packets by themselves.** `listen()` and `accept()`
  send nothing. The kernel completes handshakes on its own, and `accept()` just takes a
  finished one off the queue.
- **The server has two kinds of socket.** The *listening* socket only accepts. Each `accept()`
  returns a *new* socket for one client. Data never flows on the listening socket.
- **`recv()` returning 0 means "the other side closed"**, on TCP. It's how you know the
  conversation is over. (UDP is different: step 5.)

You can watch every one of these with what you learned in tcpdump-basics:
`sudo tcpdump -ni lo0 'tcp port 9000'` on the Mac, or `-i lo` on Linux.

### Addresses in C: structs and byte order
The C API represents an address as a struct: `sockaddr_in` for IPv4, `sockaddr_in6` for IPv6.
Functions take a pointer to the generic `sockaddr`, which is why you see `reinterpret_cast<sockaddr*>`.
Numbers on the wire are **big-endian** ("network byte order"), while x86 and Apple Silicon CPUs are
little-endian. So every port and address goes through a conversion:
`htons()` (host to network, short, for ports), `htonl()` (long, for IPv4 addresses and 4-byte
lengths), and `ntohs()`/`ntohl()` back. Forget it, and port 9000 (`0x2328`) goes out as port 10275 (`0x2823`).

`getaddrinfo()` turns a name and service ("example.com", "80") into a **list** of ready-made
addresses, IPv4 and IPv6, so a client never fills these structs by hand.

### Errors you'll meet
Every call returns -1 on failure and sets `errno`. `std::strerror(errno)` or `perror()`
turns it into text.

| errno | Means | Typical cause |
|---|---|---|
| `ECONNREFUSED` | the server's machine answered the SYN with a RST | it's up, but nothing listens on that port |
| `ETIMEDOUT` | no answer at all after several SYN retries | a firewall silently dropping, or the host is down |
| `ECONNRESET` | the peer sent a RST mid-connection | it crashed, or closed while unread data was waiting |
| `EPIPE` | you wrote after the connection was already gone | see SIGPIPE in step 3 |
| `EADDRINUSE` | `bind()` refused the port | another process has it, or old connections in TIME_WAIT (step 3) |
| `EINTR` | a signal interrupted the call | not an error: just call it again |

---

## Step 1: an HTTP client in Python
**Concept:** the client side is four moves: **resolve** the name, **connect**, **send** the
request, **receive** until the server closes. HTTP/1.0 suits this perfectly, because the server
closes the connection when the response is complete. "Read until EOF" is the whole parsing logic.

**Do it:** finish `exercises/step1_client.py`, following its TODOs.
```bash
python3 socket-basics/check.py 1
python3 socket-basics/exercises/step1_client.py example.com 80      # a real server
```
**What the tests catch:**
- **Stopping at the first `recv()`.** The test server sends its response in five uneven pieces with
  pauses in between, so a client that reads once gets 5 bytes (`HTTP/`). One `recv()` returns whatever has arrived
  so far: never "the response".
- **Messy errors.** A refused connection and an unresolvable name must exit with code 2 and a message,
  not a traceback.

**Check yourself:** you asked for HTTP/1.0, but example.com replies `HTTP/1.1 200 OK`. Why
does your client still terminate?
<details><summary>Answer</summary>A server can reply with a higher minor version, but it must still
speak the client's version: for a 1.0 request with no keep-alive, it closes the connection after the
response. If you sent HTTP/1.1 instead, the server would keep the connection open for more requests,
and your "read until EOF" loop would hang until the server's idle timeout. Real HTTP/1.1 clients read
the <code>Content-Length</code> header to know where the response ends: framing, the subject of step 4.</details>

## Step 2: the same client on the raw API
**Concept:** what `create_connection()` and `sendall()` did for you in Python:
- `getaddrinfo()` returns a *list* of addresses. Connect to them one by one until one works: a
  name like `localhost` often resolves to `::1` (IPv6) *and* `127.0.0.1`, and a server may listen on only one.
- `connect()` blocks for the handshake and reports the outcome as an errno: success, `ECONNREFUSED`
  (RST came back) or `ETIMEDOUT` (nothing came back).
- `send()` returns how many bytes it accepted, which can be fewer than you asked. Loop until done.

**Do it:** finish `exercises/step2_client.cpp`, rebuild, check.
```bash
cmake --build build && python3 socket-basics/check.py 2
./build/socket-basics/step2_client example.com 80
./build/socket-basics/step2_client 127.0.0.1 1          # refused: what does strerror say?
./build/socket-basics/step2_client 10.255.255.1 80       # nobody answers: how long does it take to give up?
```
**What the tests catch:** connecting only to the first address (the test server listens on
127.0.0.1 only and the client is given `localhost`), plus the step 1 tests.

**Check yourself:** the last command usually hangs for a long time before failing. (On some networks it fails at once with "No route to host" instead.) What's happening on
the wire, and what's the difference from the "refused" case?
<details><summary>Answer</summary>Nothing answers the SYN, so the kernel retransmits it with
exponential backoff (1 s, 2 s, 4 s, ...) and only gives up after all its retries:
<code>ETIMEDOUT</code>, about 75 s on macOS and about 2 minutes on Linux (<code>net.ipv4.tcp_syn_retries</code>).
"Refused" fails at once because a RST is an answer. In tcpdump: a single SYN and RST versus a string of
SYNs with no reply, which is exactly casebook cases 06 (firewall drop) and 07 (connection refused).</details>

## Step 3: a TCP server, one client at a time
**Concept:** a server **claims a port** (`bind`), **asks the kernel to accept handshakes**
(`listen`), then **takes connections one at a time** (`accept`). Three details separate a toy
from a server you can run:
1. **`SO_REUSEADDR`.** When a TCP connection closes, the side that closed first keeps a TIME_WAIT
   entry for about 60 s, to absorb stray late packets. When you restart a server, its old connections
   are in TIME_WAIT on its port, and `bind()` fails with `EADDRINUSE`. `SO_REUSEADDR` tells the
   kernel that's fine. Every real server sets it.
2. **A bad client ends that client, never the server.** `recv()` and `send()` fail with `ECONNRESET`
   when a client vanishes. Handle it by closing that client and going back to `accept()`.
3. **Ignore SIGPIPE.** Writing to a connection *after* the kernel has already reported it broken
   raises the SIGPIPE signal, which **kills the process by default**. Your loop stops at the first
   error, so it shouldn't get there. But one code path that ignores an error and keeps writing is
   enough, so servers set `signal(SIGPIPE, SIG_IGN)` and get `EPIPE` back instead. (The checker can't
   force this from outside, so it's on you.)

**Do it:** finish `exercises/step3_server.cpp`. Try it with `nc` (netcat) before the checker:
```bash
./build/socket-basics/step3_server 9000                 # terminal 1
nc 127.0.0.1 9000                                        # terminal 2: type lines, Ctrl-C to quit
nc 127.0.0.1 9000                                        # terminal 3, while 2 is still connected
python3 socket-basics/check.py 3
```
**What you should see:** terminal 3 connects at once, but gets no replies until terminal 2 quits.
Its connection is complete in the kernel, waiting in the accept queue while your one thread is busy.

**What the tests catch:** a missing `SO_REUSEADDR` (the test kills your server and restarts it
on the same port at once). A server that exits when a client resets mid-transfer. A server that
doesn't move on to the next client.

**Check yourself:** run the restart test without `SO_REUSEADDR`, then `netstat -an | grep TIME_WAIT`
(Mac) or `ss -tan state time-wait` (Linux). Whose connection is in TIME_WAIT, and why is the
server, not the client, the one stuck with it?
<details><summary>Answer</summary>TIME_WAIT belongs to whichever side closed <i>first</i>. The test
kills the server while the client is connected, so the server's kernel sends the first FIN and keeps
the TIME_WAIT entry on the server's port. Clients rarely notice TIME_WAIT, because each connection
gets a fresh ephemeral port.</details>

**Check yourself:** your `send_all()` loop handles short writes, but no test proves it. Why
can't a test make `send()` return fewer bytes than you asked for here?
<details><summary>Answer</summary>On a <b>blocking</b> socket, <code>send()</code> waits until all
of your bytes fit in the send buffer, so it only returns early if a signal interrupts it. Short
writes are routine on <b>non-blocking</b> sockets (echo-server's epoll version), which return as soon
as the buffer is full. The loop costs nothing, so write it everywhere.</details>

## Step 4: messages over a byte stream
**Concept:** **TCP delivers bytes, not messages.** Your three `send()` calls may arrive as one
`recv()`, and one `send()` may arrive as fifty. The kernel merges and splits freely (Nagle's
algorithm, segment sizes, buffer timing), and the only guarantee is order. So every protocol
on TCP defines its own **framing**. HTTP uses `\r\n` and `Content-Length`, Redis and
chat protocols use newlines, and many binary protocols (Postgres, Kafka, gRPC) use a
**length prefix**: you implement one here.

```
┌─────────────── one message ───────────────┐
│ length N: 4 bytes, big-endian │ N bytes   │
└───────────────────────────────────────────┘
```
The key function is `recv_exact(fd, buf, n)`: loop on `recv()` until exactly `n` bytes are in.
Read 4 bytes, convert with `ntohl`, then read exactly N. And **never trust a length from the
network**: four bytes from a client can claim 4 GiB, and a server that just allocates whatever it's
told is easy to knock over.

**Do it:** finish `exercises/step4_framed_server.cpp`, starting from your step 3 code.
```bash
python3 socket-basics/check.py 4
```
**What the tests catch:**
- A message sent **one byte per packet** (TCP_NODELAY, with 5 ms gaps): the single-`recv()` server gets 1 byte.
- **Three messages in one send**, one of them empty: a server reading "one message per recv" loses two.
- A 512 KiB message: it never fits in one `recv()`.
- A length of 2 GiB: you must drop that client, not allocate or wait forever.
- A connection that dies **mid-message**, which is different from a clean close between messages.

**Check yourself:** on a quiet laptop, a naive "one recv = one message" server often *passes*
manual testing. Why do these bugs show up only in production?
<details><summary>Answer</summary>Over loopback with small messages, each send usually does arrive as
one recv, by luck of timing. Over real networks you get segment boundaries (MSS ≈ 1448 bytes), packet
loss and retransmission, slow clients and busy servers, all of which split and merge data differently.
That's why the tests force the split and merge cases rather than hoping to hit them.</details>

## Step 5: UDP
**Concept:** UDP is the opposite trade. **No connection**: no handshake, no `listen`, no
`accept`. One socket talks to everyone, and `recvfrom()` tells you who each datagram came from,
so you reply with `sendto()` to that address. **Boundaries are kept**: one `sendto()` is exactly
one `recvfrom()`, so there's no framing to do. In exchange, **nothing is guaranteed**: datagrams can be lost,
duplicated or reordered, and your application handles it (udp-ping measures exactly this).

Two traps:
- **The buffer decides the size.** If a datagram is bigger than your buffer, the excess is
  silently thrown away. Size it for the largest possible payload (65,507 bytes over IPv4).
- **0 bytes is a real datagram.** With no connection there's no "end of file". `recvfrom()`
  returning 0 just means someone sent an empty datagram, so reply with an empty one.

**Do it:** finish `exercises/step5_udp_server.cpp`.
```bash
./build/socket-basics/step5_udp_server 9001             # terminal 1
nc -u 127.0.0.1 9001                                     # terminal 2: each line is one datagram
python3 socket-basics/check.py 5
```
**What the tests catch:** a 1 KiB buffer (an 8000-byte datagram comes back cut short), replying to
the wrong client, merging two datagrams, and exiting or going silent on an empty datagram.

**Check yourself:** DNS uses UDP for its queries. Given what UDP doesn't guarantee, what does
a DNS client have to do itself?
<details><summary>Answer</summary>Retransmit after a timeout (the query or reply may be lost), match
each reply to its query by ID (replies may be duplicated, reordered or forged), and fall back to TCP
when a reply doesn't fit in a datagram (the TC bit). dnsq does all three: dns-resolver exercises 2 and 4.</details>

## Step 6: many clients at once with poll()
**Concept:** step 3 serves one client at a time because `recv()` blocks: while it waits on
client A, client B is ignored. `poll()` turns that around. You hand it every socket and it sleeps
until **at least one is ready**, then tells you which. A `recv()` on a ready socket returns
immediately, so one thread can serve hundreds of clients: an **event loop**. nginx, Redis
and Node.js all work this way (with `epoll` or `kqueue`, faster versions of the same idea:
echo-server step 1.2).

The rules of an event loop:
- **Only call `recv()` on sockets `poll()` marked ready**, and only once per round.
- **Keep per-client state.** A client's line may arrive in pieces across rounds, so each client has its
  own buffer of not-yet-complete bytes.
- **Never block.** If one client stalls you, everyone stalls. (This solution still uses
  a blocking `send()` to each client, which a client that never reads can jam. echo-server's epoll version
  fixes that with per-client output queues and non-blocking writes. Read it after this.)
- **Don't remove clients while you iterate over them.** Mark them, then sweep.

**Do it:** finish `exercises/step6_chat_server.cpp`, then chat with yourself:
```bash
./build/socket-basics/step6_chat_server 9002            # terminal 1
nc 127.0.0.1 9002                                        # terminals 2, 3, 4: type in one, read in the others
python3 socket-basics/check.py 6
```
**What the tests catch:** echoing a line back to its sender, forwarding half a line before
its newline, losing lines when several arrive in one `recv()`, dying when a client resets, and
50 clients talking at once (each must receive exactly the other 49 lines).

**Check yourself:** `poll()` rebuilds and scans the whole list of sockets on every call. With
10,000 mostly idle clients, why does that become the bottleneck, and what does `epoll` do differently?
<details><summary>Answer</summary>Each <code>poll()</code> copies all 10,000 entries into the kernel,
which checks every one, and you scan all 10,000 results, even when only one client spoke: O(n) work per
event. <code>epoll</code> keeps the set registered inside the kernel and returns only the sockets that
are ready, so the work tracks activity, not the number of connections. That's the C10K problem, in echo-server exercise 4.</details>

---

## Where these ideas come back
- **echo-server (1.2):** step 3, then step 6 done with epoll, non-blocking writes and backpressure.
- **dns-resolver (1.3):** step 5's client side: UDP with timeouts, then TCP with step 4's length-prefix framing (DNS over TCP uses a 2-byte length).
- **udp-ping (1.3):** step 5, measuring the loss and reordering UDP allows.
- **mini-tcpdump (1.4):** the other side of every step: watching these programs' packets.

## Known limitations
- The servers are IPv4-only (`AF_INET`) to keep the structs simple. Making step 3 dual-stack
  (`AF_INET6` with `IPV6_V6ONLY` off) is a good extension. `common/src/socket.cpp` shows how.
- Step 6 uses blocking sends, so one client that never reads can stall the chat server (see above).
- `check.py` runs on 127.0.0.1, where nothing is lost and little is reordered. It forces the
  split/merge/reset cases deliberately, but it can't create real packet loss. Run your servers in the netns lab with `netem` for that.
- No timeouts: a client that connects and goes silent holds a step 3 server forever.
  `setsockopt(SO_RCVTIMEO)` or `poll()` with a timeout fixes it.
