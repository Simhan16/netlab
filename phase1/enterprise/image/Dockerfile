# One image for every box in the lab: routers, firewalls, servers and workstations.
# What each container *does* comes from its ROLE (see node/roles/), not from the image,
# and the scripts are bind-mounted, so editing them needs a restart, not a rebuild.
FROM ubuntu:24.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      iproute2 iputils-ping traceroute mtr-tiny tcpdump iptables conntrack \
      wireguard-tools frr dnsutils curl netcat-openbsd \
      dnsmasq-base squid nginx \
      ca-certificates procps less vim-tiny \
 && rm -rf /var/lib/apt/lists/*
