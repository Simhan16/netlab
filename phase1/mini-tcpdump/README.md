# mini-tcpdump

A packet capture and analysis tool on libpcap. It captures live or reads pcap files,
applies BPF filters, decodes every layer by hand, and tracks per-flow TCP health:
retransmissions, dup ACKs, zero windows, handshake and data RTT, and how each connection
ended. It also tracks DNS transactions, ICMP errors, ARP and the plaintext parts of TLS.
The flagship C++ project of Phase 1.4.

The analysis core (`src/core`, built as `pktcore`) has no capture code in it except
`pcap_source.cpp`. PacketWhisperer links the same library through pybind11.

```
src/core/packet.cpp     Ethernet/VLAN, Linux SLL/SLL2, loopback, raw → IPv4/IPv6 (+ext headers) → TCP/UDP/ICMP/ARP
src/core/tls.cpp        ClientHello SNI, negotiated version, plaintext alerts
src/core/analyzer.cpp   flow tables, TCP state + health, DNS matching, ICMP errors, ARP; JSON + text reports
src/core/pcap_source.cpp  read a pcap file → analysis (used by the Python binding)
src/tool/main.cpp       the CLI: live capture, -r/-w, BPF, per-packet lines, summary
../dns-resolver/src/dns.cpp  the DNS codec from 1.3, reused here
```

## Build & run
```bash
sudo apt install libpcap-dev cmake g++     # Ubuntu
cd phase1 && cmake -B build && cmake --build build -j && (cd build && ctest)

sudo ./build/mini-tcpdump/mini-tcpdump -i ens5 -c 200 'tcp port 443'
sudo ./build/mini-tcpdump/mini-tcpdump -i any -w out.pcap --json out.json
./build/mini-tcpdump/mini-tcpdump -r out.pcap -q            # summary only, plus packets/sec
sudo ip netns exec router ./build/mini-tcpdump/mini-tcpdump -i r-b   # inside the netns lab
```
To avoid sudo for live capture: `sudo setcap cap_net_raw,cap_net_admin=eip build/mini-tcpdump/mini-tcpdump`.

## How the TCP signals are computed
| Signal | Rule | What it usually means |
|---|---|---|
| retransmission | data segment starting before the highest seq already seen in that direction | loss upstream of the capture point (or reordering, which can't be told apart) |
| SYN retries (`syns > 1`) | repeated SYN with the same ISN | SYN or SYN-ACK lost: firewall drop, routing |
| dup ACK | pure ACK repeating the last ack and window while data is outstanding | receiver saw a gap; 3 in a row trigger fast retransmit |
| zero window | non-SYN, non-RST segment advertising window 0 | receiving app isn't reading fast enough |
| `rtt_ms.client_data` / `.server_data` | time from a data segment to the ACK covering it, split by whose data it was; samples after a retransmit are dropped (Karn's rule) | from a client-side capture, `client_data` is the network RTT and `server_data` is just the client's ACK delay (and vice versa at the server) |
| `handshake_ms` | SYN→SYN-ACK and SYN-ACK→ACK | which side of the capture point the latency is on |
| `state` | `syn_unanswered`, `refused`, `synack_unacked`, `open`, `half_closed`, `closed`, `reset`, `midstream*` | how the connection went |

## Exercises (in the netns lab)
Lab up (`sudo ../netns-router/setup.sh`), and an HTTP server on the host playing "the internet":
```bash
head -c 2000000 /dev/urandom > /tmp/big.bin
python3 -m http.server 8000 --bind 192.168.255.1 --directory /tmp &
MTD=$PWD/build/mini-tcpdump/mini-tcpdump
```

### 1. BPF filters run in the kernel
**Concept:** a capture filter like `tcp port 443` is compiled to **BPF** bytecode and runs
*in the kernel*. Packets that don't match are never copied to your program. The kernel
also counts packets it had to **drop** because your program fell behind. Those are gaps in
your capture, not network loss.

**Try it:**
```bash
sudo $MTD -i ens5 -c 10 'tcp[tcpflags] & tcp-syn != 0'       # only SYN and SYN-ACK packets
curl -so /dev/null https://example.com                        # another terminal: make some connections
sudo $MTD -i ens5 -q -c 20000                                 # everything; watch the last stderr line
```
**What you should see:** the first command prints only `Flags [S]` and `[S.]` lines, from your
curl and from anything else on the box that opens connections. The second ends with `N packets received by filter,
M dropped by kernel`.

**Why:** the filter reads raw header bytes (`tcp[13]` is the flags byte) before anything
reaches user space, which is how tcpdump keeps up with fast links.

**Check yourself:** your summary shows retransmissions, but the kernel reported drops. Can
you trust the retransmission count?
<details><summary>Answer</summary>Not fully. Missed packets create apparent sequence gaps, so a
later normal segment can look out of order, and you may miss real retransmissions. Fix the
capture first (filter tighter, bigger buffer, write with <code>-w</code> and analyze offline).</details>

### 2. Loss: retransmissions vs duplicate ACKs
**Concept:** when a segment is lost, the receiver keeps acknowledging the last byte it got
in order. These are **duplicate ACKs**. Three of them trigger **fast retransmit**: the
sender re-sends without waiting for its timeout (RTO).

**Try it:**
```bash
sudo ip netns exec router tc qdisc add dev r-a root netem loss 3%
sudo ip netns exec lan-a $MTD -i a-eth0 -q -w /tmp/loss.pcap --json /tmp/loss.json &
sudo ip netns exec lan-a curl -so /dev/null http://192.168.255.1:8000/big.bin
sudo pkill -INT -f "mini-tcpdump -i a-eth0"
sudo ip netns exec router tc qdisc del dev r-a root
$MTD -r /tmp/loss.pcap -q
```
**What you should see:** in the flow line, the server→client `retrans` count is in the dozens,
and `loss.json` shows non-zero client `dup_acks`. (A dup ACK only counts when the advertised
window is also unchanged, the same rule Wireshark uses, so the count depends on how the
client's receive buffer was growing.) The state is still `closed`: the download
finished, just slower.

**Why:** netem on `r-a` drops the server's data on its way to lan-a. The client sees the gap
and sends dup ACKs; the retransmitted copy arrives later with an old sequence number, which is
what the analyzer counts.

**Check yourself:** the capture is on the client, but the retransmissions are the server's.
How can the client's capture show *server* retransmissions at all?
<details><summary>Answer</summary>The client receives the re-sent copy. Its sequence number is
below the highest one already seen from the server, because later segments arrived first. So
"retransmission" here means "old data arriving again", whoever sent it.</details>

### 3. Where is the latency? Vantage point matters
**Concept:** a capture point splits the path in two. The handshake shows each half: **SYN →
SYN-ACK** is the round trip to the server's side of the capture point, and **SYN-ACK → ACK**
is the round trip to the client's side.

**Try it:** add 100 ms on the router's link to lan-a and capture at two points at once.
```bash
sudo ip netns exec router tc qdisc add dev r-a root netem delay 100ms
sudo ip netns exec lan-a  $MTD -i a-eth0 -q --json /tmp/at-client.json 'tcp port 8000' &
sudo ip netns exec router $MTD -i r-up  -q --json /tmp/at-router.json 'tcp port 8000' &
sudo ip netns exec lan-a curl -so /dev/null http://192.168.255.1:8000/
sleep 1; sudo pkill -INT -f mini-tcpdump
sudo ip netns exec router tc qdisc del dev r-a root
python3 -c "import json; [print(f, json.load(open(f))['tcp']['flows'][0]['handshake_ms']) for f in ('/tmp/at-client.json', '/tmp/at-router.json')]"
```
**What you should see:** at the client, `syn_to_synack` ≈ 100 ms and `synack_to_ack` ≈ 0. At
the router uplink it's the other way round: `syn_to_synack` ≈ 0 and `synack_to_ack` ≈ 100 ms.

**Why:** the delay sits between the router and lan-a. From the client, the wait is for the
SYN-ACK to arrive. From the router, the SYN-ACK passes immediately, and the wait is for the
client's ACK to come back through the slow link.

**Check yourself:** the help desk captures on the server and sees `synack_to_ack` = 300 ms.
Whose network should they look at?
<details><summary>Answer</summary>The client's side: the server answered instantly, and the
delay is between the server and the client. This is why the casebook records capture points.</details>

### 4. Zero windows and TLS, from captures you already know
**Concept:** the analyzer turns raw signals into named facts. Check that it agrees with what
you caused.

**Try it:**
- Repeat the echo-server backpressure exercise (`../echo-server/README.md`, exercise 3) while
  running `sudo $MTD -i lo -q 'tcp port 9000'`. Expect `zero_windows` > 0 in **both** directions.
- In lan-a, capture `tcp port 443` while running `curl -so /dev/null https://example.com`, then
  `curl --tls-max 1.2 ...`. Expect the SNI `example.com` both times, with version `TLS1.3` and then `TLS1.2`.

**Why:** the SNI (the hostname) travels in the *unencrypted* ClientHello, because the server
needs it to pick a certificate before any keys exist. Everything after the handshake is
ciphertext.

**Check yourself:** what would make the SNI invisible to mini-tcpdump?
<details><summary>Answer</summary>Encrypted Client Hello (ECH), where the real ClientHello is
encrypted with a key the server publishes in DNS. A middlebox then sees only an outer,
generic name.</details>

## Sample output
<!-- Paste a real capture summary, and the packets/sec line from a large `-r` run, here. -->
```
[paste mini-tcpdump output]
```

## Performance
Measure it with `-q -r big.pcap`; it prints packets/sec at the end. Write it down with the
instance type and the pcap's traffic mix before putting it on your resume. To get a big pcap,
capture a few minutes of `iperf3` traffic, or replay one with `tcpreplay`.

## Known limitations
- No TCP stream reassembly. TLS records and DNS-over-TCP split across segments are skipped. In practice SNI still works, because ClientHellos fit in one segment.
- Retransmission and reordering look the same from one vantage point, and keepalive probes count as retransmissions.
- SACK blocks are detected but not used to refine loss estimates.
- TLS 1.3 encrypts nearly everything after the ServerHello, including most alerts. Those show up as `encrypted_alert` or nothing.
- Each flow keeps up to 1024 unacked segments for RTT sampling. A huge flight or a capture that misses ACKs reduces the number of samples.
- IPv4 fragments are counted but not reassembled.
