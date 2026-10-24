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
