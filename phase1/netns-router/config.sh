# Shared settings for setup.sh, teardown.sh and test.sh. Sourced, not executed.

LAN_A_NS=lan-a     # "trusted" LAN: may reach anything
LAN_B_NS=lan-b     # "guest" LAN: internet only, cannot initiate to lan-a
ROUTER_NS=router   # forwards between LANs, enforces the firewall

# 192.168.x avoids overlapping common AWS VPC ranges (172.31/16 default, 10.x custom).
LAN_PREFIX=24
LAN_A_NET=192.168.10.0/24
LAN_A_GW=192.168.10.1
LAN_A_HOST=192.168.10.10
LAN_B_NET=192.168.20.0/24
LAN_B_GW=192.168.20.1
LAN_B_HOST=192.168.20.10

# Point-to-point /30 link between the host and the router namespace.
UPLINK_PREFIX=30
UPLINK_HOST_IP=192.168.255.1
UPLINK_ROUTER_IP=192.168.255.2

LAB_SUPERNET=192.168.0.0/16   # everything the host should NAT
DNS_SERVER=1.1.1.1
PING_TARGET=1.1.1.1
STATE_DIR=/run/netns-lab

# The host's real internet-facing interface (ens5 on most EC2 instances).
# Override with: sudo UPLINK_IF=eth0 ./setup.sh
UPLINK_IF="${UPLINK_IF:-$(ip -4 route show default | awk '{for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit }}')}"

# Host-side iptables rules as "table|chain|rule". Setup inserts them, teardown
# deletes the identical specs, so the two can never drift apart. Only NAT and
# FORWARD are touched -- never INPUT -- so your SSH session is safe.
HOST_RULES=(
  "nat|POSTROUTING|-s $LAB_SUPERNET -o $UPLINK_IF -m comment --comment netns-lab -j MASQUERADE"
  "filter|FORWARD|-i h-up -o $UPLINK_IF -m comment --comment netns-lab -j ACCEPT"
  "filter|FORWARD|-i $UPLINK_IF -o h-up -m conntrack --ctstate ESTABLISHED,RELATED -m comment --comment netns-lab -j ACCEPT"
)

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
require_root() { [[ $EUID -eq 0 ]] || die "run as root: sudo $0"; }
