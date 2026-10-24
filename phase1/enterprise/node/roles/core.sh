#!/bin/bash
# core-rtr: routes between HQ's internal segments and enforces "east-west" policy
# (who inside may talk to whom). Everything leaving HQ goes to edge-fw over transit.
#
#   users 10.10.10.1 ──┐
#   servers 10.10.20.1 ┼── core-rtr ── transit 10.255.0.2 ── edge-fw
#   mgmt 10.10.99.1 ───┘
set -euo pipefail
source /lab/lib.sh

log "segmentation firewall"
reset_firewall
ipt() { iptables "$@"; }

ipt -P FORWARD DROP
ipt -A FORWARD -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
ipt -A FORWARD -m conntrack --ctstate INVALID -j DROP
ipt -A FORWARD -s "$MGMT_NET" -m comment --comment "IT admins reach everything" -j ACCEPT
ipt -A FORWARD -p icmp -m comment --comment "ping for troubleshooting" -j ACCEPT
ipt -A FORWARD -d "$DNS_IP" -p udp --dport 53 -m comment --comment "anyone -> DNS" -j ACCEPT
ipt -A FORWARD -d "$DNS_IP" -p tcp --dport 53 -m comment --comment "anyone -> DNS" -j ACCEPT
ipt -A FORWARD -d "$INTRANET_IP" -p tcp -m multiport --dports 80,443 -m comment --comment "anyone -> intranet web" -j ACCEPT
ipt -A FORWARD -i users -o transit -m comment --comment "users -> beyond HQ (edge-fw decides)" -j ACCEPT
ipt -A FORWARD -i servers -o transit -m comment --comment "servers -> beyond HQ (edge-fw decides)" -j ACCEPT
ipt -A FORWARD -m comment --comment "default deny" -j DROP

start_frr core
idle
