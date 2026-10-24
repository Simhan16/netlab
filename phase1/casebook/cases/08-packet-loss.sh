# Random 8% loss on the router's link toward lan-a (netem). TCP recovers with
# retransmissions, so the download completes, just slowly.
# Needs sch_netem: on AWS kernels, apt install linux-modules-extra-$(uname -r).
CASE_LABEL=packet_loss
CASE_DESCRIPTION="8% random packet loss toward the client; download completes with many retransmissions and duplicate ACKs"
CAPTURE_NS=$LAN_A_NS
CAPTURE_IF=a-eth0

case_break() {
  start_host_http
  in_ns "$ROUTER_NS" tc qdisc add dev r-a root netem loss 8%
}
case_trigger() { in_ns "$LAN_A_NS" curl -sS -o /dev/null --max-time 30 "$HOST_HTTP_URL/big.bin"; }
case_fix() {
  in_ns "$ROUTER_NS" tc qdisc del dev r-a root 2>/dev/null || true
  stop_host_http
}
