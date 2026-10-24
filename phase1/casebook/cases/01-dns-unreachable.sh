# The resolver lan-a is configured to use doesn't exist (192.0.2.0/24 is
# TEST-NET, never routed). Every lookup times out; no TCP connection is even tried.
CASE_LABEL=dns_unreachable
CASE_DESCRIPTION="lan-a's resolv.conf points at a dead DNS server; queries go unanswered and the app never connects"
CAPTURE_NS=$LAN_A_NS
CAPTURE_IF=a-eth0

case_break() { echo "nameserver 192.0.2.53" >"/etc/netns/$LAN_A_NS/resolv.conf"; }
case_trigger() { in_ns "$LAN_A_NS" curl -sS -o /dev/null --max-time 8 https://example.com; }
case_fix() { echo "nameserver $DNS_SERVER" >"/etc/netns/$LAN_A_NS/resolv.conf"; }
