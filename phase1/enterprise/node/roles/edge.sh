#!/bin/bash
# edge-fw: HQ's internet edge. The "north-south" firewall, NAT, the inbound port
# forward to the DMZ web server, and both VPN concentrators (site-to-site and remote access).
#
#   inet 203.0.113.10 ── edge-fw ── transit 10.255.0.1 (to core-rtr)
#                           ├── dmz 172.16.10.1
#                           ├── wg-site 10.255.1.1/30 (to branch-fw, OSPF runs over it)
#                           └── wg-ra   10.255.2.1/24 (remote workers)
set -euo pipefail
source /lab/lib.sh

log "VPN: site-to-site tunnel to the branch"
wg_up wg-site 10.255.1.1/30 51820 /state/wg/edge-site.key
# allowed-ips is WireGuard's routing table *and* its source filter. 224.0.0.5/.6 are the
# OSPF multicast groups: without them, wg drops the routing protocol's own hellos.
wg set wg-site peer "$(cat /state/wg/branch-site.pub)" endpoint "$BRANCH_PUBLIC_IP:51820" \
  allowed-ips "$BRANCH_NET,10.255.1.2/32,224.0.0.5/32,224.0.0.6/32"

log "VPN: remote-access concentrator"
wg_up wg-ra 10.255.2.1/24 51821 /state/wg/edge-ra.key
wg set wg-ra peer "$(cat /state/wg/remote.pub)" allowed-ips 10.255.2.10/32

log "firewall + NAT"
reset_firewall
ipt() { iptables "$@"; }

# --- traffic to the firewall itself ---
ipt -P INPUT DROP
ipt -A INPUT -i lo -j ACCEPT
ipt -A INPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
ipt -A INPUT -p icmp -j ACCEPT
ipt -A INPUT -p 89 -i transit -j ACCEPT
ipt -A INPUT -p 89 -i wg-site -j ACCEPT
ipt -A INPUT -i inet -p udp -m multiport --dports 51820,51821 -m comment --comment "VPN" -j ACCEPT
ipt -A INPUT -s "$MGMT_NET" -m comment --comment "admins manage the firewall" -j ACCEPT
ipt -A INPUT -m comment --comment "default deny" -j DROP

# --- traffic through the firewall: default deny, then each flow the business needs ---
ipt -P FORWARD DROP
ipt -A FORWARD -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
ipt -A FORWARD -m conntrack --ctstate INVALID -j DROP
ipt -A FORWARD -s "$HQ_NET,$BRANCH_NET,$RA_NET" -p icmp -m comment --comment "ping for troubleshooting" -j ACCEPT
ipt -A FORWARD -s "$HQ_NET" -d "$BRANCH_NET" -m comment --comment "HQ <-> branch" -j ACCEPT
ipt -A FORWARD -s "$BRANCH_NET" -d "$HQ_NET" -m comment --comment "HQ <-> branch" -j ACCEPT
ipt -A FORWARD -i wg-ra -d "$INTRANET_IP" -p tcp -m multiport --dports 80,443 -m comment --comment "remote: intranet" -j ACCEPT
ipt -A FORWARD -i wg-ra -d "$DNS_IP" -p udp --dport 53 -m comment --comment "remote: DNS" -j ACCEPT
ipt -A FORWARD -i wg-ra -d "$DNS_IP" -p tcp --dport 53 -m comment --comment "remote: DNS" -j ACCEPT
ipt -A FORWARD -s "$HQ_NET,$BRANCH_NET" -d "$WWW_IP" -p tcp -m multiport --dports 80,443 -m comment --comment "staff -> public site" -j ACCEPT
ipt -A FORWARD -s "$HQ_NET,$BRANCH_NET" -d "$PROXY_IP" -p tcp --dport 3128 -m comment --comment "staff -> web proxy" -j ACCEPT
ipt -A FORWARD -o inet -s "$DNS_IP" -p udp --dport 53 -m comment --comment "DNS server -> upstream resolvers" -j ACCEPT
ipt -A FORWARD -o inet -s "$DNS_IP" -p tcp --dport 53 -m comment --comment "DNS server -> upstream resolvers" -j ACCEPT
ipt -A FORWARD -o inet -s "$JUMPBOX_IP" -m comment --comment "IT jumpbox: direct internet" -j ACCEPT
ipt -A FORWARD -o inet -s "$PROXY_IP" -p tcp -m multiport --dports 80,443 -m comment --comment "proxy -> web" -j ACCEPT
ipt -A FORWARD -o inet -s "$PROXY_IP" -p udp --dport 53 -m comment --comment "proxy -> public DNS" -j ACCEPT
ipt -A FORWARD -i inet -d "$WWW_IP" -p tcp --dport 80 -m conntrack --ctstate DNAT -m comment --comment "internet -> public site (port forward)" -j ACCEPT
ipt -A FORWARD -m comment --comment "default deny" -j DROP

# --- NAT ---
# Port forward: the public address's port 80 is really the DMZ web server.
ipt -t nat -A PREROUTING -i inet -d "$EDGE_PUBLIC_IP" -p tcp --dport 80 -j DNAT --to-destination "$WWW_IP:80"
# Source NAT: every private address leaves as 203.0.113.10.
ipt -t nat -A POSTROUTING -o inet -s "$HQ_NET,$BRANCH_NET,$DMZ_NET" -j MASQUERADE

# Tunnels have a smaller MTU than the LANs. Rewrite the MSS in passing SYNs so TCP
# never sends a segment the tunnel can't carry (see overlays exercise 9).
ipt -t mangle -A FORWARD -o wg+ -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
ipt -t mangle -A FORWARD -i wg+ -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu

start_frr edge
idle
