# Asymmetric routing. lan-b gets a second "shortcut" link to the host and a
# route sending its replies to lan-a that way. Requests arrive via the router;
# replies come back a different path and are dropped by reverse-path
# filtering (uRPF) because they arrive on an interface the source shouldn't be
# behind. The server sees SYNs and keeps answering; the client never hears back.
CASE_LABEL=asymmetric_routing
CASE_DESCRIPTION="server replies take a different path than requests and are dropped by reverse-path filtering; server sees SYNs but its SYN-ACKs never arrive"
CAPTURE_NS=$LAN_B_NS
CAPTURE_IF=any    # lan-b's own view: SYNs in on b-eth0, SYN-ACKs out on b-eth1

case_break() {
  RP_ALL=$(in_ns "$ROUTER_NS" sysctl -n net.ipv4.conf.all.rp_filter)
  RP_UP=$(in_ns "$ROUTER_NS" sysctl -n net.ipv4.conf.r-up.rp_filter)
  ip link add h-b2 type veth peer name b-eth1 netns "$LAN_B_NS"
  ip addr add 192.168.30.1/30 dev h-b2 && ip link set h-b2 up
  ip -n "$LAN_B_NS" addr add 192.168.30.2/30 dev b-eth1 && ip -n "$LAN_B_NS" link set b-eth1 up
  ip -n "$LAN_B_NS" route add "$LAN_A_NET" via 192.168.30.1
  iptables -I FORWARD -i h-b2 -m comment --comment netlab-case -j ACCEPT
  in_ns "$ROUTER_NS" sysctl -qw net.ipv4.conf.all.rp_filter=1 net.ipv4.conf.r-up.rp_filter=1
  start_ns_http "$LAN_B_NS" "$LAN_B_HOST" 8080
}
case_trigger() { in_ns "$LAN_A_NS" curl -sS -o /dev/null --max-time 10 "http://$LAN_B_HOST:8080/"; }
case_fix() {
  stop_ns_http
  iptables -D FORWARD -i h-b2 -m comment --comment netlab-case -j ACCEPT 2>/dev/null || true
  ip link del h-b2 2>/dev/null || true
  in_ns "$ROUTER_NS" sysctl -qw "net.ipv4.conf.all.rp_filter=${RP_ALL:-0}" "net.ipv4.conf.r-up.rp_filter=${RP_UP:-0}" || true
}
