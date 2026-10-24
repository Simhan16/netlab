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

## Experiments (in the netns lab)
```bash
sudo ip netns exec lan-b ./build/udp-ping/udp_pong &
sudo ip netns exec router tc qdisc add dev r-b root netem delay 50ms 20ms loss 5%
sudo ip netns exec lan-a ./build/udp-ping/udp_ping 192.168.20.10 -c 100 -i 50
sudo ip netns exec router tc qdisc del dev r-b root
```
Compare the loss and jitter it reports with what you configured. Then try `-s 1472` against
`-s 1473` on a 1500-byte link and watch with tcpdump: the larger probe leaves as two IP
fragments. Compare that with how TCP behaves in the casebook's MTU black-hole case.

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
