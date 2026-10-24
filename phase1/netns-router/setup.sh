#!/usr/bin/env bash
# Build two LANs behind a router namespace, NATed out through the host's uplink.
#
#   lan-a 192.168.10.10 ─┐
#                        ├─ router ── 192.168.255.0/30 ── host ── ens5 ── internet
#   lan-b 192.168.20.10 ─┘
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

require_root
for cmd in ip iptables sysctl; do
  command -v "$cmd" >/dev/null || die "missing required command: $cmd"
done
[[ -n $UPLINK_IF ]] || die "could not detect the uplink interface; set UPLINK_IF"
log "uplink interface: $UPLINK_IF"

# Always start from a clean slate so the script can be re-run safely.
"$SCRIPT_DIR/teardown.sh"

mkdir -p "$STATE_DIR"
cat /proc/sys/net/ipv4/ip_forward > "$STATE_DIR/ip_forward"

log "creating namespaces"
# A network namespace is a separate copy of the kernel's network stack:
# its own interfaces, routes, ARP table and firewall. Each one acts like a machine.
for ns in "$LAN_A_NS" "$LAN_B_NS" "$ROUTER_NS"; do
  ip netns add "$ns"
  ip -n "$ns" link set lo up
done

log "wiring veth pairs"
# A veth pair is a virtual Ethernet cable: a frame sent into one end comes out the other.
ip link add a-eth0 netns "$LAN_A_NS" type veth peer name r-a netns "$ROUTER_NS"
ip link add b-eth0 netns "$LAN_B_NS" type veth peer name r-b netns "$ROUTER_NS"
ip link add h-up type veth peer name r-up netns "$ROUTER_NS"

log "addressing and routing"
ip -n "$LAN_A_NS" addr add "$LAN_A_HOST/$LAN_PREFIX" dev a-eth0
ip -n "$LAN_A_NS" link set a-eth0 up
ip -n "$LAN_A_NS" route add default via "$LAN_A_GW"

ip -n "$LAN_B_NS" addr add "$LAN_B_HOST/$LAN_PREFIX" dev b-eth0
ip -n "$LAN_B_NS" link set b-eth0 up
ip -n "$LAN_B_NS" route add default via "$LAN_B_GW"

ip -n "$ROUTER_NS" addr add "$LAN_A_GW/$LAN_PREFIX" dev r-a
ip -n "$ROUTER_NS" addr add "$LAN_B_GW/$LAN_PREFIX" dev r-b
ip -n "$ROUTER_NS" addr add "$UPLINK_ROUTER_IP/$UPLINK_PREFIX" dev r-up
for dev in r-a r-b r-up; do ip -n "$ROUTER_NS" link set "$dev" up; done
ip -n "$ROUTER_NS" route add default via "$UPLINK_HOST_IP"

ip addr add "$UPLINK_HOST_IP/$UPLINK_PREFIX" dev h-up
ip link set h-up up
# Return path: the host must know the LANs live behind the router namespace.
ip route add "$LAN_A_NET" via "$UPLINK_ROUTER_IP"
ip route add "$LAN_B_NET" via "$UPLINK_ROUTER_IP"

log "enabling IP forwarding"
# By default Linux drops packets not addressed to itself; forwarding makes it a router.
# The setting is per-namespace, so both the router and the host need it.
ip netns exec "$ROUTER_NS" sysctl -qw net.ipv4.ip_forward=1
sysctl -qw net.ipv4.ip_forward=1

log "router firewall (lives only inside the router namespace)"
rfw() { ip netns exec "$ROUTER_NS" iptables "$@"; }
rfw -P FORWARD DROP
# Stateful firewall: once a connection is allowed, its replies are allowed too.
rfw -A FORWARD -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
rfw -A FORWARD -i r-a -m comment --comment "lan-a trusted: anywhere" -j ACCEPT
rfw -A FORWARD -i r-b -o r-up -m comment --comment "lan-b guest: internet only" -j ACCEPT
# Explicit drop (instead of relying on the policy) so its packet counter shows blocked attempts.
rfw -A FORWARD -i r-b -o r-a -m comment --comment "guest to trusted blocked" -j DROP

log "host NAT to the internet"
# MASQUERADE rewrites the 192.168.x source to the instance's own IP on the way out,
# and conntrack reverses it for replies. This is what your home router does.
for rule in "${HOST_RULES[@]}"; do
  IFS='|' read -r table chain spec <<< "$rule"
  read -ra args <<< "$spec"
  # -I (insert at top) so a DROP policy from ufw or docker can't shadow these.
  iptables -t "$table" -I "$chain" "${args[@]}"
done

log "per-namespace DNS"
# `ip netns exec` bind-mounts /etc/netns/<ns>/resolv.conf over /etc/resolv.conf.
# Needed because the host's resolver (127.0.0.53) is unreachable from inside a namespace.
for ns in "$LAN_A_NS" "$LAN_B_NS"; do
  mkdir -p "/etc/netns/$ns"
  echo "nameserver $DNS_SERVER" > "/etc/netns/$ns/resolv.conf"
done

log "done. try: sudo $SCRIPT_DIR/test.sh"
