#!/bin/bash
# dns: the internal DNS server. Answers for corp.acme itself (and gives the internal
# answer for the public website: split-horizon DNS), and forwards everything else to
# public resolvers.
set -euo pipefail
source /lab/lib.sh
log "dnsmasq on $DNS_IP (queries are logged: docker logs -f dns)"
exec dnsmasq --keep-in-foreground --conf-file=/lab/config/dnsmasq.conf
