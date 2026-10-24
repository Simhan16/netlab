# Path MTU black hole. The router's link to lan-a has MTU 1200 but lan-a still
# advertises MSS 1460, so the server's full-size segments (with DF set) can't
# be forwarded. The router would send ICMP "fragmentation needed" so the
# server could shrink them, but a firewall drops that ICMP. Result: the
# handshake and small packets work, large packets vanish, the transfer stalls.
CASE_LABEL=mtu_blackhole
CASE_DESCRIPTION="1200-byte MTU hop with ICMP frag-needed filtered; handshake succeeds but full-size segments are silently dropped"
CAPTURE_NS=$ROUTER_NS
CAPTURE_IF=r-up   # router uplink: sees the server's big segments arrive and get retransmitted
CASE_SETTLE=1

case_break() {
  start_host_http
  ip -n "$ROUTER_NS" link set r-a mtu 1200
  in_ns "$ROUTER_NS" iptables -I OUTPUT -p icmp --icmp-type fragmentation-needed -m comment --comment netlab-case -j DROP
}
case_trigger() { in_ns "$LAN_A_NS" curl -sS -o /dev/null --max-time 12 "$HOST_HTTP_URL/big.bin"; }
case_fix() {
  in_ns "$ROUTER_NS" iptables -D OUTPUT -p icmp --icmp-type fragmentation-needed -m comment --comment netlab-case -j DROP 2>/dev/null || true
  ip -n "$ROUTER_NS" link set r-a mtu 1500
  ip route flush cache 2>/dev/null || true
  stop_host_http
}
