#!/bin/bash
# branch-fw: the branch office router. One tunnel home to HQ, and everything,
# internet included, goes through it ("backhaul"): the branch learns its default
# route from HQ over OSPF, so branch users get the same proxy and policy as HQ.
#
#   lan 10.20.10.1 ── branch-fw ── inet 203.0.113.20 ═══ wg-site 10.255.1.2/30 ═══ edge-fw
set -euo pipefail
source /lab/lib.sh

log "VPN: site-to-site tunnel to HQ"
wg_up wg-site 10.255.1.2/30 51820 /state/wg/branch-site.key
# 0.0.0.0/0: any destination may go through this peer (there is only one: HQ).
wg set wg-site peer "$(cat /state/wg/edge-site.pub)" endpoint "$EDGE_PUBLIC_IP:51820" \
  allowed-ips 0.0.0.0/0 persistent-keepalive 25

log "firewall"
reset_firewall
ipt() { iptables "$@"; }

ipt -P INPUT DROP
ipt -A INPUT -i lo -j ACCEPT
ipt -A INPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
ipt -A INPUT -p icmp -j ACCEPT
ipt -A INPUT -p 89 -i wg-site -j ACCEPT
ipt -A INPUT -i inet -p udp --dport 51820 -m comment --comment "VPN" -j ACCEPT
ipt -A INPUT -s "$MGMT_NET" -m comment --comment "admins manage the router" -j ACCEPT
ipt -A INPUT -m comment --comment "default deny" -j DROP

ipt -P FORWARD DROP
ipt -A FORWARD -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
ipt -A FORWARD -m conntrack --ctstate INVALID -j DROP
ipt -A FORWARD -i lan -o wg-site -m comment --comment "branch -> HQ (and the internet, via HQ)" -j ACCEPT
ipt -A FORWARD -i wg-site -o lan -p icmp -j ACCEPT
ipt -A FORWARD -i wg-site -o lan -s "$MGMT_NET" -m comment --comment "IT admins reach the branch" -j ACCEPT
ipt -A FORWARD -m comment --comment "default deny" -j DROP

ipt -t mangle -A FORWARD -o wg+ -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
ipt -t mangle -A FORWARD -i wg+ -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu

start_frr branch
idle
