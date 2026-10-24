#!/usr/bin/env bash
# Switch the tunnel between the two sites: none | gre | vxlan | wireguard.
#
#   sudo ./tunnel.sh gre
#
# Each mode removes the previous tunnel first, so you can flip between them and
# compare captures of the same traffic.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

require_root
mode=${1:-}
[[ $mode =~ ^(none|gre|vxlan|wireguard)$ ]] || die "usage: $0 none|gre|vxlan|wireguard"
lab_running || die "lab is not running; run setup.sh first"

site() { ip -n "$1" "${@:2}"; }

clear_tunnels() {
  for ns in "$SITE_A_NS" "$SITE_B_NS"; do
    # Deleting an interface also deletes every route through it.
    for dev in gre1 vxlan100 wg0; do site "$ns" link del "$dev" 2>/dev/null || true; done
  done
}

gre() {
  # GRE: wrap each inner IP packet in a new IP header (protocol 47) plus a 4-byte GRE
  # header. Point to point, unencrypted, routed (Layer 3): the oldest, simplest tunnel.
  site "$SITE_A_NS" link add gre1 type gre local "$SITE_A_WAN_IP" remote "$SITE_B_WAN_IP" ttl 64
  site "$SITE_B_NS" link add gre1 type gre local "$SITE_B_WAN_IP" remote "$SITE_A_WAN_IP" ttl 64
  # 1500 underlay - 20 (outer IP) - 4 (GRE) = 1476. Set explicitly so it never depends on autodetection.
  site "$SITE_A_NS" link set gre1 mtu 1476 up
  site "$SITE_B_NS" link set gre1 mtu 1476 up
  site "$SITE_A_NS" addr add "$GRE_A_IP" dev gre1
  site "$SITE_B_NS" addr add "$GRE_B_IP" dev gre1
  # The tunnel is just an interface, so reaching the far LAN is an ordinary route.
  site "$SITE_A_NS" route add "$SITE_B_LAN" dev gre1
  site "$SITE_B_NS" route add "$SITE_A_LAN" dev gre1
}

vxlan() {
  # VXLAN: wrap whole Ethernet *frames* in UDP. Joined to a bridge at each site, it
  # stretches one Layer 2 segment across the underlay: vA and vB end up on the same
  # subnet, ARPing for each other, two routers apart.
  site "$SITE_A_NS" link add vxlan100 type vxlan id "$VXLAN_VNI" local "$SITE_A_WAN_IP" \
    remote "$SITE_B_WAN_IP" dstport "$VXLAN_PORT" dev A-wan
  site "$SITE_B_NS" link add vxlan100 type vxlan id "$VXLAN_VNI" local "$SITE_B_WAN_IP" \
    remote "$SITE_A_WAN_IP" dstport "$VXLAN_PORT" dev B-wan
  for ns in "$SITE_A_NS" "$SITE_B_NS"; do
    # 1500 - 20 (outer IP) - 8 (UDP) - 8 (VXLAN) - 14 (inner Ethernet) = 1450.
    site "$ns" link set vxlan100 mtu 1450 master br100 up
  done
}

wireguard() {
  command -v wg >/dev/null || die "wireguard mode needs the wg tool: sudo apt install wireguard-tools"
  # WireGuard: encrypt each inner IP packet and send it in UDP. Each side is identified
  # by a public key; "allowed-ips" is both the routing table and the access list.
  (umask 077
   for side in a b; do
     wg genkey > "$STATE_DIR/wg-$side.key"
     wg pubkey < "$STATE_DIR/wg-$side.key" > "$STATE_DIR/wg-$side.pub"
   done)
  site "$SITE_A_NS" link add wg0 type wireguard
  site "$SITE_B_NS" link add wg0 type wireguard
  in_ns "$SITE_A_NS" wg set wg0 listen-port "$WG_PORT" private-key "$STATE_DIR/wg-a.key" \
    peer "$(cat "$STATE_DIR/wg-b.pub")" endpoint "$SITE_B_WAN_IP:$WG_PORT" allowed-ips "$SITE_B_LAN,${WG_B_IP%/*}/32"
  in_ns "$SITE_B_NS" wg set wg0 listen-port "$WG_PORT" private-key "$STATE_DIR/wg-b.key" \
    peer "$(cat "$STATE_DIR/wg-a.pub")" endpoint "$SITE_A_WAN_IP:$WG_PORT" allowed-ips "$SITE_A_LAN,${WG_A_IP%/*}/32"
  # wg0 defaults to MTU 1420: 1500 - 40 (outer IPv6, the worst case) - 8 (UDP) - 32 (WireGuard).
  site "$SITE_A_NS" addr add "$WG_A_IP" dev wg0
  site "$SITE_B_NS" addr add "$WG_B_IP" dev wg0
  site "$SITE_A_NS" link set wg0 up
  site "$SITE_B_NS" link set wg0 up
  site "$SITE_A_NS" route add "$SITE_B_LAN" dev wg0
  site "$SITE_B_NS" route add "$SITE_A_LAN" dev wg0
}

clear_tunnels
case $mode in
  none) ;;
  gre) gre ;;
  vxlan) vxlan ;;
  wireguard) wireguard ;;
esac
echo "$mode" > "$STATE_DIR/mode"
log "tunnel mode: $mode. check it: sudo $SCRIPT_DIR/test.sh"
