#!/bin/bash
# web: an nginx web server. SITE=intranet serves the intranet on port 80 plus an
# "admin console" on 8080 that only IT should reach. SITE=public is the company website in the DMZ.
set -euo pipefail
source /lab/lib.sh
: "${SITE:?SITE must be intranet or public}"

{
  echo "server { listen 80 default_server; root /lab/www/$SITE; access_log /dev/stdout; }"
  if [[ $SITE == intranet ]]; then
    echo "server { listen 8080; root /lab/www/admin; access_log /dev/stdout; }"
  fi
} > /etc/nginx/sites-enabled/default

log "nginx serving $SITE (requests are logged: docker logs -f $(hostname))"
exec nginx -g 'daemon off;'
