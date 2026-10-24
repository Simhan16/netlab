#!/usr/bin/env bash
# Drive the Acme Corp lab.
#
#   ./lab.sh up              build the image, make VPN keys, start every box
#   ./lab.sh test            check the whole policy matrix (test.sh)
#   ./lab.sh status          containers + OSPF neighbors
#   ./lab.sh shell ws1       a shell "on" any box
#   ./lab.sh ticket 1..5     inject a fault (see README: "Help desk tickets")
#   ./lab.sh fix             undo every ticket by restarting the affected boxes
#   ./lab.sh down            stop and remove everything (keeps the VPN keys)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

IMAGE=acme-netbox:lab
KEYS=(edge-site branch-site edge-ra remote)

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

preflight() {
  command -v docker >/dev/null || die "docker not found: sudo apt install -y docker.io docker-compose-v2"
  docker compose version >/dev/null 2>&1 || die "docker compose v2 not found: sudo apt install -y docker-compose-v2"
  docker info >/dev/null 2>&1 || die "can't talk to Docker: add yourself to the docker group (sudo usermod -aG docker \$USER, then log in again)"
  # Containers create WireGuard interfaces, but only the host kernel can load the module.
  if ! grep -q '^wireguard ' /proc/modules 2>/dev/null; then
    echo "loading the wireguard kernel module (needs sudo once per boot)"
    sudo modprobe wireguard || die "could not load the wireguard module"
  fi
}

keygen() {
  mkdir -p state/wg
  chmod 700 state state/wg
  local k
  for k in "${KEYS[@]}"; do
    if [[ ! -s state/wg/$k.key ]]; then
      (umask 077; docker run --rm "$IMAGE" wg genkey > "state/wg/$k.key")
    fi
    docker run --rm -i "$IMAGE" wg pubkey < "state/wg/$k.key" > "state/wg/$k.pub"
  done
}

case ${1:-help} in
  up)
    preflight
    docker build -t "$IMAGE" image
    keygen
    docker compose up -d
    echo
    echo "Acme Corp is starting. OSPF needs ~30 s to converge. Then: ./lab.sh test"
    ;;
  down)
    docker compose down
    ;;
  test)
    ./test.sh
    ;;
  status)
    docker compose ps
    echo
    docker exec edge-fw vtysh -c 'show ip ospf neighbor'
    ;;
  shell)
    [[ -n ${2:-} ]] || die "usage: $0 shell <container>   (e.g. ws1, core-rtr, edge-fw)"
    docker exec -it "$2" bash
    ;;
  ticket)
    case ${2:-} in
      1) docker exec core-rtr sh -c 'iptables -I FORWARD 1 -d 10.10.20.10 -p udp --dport 53 -j DROP;
                                     iptables -I FORWARD 1 -d 10.10.20.10 -p tcp --dport 53 -j DROP' ;;
      2) docker exec branch-fw vtysh -c 'configure terminal' -c 'interface wg-site' -c 'ip ospf hello-interval 10' ;;
      3) docker exec edge-fw iptables -t nat -F POSTROUTING ;;
      4) docker exec ws2 ip route replace default via 10.10.10.99 ;;
      5) docker stop proxy >/dev/null ;;
      *) die "usage: $0 ticket 1|2|3|4|5" ;;
    esac
    echo "ticket ${2} is live. Read it in README.md and start troubleshooting. Undo with: ./lab.sh fix"
    ;;
  fix)
    docker start proxy >/dev/null
    docker restart edge-fw core-rtr branch-fw ws2 >/dev/null
    echo "restored. OSPF needs ~30 s to reconverge; then ./lab.sh test"
    ;;
  *)
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
    ;;
esac
