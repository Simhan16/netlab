# Baseline: everything works. Without healthy captures an eval can't measure
# false positives -- a diagnoser that always says "broken" would look perfect.
CASE_LABEL=healthy
CASE_DESCRIPTION="lan-a resolves a name and fetches pages over HTTPS and HTTP with no faults injected"
CAPTURE_NS=$LAN_A_NS
CAPTURE_IF=a-eth0

case_break() { start_host_http; }
case_trigger() {
  in_ns "$LAN_A_NS" curl -sS -o /dev/null --max-time 10 https://example.com
  in_ns "$LAN_A_NS" curl -sS -o /dev/null --max-time 10 "$HOST_HTTP_URL/big.bin"
}
case_fix() { stop_host_http; }
