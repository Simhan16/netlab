#!/usr/bin/env bash
# Remove everything setup.sh and tunnel.sh created. Safe to run repeatedly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/config.sh"

require_root
log "tearing down overlays lab"
# Deleting a namespace destroys every interface, bridge, tunnel, route and firewall
# rule inside it. The lab created nothing outside its namespaces.
for ns in "${ALL_NS[@]}"; do
  ip netns del "$ns" 2>/dev/null || true
done
rm -rf "$STATE_DIR"
