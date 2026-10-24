#!/usr/bin/env bash
# Remove everything setup.sh created. Safe to run repeatedly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

require_root
log "tearing down lab"

# Deleting a namespace destroys its interfaces; a veth pair dies with either end,
# which also removes h-up and the host routes that pointed through it.
for ns in "$LAN_A_NS" "$LAN_B_NS" "$ROUTER_NS"; do
  ip netns del "$ns" 2>/dev/null || true
  rm -rf "/etc/netns/$ns"
done

for rule in "${HOST_RULES[@]}"; do
  IFS='|' read -r table chain spec <<< "$rule"
  read -ra args <<< "$spec"
  while iptables -t "$table" -D "$chain" "${args[@]}" 2>/dev/null; do :; done
done

if [[ -f $STATE_DIR/ip_forward ]]; then
  sysctl -qw "net.ipv4.ip_forward=$(cat "$STATE_DIR/ip_forward")"
fi
rm -rf "$STATE_DIR"
