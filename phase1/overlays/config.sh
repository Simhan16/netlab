# Shared settings for the overlays lab. Sourced, not executed.
#
# Two independent topologies, both made of namespaces prefixed "ov-". Nothing
# touches the host's own interfaces, routes or firewall, and nothing reaches
# the internet, so this lab can run alongside netns-router.

# --- Part 1: one switch, two VLANs, a router on a stick ---------------------------------
SW_NS=ov-sw                         # a VLAN-aware Linux bridge: the "switch"
RTR_NS=ov-rtr                       # routes between the VLANs over one trunk link
VLAN_HOSTS=(ov-h1 ov-h2 ov-h3 ov-h4)
# host namespace → "vlan|address/prefix|gateway"
declare -A VLAN_HOST_CFG=(
  [ov-h1]="10|10.10.0.1/24|10.10.0.254"
  [ov-h2]="10|10.10.0.2/24|10.10.0.254"
  [ov-h3]="20|10.20.0.3/24|10.20.0.254"
  [ov-h4]="20|10.20.0.4/24|10.20.0.254"
)
RTR_VLAN10_IP=10.10.0.254/24
RTR_VLAN20_IP=10.20.0.254/24

# --- Part 2: two sites joined across an "internet" ----------------------------------------
WAN_NS=ov-wan                       # the internet: routes public addresses only
SITE_A_NS=ov-siteA                  # site A's edge router: a tunnel endpoint
SITE_B_NS=ov-siteB
HOST_A_NS=ov-hA                     # a host on site A's private LAN
HOST_B_NS=ov-hB
VX_A_NS=ov-vA                       # hosts on the VXLAN-stretched segment
VX_B_NS=ov-vB

# Underlay ("public") addressing, from the 198.51.100.0/24 documentation range.
SITE_A_WAN_IP=198.51.100.1
WAN_A_IP=198.51.100.2
SITE_B_WAN_IP=198.51.100.5
WAN_B_IP=198.51.100.6
WAN_PREFIX=30

# Private site LANs. The "internet" has no route to these.
SITE_A_LAN=10.1.0.0/24
SITE_A_GW=10.1.0.1
HOST_A_IP=10.1.0.10
SITE_B_LAN=10.2.0.0/24
SITE_B_GW=10.2.0.1
HOST_B_IP=10.2.0.10
LAN_PREFIX=24

# Inside the tunnels.
GRE_A_IP=10.255.0.1/30
GRE_B_IP=10.255.0.2/30
WG_A_IP=10.255.2.1/30
WG_B_IP=10.255.2.2/30
WG_PORT=51820
VXLAN_VNI=100
VXLAN_PORT=4789                     # the IANA port. Linux's own default (8472) predates it.
VX_A_IP=10.50.0.1/24                # vA and vB share one subnet, 2 sites apart
VX_B_IP=10.50.0.2/24

STATE_DIR=/run/overlay-lab          # tmpfs: the WireGuard keys never touch the disk
ALL_NS=("$SW_NS" "$RTR_NS" "${VLAN_HOSTS[@]}" "$WAN_NS" "$SITE_A_NS" "$SITE_B_NS"
        "$HOST_A_NS" "$HOST_B_NS" "$VX_A_NS" "$VX_B_NS")

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
require_root() { [[ $EUID -eq 0 ]] || die "run as root: sudo $0"; }
in_ns() { ip netns exec "$@"; }
lab_running() { [[ -e /run/netns/$SITE_A_NS ]]; }
