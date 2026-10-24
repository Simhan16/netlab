# Helpers shared by the entrypoint and the role scripts. Sourced, not executed.

# Enterprise address plan, used by the firewall and VPN scripts.
HQ_NET=10.10.0.0/16
USERS_NET=10.10.10.0/24
SERVERS_NET=10.10.20.0/24
MGMT_NET=10.10.99.0/24
BRANCH_NET=10.20.0.0/16
RA_NET=10.255.2.0/24          # remote-access VPN clients
DMZ_NET=172.16.10.0/24
DNS_IP=10.10.20.10
INTRANET_IP=10.10.20.20
JUMPBOX_IP=10.10.99.10
WWW_IP=172.16.10.10
PROXY_IP=172.16.10.20
EDGE_PUBLIC_IP=203.0.113.10
BRANCH_PUBLIC_IP=203.0.113.20

log() { printf '[%s] %s\n' "$(hostname)" "$*"; }
die() { log "error: $*"; exit 1; }

# rename_links "users=10.10.10.1 transit=10.255.0.2": find the interface holding each
# address and rename it, so configs and captures say `users`, not `eth2`. Docker
# numbers interfaces in no guaranteed order, but the addresses are fixed.
rename_links() {
  local pair name ip dev tries
  for pair in $1; do
    name=${pair%%=*}
    ip=${pair#*=}
    dev=""
    for ((tries = 0; tries < 50; tries++)); do
      dev=$(ip -o -4 addr show | awk -v ip="$ip" '{split($4, a, "/"); if (a[1] == ip) { sub(/@.*/, "", $2); print $2 }}')
      [[ -n $dev ]] && break
      sleep 0.2                                   # Docker may still be attaching networks
    done
    [[ -n $dev ]] || die "no interface has $ip (is the container on that network?)"
    if [[ $dev != "$name" ]]; then
      ip link set "$dev" down
      ip link set "$dev" name "$name"
      ip link set "$name" up
    fi
  done
}

# start_frr <name>: load /lab/config/frr/<name>.conf and start zebra + ospfd.
start_frr() {
  cp "/lab/config/frr/$1.conf" /etc/frr/frr.conf
  sed -i 's/^ospfd=no/ospfd=yes/' /etc/frr/daemons
  mkdir -p /run/frr /var/log/frr
  chown -R frr:frr /etc/frr /run/frr /var/log/frr
  /usr/lib/frr/frrinit.sh start >/dev/null
  log "FRR started (vtysh -c 'show ip ospf neighbor')"
}

# reset_firewall: start every role from an empty ruleset, so a restart is clean.
reset_firewall() {
  local table
  for table in filter nat mangle; do
    iptables -t "$table" -F
    iptables -t "$table" -X
  done
  iptables -P INPUT ACCEPT
  iptables -P FORWARD ACCEPT
  iptables -P OUTPUT ACCEPT
}

# wg_up <ifname> <address/prefix> <listen port|-> <private key file>, then wg_peer calls.
wg_up() {
  ip link del "$1" 2>/dev/null || true
  ip link add "$1" type wireguard
  if [[ $3 == - ]]; then
    wg set "$1" private-key "$4"
  else
    wg set "$1" listen-port "$3" private-key "$4"
  fi
  ip addr add "$2" dev "$1"
  ip link set "$1" up
}

# Keep PID 1 alive for roles whose daemons run in the background.
idle() { log "ready"; exec sleep infinity; }
