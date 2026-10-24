# 300 ms of added delay toward lan-a: no loss, everything works, everything is slow.
CASE_LABEL=high_latency
CASE_DESCRIPTION="300 ms of added one-way delay on the client's link; no loss, but every round trip is slow"
CAPTURE_NS=$LAN_A_NS
CAPTURE_IF=a-eth0

case_break() {
  start_host_http
  in_ns "$ROUTER_NS" tc qdisc add dev r-a root netem delay 300ms
}
case_trigger() {
  for _ in 1 2 3; do in_ns "$LAN_A_NS" curl -sS -o /dev/null --max-time 15 "$HOST_HTTP_URL/index.html"; done
}
case_fix() {
  in_ns "$ROUTER_NS" tc qdisc del dev r-a root 2>/dev/null || true
  stop_host_http
}
