#!/bin/bash
# proxy: the Squid web proxy in the DMZ. Staff browse the web only through it: the
# edge firewall blocks their direct web traffic, and the proxy enforces URL policy and
# logs every request.
set -euo pipefail
source /lab/lib.sh
mkdir -p /var/log/squid /var/spool/squid
touch /var/log/squid/access.log
chown -R proxy:proxy /var/log/squid /var/spool/squid
rm -f /run/squid.pid
log "squid on $PROXY_IP:3128 (requests: docker exec proxy tail -f /var/log/squid/access.log)"
exec squid -N -d1 -f /lab/config/squid.conf
