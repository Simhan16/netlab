# 03-mtu-blackhole

**Label:** `mtu_blackhole`

## Root cause
A hop on the path has a smaller MTU, and the ICMP 'fragmentation needed' messages that Path MTU Discovery relies on are filtered. Full-size DF packets disappear silently.

## Expected signature in the mini-tcpdump summary
- Handshake completes (`open`/`reset` state, `synack_to_ack` present)
- Server→client: repeated retransmissions of segments with `max_payload` ≈ MSS (1448/1460)
- `bytes_acked` for the server direction stays tiny: the transfer never progresses
- No `frag-needed` ICMP reaches the sender (that's the black hole); if you *do* see `frag-needed` with an MTU, PMTUD is working

## Real-world fix
Allow ICMP type 3 code 4 (and ICMPv6 type 2) through firewalls, or clamp MSS on the router: `iptables -t mangle -A FORWARD -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu`.

## Observed
<!-- Fill in after `sudo ./run_case.sh 03-mtu-blackhole`: paste the relevant lines from
`mini-tcpdump -r pcaps/03-mtu-blackhole/run1.pcap -q` and note anything that differed from the expected signature. -->
