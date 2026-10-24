#!/usr/bin/env bash
# Check that the lab behaves as designed for the current tunnel mode.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

require_root
lab_running || die "lab is not running; run setup.sh first"
mode=$(cat "$STATE_DIR/mode")

pass=0
fail=0
# check <ok|blocked> <description> <command...>
check() {
  local expect=$1 desc=$2 got
  shift 2
  if "$@" >/dev/null 2>&1; then got=ok; else got=blocked; fi
  if [[ $got == "$expect" ]]; then
    printf 'PASS  %-52s %s\n' "$desc" "$got"
    pass=$((pass + 1))
  else
    printf 'FAIL  %-52s expected %s, got %s\n' "$desc" "$expect" "$got"
    fail=$((fail + 1))
  fi
}
# ping_df <ns> <dst> <payload bytes>: one ping with Don't Fragment set
ping_df() { in_ns "$1" ping -c1 -W2 -M "do" -s "$3" "$2"; }

echo "part 1: VLANs"
check ok      "h1 -> h2 (same VLAN)"                      in_ns ov-h1 ping -c1 -W2 10.10.0.2
check ok      "h3 -> h4 (same VLAN)"                      in_ns ov-h3 ping -c1 -W2 10.20.0.4
check ok      "h1 -> h3 (VLAN 10 -> 20, via the router)"  in_ns ov-h1 ping -c1 -W2 10.20.0.3
# With TTL 1 the packet dies at the first router, so this only works within one VLAN.
check ok      "h1 -> h2 with TTL 1 (no router needed)"    in_ns ov-h1 ping -c1 -W2 -t1 10.10.0.2
check blocked "h1 -> h3 with TTL 1 (must cross a router)" in_ns ov-h1 ping -c1 -W2 -t1 10.20.0.3

echo
echo "part 2: tunnel mode '$mode'"
check ok "siteA -> siteB over the underlay" in_ns "$SITE_A_NS" ping -c1 -W2 "$SITE_B_WAN_IP"
case $mode in
  none)
    check blocked "hA -> hB (no tunnel: the wan can't route 10.x)" in_ns "$HOST_A_NS" ping -c1 -W2 "$HOST_B_IP"
    check blocked "vA -> vB (no VXLAN segment)"                    in_ns "$VX_A_NS" ping -c1 -W2 "${VX_B_IP%/*}"
    ;;
  gre)
    check ok      "hA -> hB through GRE"                  in_ns "$HOST_A_NS" ping -c1 -W2 "$HOST_B_IP"
    check ok      "1476-byte packet fits (1448 + 28)"     ping_df "$HOST_A_NS" "$HOST_B_IP" 1448
    check blocked "1477-byte packet with DF doesn't"      ping_df "$HOST_A_NS" "$HOST_B_IP" 1449
    ;;
  vxlan)
    check ok      "vA -> vB, same subnet across 2 sites"  in_ns "$VX_A_NS" ping -c1 -W2 "${VX_B_IP%/*}"
    check ok      "1450-byte packet fits (1422 + 28)"     ping_df "$VX_A_NS" "${VX_B_IP%/*}" 1422
    check blocked "1451-byte packet doesn't"              ping_df "$VX_A_NS" "${VX_B_IP%/*}" 1423
    check blocked "hA -> hB (VXLAN joins vA/vB only)"     in_ns "$HOST_A_NS" ping -c1 -W2 "$HOST_B_IP"
    ;;
  wireguard)
    check ok      "hA -> hB through WireGuard"            in_ns "$HOST_A_NS" ping -c1 -W2 "$HOST_B_IP"
    check ok      "1420-byte packet fits (1392 + 28)"     ping_df "$HOST_A_NS" "$HOST_B_IP" 1392
    check blocked "1421-byte packet with DF doesn't"      ping_df "$HOST_A_NS" "$HOST_B_IP" 1393
    echo
    in_ns "$SITE_A_NS" wg show wg0
    ;;
esac

echo
echo "$pass passed, $fail failed"
(( fail == 0 ))
