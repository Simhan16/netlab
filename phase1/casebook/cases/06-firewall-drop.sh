# A firewall silently drops the connection (the lab's guest isolation rule:
# lan-b may not reach lan-a). DROP sends nothing back, so the client just
# retransmits SYNs with exponential backoff until it gives up.
CASE_LABEL=firewall_drop
CASE_DESCRIPTION="guest LAN connects to a trusted-LAN server; a firewall silently drops the SYNs"
CAPTURE_NS=$LAN_B_NS
CAPTURE_IF=b-eth0

case_break() { start_ns_http "$LAN_A_NS" "$LAN_A_HOST" 8080; }
case_trigger() { in_ns "$LAN_B_NS" curl -sS -o /dev/null --max-time 8 "http://$LAN_A_HOST:8080/"; }
case_fix() { stop_ns_http; }
