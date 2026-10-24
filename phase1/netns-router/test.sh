#!/usr/bin/env bash
# Check that the lab behaves as designed: what should connect does, what should be blocked is.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

require_root
ip netns list | grep -qw "$ROUTER_NS" || die "lab is not running; run setup.sh first"

pass=0
fail=0
in_ns() { ip netns exec "$@"; }

# check <ok|blocked> <description> <command...>
check() {
  local expect=$1 desc=$2 got
  shift 2
  if "$@" >/dev/null 2>&1; then got=ok; else got=blocked; fi
  if [[ $got == "$expect" ]]; then
    printf 'PASS  %-40s %s\n' "$desc" "$got"
    pass=$((pass + 1))
  else
    printf 'FAIL  %-40s expected %s, got %s\n' "$desc" "$expect" "$got"
    fail=$((fail + 1))
  fi
}

check ok      "lan-a -> its gateway"          in_ns "$LAN_A_NS" ping -c1 -W2 "$LAN_A_GW"
check ok      "lan-b -> its gateway"          in_ns "$LAN_B_NS" ping -c1 -W2 "$LAN_B_GW"
check ok      "lan-a -> lan-b"                in_ns "$LAN_A_NS" ping -c1 -W2 "$LAN_B_HOST"
check blocked "lan-b -> lan-a (guest isolation)" in_ns "$LAN_B_NS" ping -c1 -W2 "$LAN_A_HOST"
check ok      "lan-a -> internet ($PING_TARGET)" in_ns "$LAN_A_NS" ping -c1 -W2 "$PING_TARGET"
check ok      "lan-b -> internet ($PING_TARGET)" in_ns "$LAN_B_NS" ping -c1 -W2 "$PING_TARGET"
check ok      "lan-a DNS + HTTPS"             in_ns "$LAN_A_NS" curl -sS --max-time 5 -o /dev/null https://example.com
check ok      "lan-b DNS + HTTPS"             in_ns "$LAN_B_NS" curl -sS --max-time 5 -o /dev/null https://example.com

echo
echo "router FORWARD counters (the DROP line counts blocked guest packets):"
in_ns "$ROUTER_NS" iptables -L FORWARD -v -n --line-numbers

echo
echo "$pass passed, $fail failed"
(( fail == 0 ))
