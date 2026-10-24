#!/bin/bash
# remote: an employee working from home, on the public internet. Their WireGuard client
# is a *split tunnel*: only HQ's internal ranges go through the VPN, and everything else
# goes straight out their home connection.
set -euo pipefail
source /lab/lib.sh

wg_up wg0 10.255.2.10/24 - /state/wg/remote.key
wg set wg0 peer "$(cat /state/wg/edge-ra.pub)" endpoint "$EDGE_PUBLIC_IP:51821" \
  allowed-ips "$HQ_NET,$RA_NET" persistent-keepalive 25
ip route replace "$HQ_NET" dev wg0
idle
