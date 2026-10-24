# The host is up but nothing listens on the port (service crashed / wrong
# port in config). The kernel answers the SYN with a RST immediately.
CASE_LABEL=connection_refused
CASE_DESCRIPTION="client connects to a port with no listening service; the host answers SYN with RST"
CAPTURE_NS=$LAN_A_NS
CAPTURE_IF=a-eth0

case_trigger() { in_ns "$LAN_A_NS" curl -sS -o /dev/null --max-time 5 "http://$LAN_B_HOST:9090/"; }
