#!/usr/bin/env bash
# Build both topologies. Re-runnable: it tears down first. Starts with no tunnel;
# pick one with ./tunnel.sh gre|vxlan|wireguard.
#
# Part 1, VLANs:                          Part 2, sites:
#   h1 ─┐ VLAN 10                           hA ── siteA ═══ wan ═══ siteB ── hB
#   h2 ─┤                                   vA ──┘  (tunnel over the wan)  └── vB
#       ├── sw ══ trunk ══ rtr
#   h3 ─┤ VLAN 20
#   h4 ─┘
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

require_root
for cmd in ip bridge sysctl iptables; do
  command -v "$cmd" >/dev/null || die "missing required command: $cmd"
done

"$SCRIPT_DIR/teardown.sh"
mkdir -p "$STATE_DIR"
chmod 700 "$STATE_DIR"

log "creating namespaces"
for ns in "${ALL_NS[@]}"; do
  ip netns add "$ns"
  ip -n "$ns" link set lo up
done

# ---------------------------------------------------------------------------------------
log "part 1: VLAN-aware switch"
# vlan_filtering turns a plain bridge (one big broadcast domain) into a VLAN switch:
# every port gets a list of VLANs it carries, and frames never cross between VLANs.
ip -n "$SW_NS" link add br0 type bridge vlan_filtering 1
ip -n "$SW_NS" link set br0 up

for ns in "${VLAN_HOSTS[@]}"; do
  IFS='|' read -r vid addr gw <<< "${VLAN_HOST_CFG[$ns]}"
  name=${ns#ov-}                              # h1, h2, ...
  ip link add "$name-eth0" netns "$ns" type veth peer name "sw-$name" netns "$SW_NS"
  ip -n "$SW_NS" link set "sw-$name" master br0
  ip -n "$SW_NS" link set "sw-$name" up
  # An access port: untagged frames from the host belong to VLAN $vid (pvid), and
  # frames leave towards the host with the tag removed (untagged). The host never
  # knows VLANs exist.
  in_ns "$SW_NS" bridge vlan add dev "sw-$name" vid "$vid" pvid untagged
  in_ns "$SW_NS" bridge vlan del dev "sw-$name" vid 1    # drop the default VLAN

  ip -n "$ns" addr add "$addr" dev "$name-eth0"
  ip -n "$ns" link set "$name-eth0" up
  ip -n "$ns" route add default via "$gw"
done

log "part 1: trunk to the router (router on a stick)"
ip link add rtr-eth0 netns "$RTR_NS" type veth peer name sw-rtr netns "$SW_NS"
ip -n "$SW_NS" link set sw-rtr master br0
ip -n "$SW_NS" link set sw-rtr up
# A trunk port carries several VLANs, each frame keeping its 802.1Q tag so the far
# end can tell them apart. No pvid: untagged frames aren't accepted at all.
in_ns "$SW_NS" bridge vlan add dev sw-rtr vid 10
in_ns "$SW_NS" bridge vlan add dev sw-rtr vid 20
in_ns "$SW_NS" bridge vlan del dev sw-rtr vid 1

ip -n "$RTR_NS" link set rtr-eth0 up
# One subinterface per VLAN: rtr-eth0.10 sends and receives frames tagged 10.
ip -n "$RTR_NS" link add link rtr-eth0 name rtr-eth0.10 type vlan id 10
ip -n "$RTR_NS" link add link rtr-eth0 name rtr-eth0.20 type vlan id 20
ip -n "$RTR_NS" addr add "$RTR_VLAN10_IP" dev rtr-eth0.10
ip -n "$RTR_NS" addr add "$RTR_VLAN20_IP" dev rtr-eth0.20
ip -n "$RTR_NS" link set rtr-eth0.10 up
ip -n "$RTR_NS" link set rtr-eth0.20 up
in_ns "$RTR_NS" sysctl -qw net.ipv4.ip_forward=1

# ---------------------------------------------------------------------------------------
log "part 2: the underlay (the internet between the sites)"
ip link add A-wan netns "$SITE_A_NS" type veth peer name wan-A netns "$WAN_NS"
ip link add B-wan netns "$SITE_B_NS" type veth peer name wan-B netns "$WAN_NS"
ip -n "$WAN_NS" addr add "$WAN_A_IP/$WAN_PREFIX" dev wan-A
ip -n "$WAN_NS" addr add "$WAN_B_IP/$WAN_PREFIX" dev wan-B
ip -n "$WAN_NS" link set wan-A up
ip -n "$WAN_NS" link set wan-B up
# The wan routes only what it's directly connected to: the two public /30s.
# It has no idea 10.1.0.0/24 or 10.2.0.0/24 exist, just like the real internet.
in_ns "$WAN_NS" sysctl -qw net.ipv4.ip_forward=1

ip -n "$SITE_A_NS" addr add "$SITE_A_WAN_IP/$WAN_PREFIX" dev A-wan
ip -n "$SITE_A_NS" link set A-wan up
ip -n "$SITE_A_NS" route add default via "$WAN_A_IP"
ip -n "$SITE_B_NS" addr add "$SITE_B_WAN_IP/$WAN_PREFIX" dev B-wan
ip -n "$SITE_B_NS" link set B-wan up
ip -n "$SITE_B_NS" route add default via "$WAN_B_IP"

log "part 2: private LANs behind each site"
ip link add hA-eth0 netns "$HOST_A_NS" type veth peer name A-lan netns "$SITE_A_NS"
ip link add hB-eth0 netns "$HOST_B_NS" type veth peer name B-lan netns "$SITE_B_NS"
ip -n "$SITE_A_NS" addr add "$SITE_A_GW/$LAN_PREFIX" dev A-lan
ip -n "$SITE_A_NS" link set A-lan up
ip -n "$SITE_B_NS" addr add "$SITE_B_GW/$LAN_PREFIX" dev B-lan
ip -n "$SITE_B_NS" link set B-lan up
ip -n "$HOST_A_NS" addr add "$HOST_A_IP/$LAN_PREFIX" dev hA-eth0
ip -n "$HOST_A_NS" link set hA-eth0 up
ip -n "$HOST_A_NS" route add default via "$SITE_A_GW"
ip -n "$HOST_B_NS" addr add "$HOST_B_IP/$LAN_PREFIX" dev hB-eth0
ip -n "$HOST_B_NS" link set hB-eth0 up
ip -n "$HOST_B_NS" route add default via "$SITE_B_GW"
in_ns "$SITE_A_NS" sysctl -qw net.ipv4.ip_forward=1
in_ns "$SITE_B_NS" sysctl -qw net.ipv4.ip_forward=1

log "part 2: a bridge at each site for the VXLAN segment (unused until ./tunnel.sh vxlan)"
ip link add vA-eth0 netns "$VX_A_NS" type veth peer name A-vx netns "$SITE_A_NS"
ip link add vB-eth0 netns "$VX_B_NS" type veth peer name B-vx netns "$SITE_B_NS"
ip -n "$SITE_A_NS" link add br100 type bridge
ip -n "$SITE_B_NS" link add br100 type bridge
ip -n "$SITE_A_NS" link set A-vx master br100
ip -n "$SITE_B_NS" link set B-vx master br100
for ns in "$SITE_A_NS" "$SITE_B_NS"; do ip -n "$ns" link set br100 up; done
ip -n "$SITE_A_NS" link set A-vx up
ip -n "$SITE_B_NS" link set B-vx up
# vA and vB keep the default 1500-byte MTU on purpose: see exercise 7.
ip -n "$VX_A_NS" addr add "$VX_A_IP" dev vA-eth0
ip -n "$VX_A_NS" link set vA-eth0 up
ip -n "$VX_B_NS" addr add "$VX_B_IP" dev vB-eth0
ip -n "$VX_B_NS" link set vB-eth0 up

echo none > "$STATE_DIR/mode"
log "done. next: sudo $SCRIPT_DIR/test.sh, then sudo $SCRIPT_DIR/tunnel.sh gre"
