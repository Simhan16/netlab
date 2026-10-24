#!/bin/bash
# Every container starts here. It turns a Docker container into a "real" box on the
# enterprise network, then hands over to its role script:
#   1. rename interfaces after the segment they're on (eth1 → users), using LINKS
#   2. replace Docker's default route with the enterprise gateway (GW)
#   3. replace Docker's embedded DNS with the enterprise DNS server (DNS)
#   4. exec /lab/roles/$ROLE.sh
# Safe to re-run: `docker restart <name>` re-applies everything from scratch.
set -euo pipefail
source /lab/lib.sh

: "${ROLE:?ROLE must be set}"
log "role $ROLE starting"

rename_links "${LINKS:-}"

# Docker points every container's default route at its own bridge gateway (the host,
# .254 here). A real host's gateway is its router, so replace it, or remove it for
# routers that learn their default from OSPF.
if [[ -n ${GW:-} ]]; then
  ip route replace default via "$GW"
else
  ip route del default 2>/dev/null || true
fi

# Docker's embedded resolver (127.0.0.11) only knows container names. Enterprise
# machines use the enterprise DNS server. /etc/resolv.conf is a writable bind mount.
if [[ -n ${DNS:-} ]]; then
  printf 'nameserver %s\nsearch corp.acme\noptions timeout:2 attempts:2\n' "$DNS" > /etc/resolv.conf
fi

exec "/lab/roles/$ROLE.sh"
