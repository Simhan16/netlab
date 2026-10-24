# udp-ping

Measures latency, jitter and loss over UDP: `udp_ping` sends sequence-numbered probes, and
`udp_pong` echoes them. Phase 1.3.

Unlike ICMP ping it runs without root, follows the same path and firewall rules as
application UDP (VoIP, games, QUIC), and lets you choose the probe size. That makes it
useful for finding the size where packets start disappearing.

## Build & run
```bash
cd phase1 && cmake -B build && cmake --build build -j
./build/udp-ping/udp_pong 9999                                    # on the far host
./build/udp-ping/udp_ping <host> -p 9999 -c 20 -i 200 -s 512      # 20 probes, 200 ms apart, 512 bytes
```
Options: `-c` count, `-i` interval (ms), `-s` probe size (bytes), `-W` timeout (ms).

## How it works
Each probe carries `magic | seq | send-timestamp`. The server echoes it untouched, so RTT
is `now - echoed timestamp` on the client's own monotonic clock. No clock sync is needed,
and there's no per-probe state. Replies are classified as:
- **lost:** never came back
- **duplicate:** the same seq twice (the network duplicated it)
- **reordered:** arrived after a higher seq
- **late:** arrived after its timeout; counted as received but flagged

Jitter is the RFC 3550 estimator: a running average of |RTT(n) − RTT(n−1)| with gain 1/16.

## Exercises (in the netns lab)
Start the lab (`sudo ../netns-router/setup.sh`) and a server in lan-b:
```bash
sudo ip netns exec lan-b ./build/udp-ping/udp_pong &
```
`tc ... netem` injects delay, jitter and loss. It's a **queueing discipline** on an
interface's *outgoing* queue, so it only affects packets leaving through that interface.

### 1. Measure what you injected
**Concept:** RTT, loss and jitter are the three numbers that decide whether real-time apps
(calls, games) feel OK. Impairments are usually **one-directional**, but RTT always covers
both directions.

**Try it:**
```bash
sudo ip netns exec router tc qdisc add dev r-b root netem delay 50ms 10ms loss 5%
sudo ip netns exec lan-a ./build/udp-ping/udp_ping 192.168.20.10 -c 200 -i 50
```
**What you should see:** average RTT around 50 ms, ranging roughly 40-60 ms, loss near 5%
(it's random, so 200 probes give a noisy estimate), and a jitter value of a few ms.

**Why:** netem on `r-b` only touches probes going *to* lan-b. Replies leave the router through
`r-a`, unimpaired. The whole 50 ms and 5% come from one direction.

**Check yourself:** with only round-trip numbers, can you tell which direction is lossy?
<details><summary>Answer</summary>No. A lost probe and a lost reply look identical to the
sender. Capture on both ends (or on the router, per interface) to find out, which is the same
reason the casebook records its capture point.</details>

### 2. Make the network reorder packets
**Concept:** **reordering** means packets arrive in a different order than they were sent.
It breaks naive protocols, and TCP can mistake it for loss.

**Try it:**
```bash
sudo ip netns exec router tc qdisc change dev r-b root netem delay 50ms 20ms
sudo ip netns exec lan-a ./build/udp-ping/udp_ping 192.168.20.10 -c 200 -i 50   # few or none
sudo ip netns exec lan-a ./build/udp-ping/udp_ping 192.168.20.10 -c 200 -i 5    # many "out of order"
```
**What you should see:** at 50 ms spacing there's little or no reordering. At 5 ms the
`reordered` count jumps.

**Why:** netem gives each packet its own random delay (30-70 ms). When packets are sent 5 ms
apart, a later one can easily draw a shorter delay and overtake an earlier one. At 50 ms
apart they almost never can.

**Check yourself:** real single-path networks reorder rarely. Where does real-world
reordering come from?
<details><summary>Answer</summary>Parallel paths: ECMP load balancing, link bonding, and
multi-queue NICs. Packets of one flow taking different paths can arrive out of order.
Routers hash flows onto one path precisely to avoid this.</details>

### 3. Fragmentation multiplies loss
*(Continues from exercise 2's qdisc. If you start here, use `tc qdisc add` instead of `change`.)*

**Concept:** UDP doesn't split data into segments the way TCP does. A datagram bigger than the
link MTU is split by IP into **fragments**, and losing any one fragment loses the whole
datagram. That's why DNS and QUIC work hard to stay under the path MTU.

**Try it:**
```bash
sudo ip netns exec router tc qdisc change dev r-b root netem loss 10%
sudo ip netns exec lan-a tcpdump -ni a-eth0 -v 'host 192.168.20.10' -c 6       # terminal 2
sudo ip netns exec lan-a ./build/udp-ping/udp_ping 192.168.20.10 -c 400 -i 10 -s 1472
sudo ip netns exec lan-a ./build/udp-ping/udp_ping 192.168.20.10 -c 400 -i 10 -s 1473
sudo ip netns exec router tc qdisc del dev r-b root
```
**What you should see:** a 1473-byte probe leaves as two packets: tcpdump shows `flags [+]`
(more fragments) and then a fragment `offset`. Loss rises from about 10% to about 19%.

**Why:** 1472 + 8 (UDP) + 20 (IP) = 1500, exactly the MTU. One more byte and the probe
doesn't fit, so it's fragmented. With each fragment independently surviving 90% of the time,
the datagram survives only 0.9 × 0.9 = 81%.

**Check yourself:** if a datagram is split into 5 fragments on a 2%-loss path, what fraction
of datagrams is lost?
<details><summary>Answer</summary>1 − 0.98⁵ ≈ 9.6%: nearly five times the packet loss rate.</details>

## Sample output
Loopback on macOS while building this. Run it across the lab or a real path for interesting numbers.
```
UDP PING 127.0.0.1:9124: 200-byte probes
200 bytes from 127.0.0.1: seq=0 time=0.130 ms
...
--- 127.0.0.1 udp_ping statistics ---
5 sent, 5 received, 0.0% loss, 0 duplicates, 0 reordered, 0 late
rtt min/avg/max/mdev = 0.130/0.156/0.177/0.019 ms, jitter 0.005 ms
```
With no server running, the ICMP port-unreachable surfaces as `ECONNREFUSED` on the
connected socket, and the client says so.

## Known limitations
- The server echoes to anyone who sends a valid probe. Don't leave it running on a public IP: it's a (small, 1:1) reflection target.
- IPv4/IPv6 via getaddrinfo, but no per-probe DSCP or TTL control.
- The sequence space caps at `-c`, so there's no continuous mode.
