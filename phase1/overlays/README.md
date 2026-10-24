# overlays: VLANs, bridges and tunnels

Phase 1.6, the advanced chapter. So far every network in this repo has been "real": one
cable, one subnet, one router. Production networks are mostly **virtual networks built on top
of other networks**:
- Office switches carry many separate networks over the same cables (**VLANs**).
- Companies join sites across the internet as if they were one LAN (**GRE, WireGuard** and other VPNs).
- Cloud VPCs and Kubernetes give every VM or pod its own network that floats on top of the
  datacenter's (**VXLAN** and similar overlays).

They all rest on one idea, **encapsulation**: wrap a packet (or a whole frame) inside another
one, carry it across a network that knows nothing about the inner addresses, and unwrap it at
the far end. You already know the cost of it from netns-router exercise 4: **every wrapper eats
MTU**, and overlay MTU problems are some of the most common tickets in cloud networking.

**Before this:** tcpdump-basics (addressing, `tcpdump -e`) and netns-router (namespaces, veth, routing).
**Runs on:** the Ubuntu EC2 box, as root. Like netns-router, but this lab is **fully isolated**:
its namespaces start with `ov-`, it changes nothing on the host (no routes, no iptables), never
touches the internet, and can run alongside netns-router.

## The two topologies
```
Part 1: one switch, two VLANs, a router on a stick

  h1 10.10.0.1 ──┐ access port, VLAN 10
  h2 10.10.0.2 ──┤
                 ├── sw (bridge br0, vlan_filtering) ══ trunk: VLANs 10+20 tagged ══ rtr
  h3 10.20.0.3 ──┤                                                    rtr-eth0.10 10.10.0.254
  h4 10.20.0.4 ──┘ access port, VLAN 20                               rtr-eth0.20 10.20.0.254

Part 2: two sites joined across an "internet" that can't route their private addresses

  site A                         the internet ("underlay")                        site B
  hA 10.1.0.10 ── siteA ── 198.51.100.1 ═══ wan ═══ 198.51.100.5 ── siteB ── hB 10.2.0.10
  vA 10.50.0.1 ── br100                                                br100 ── vB 10.50.0.2
                    └────── tunnel: gre | vxlan | wireguard, over the wan ──────┘
```
The wan namespace forwards packets between the two public /30s and nothing else. It has no
route to `10.x`, so without a tunnel the sites can't reach each other.

| Mode (`./tunnel.sh`) | Layer | Wraps | Overhead | Encrypted | Joins |
|---|---|---|---|---|---|
| `none` | n/a | n/a | n/a | n/a | nothing |
| `gre` | 3 (routed) | IP packet in IP (protocol 47) | 24 bytes → MTU 1476 | no | hA ↔ hB |
| `vxlan` | 2 (bridged) | Ethernet frame in UDP 4789 | 50 bytes → MTU 1450 | no | vA ↔ vB, same subnet |
| `wireguard` | 3 (routed) | IP packet in UDP 51820, encrypted | 60 bytes (MTU 1420 by default) | yes | hA ↔ hB |

## Run
```bash
sudo apt install -y iproute2 iptables tcpdump wireguard-tools curl python3
cd phase1/overlays
sudo ./setup.sh               # both topologies, no tunnel yet
sudo ./test.sh                # checks part 1, plus part 2 for the current tunnel mode
sudo ./tunnel.sh gre          # or vxlan, wireguard, none
sudo ./test.sh
sudo ./teardown.sh            # removes every ov-* namespace; nothing else to clean up
```
Get a shell inside any "machine" with `sudo ip netns exec ov-h1 bash` (the prompt doesn't
change, so `ip -br addr` tells you where you are).

## Sample output
<!-- Paste real test.sh output for each tunnel mode here, from your instance. -->
```
[paste test.sh output: none, gre, vxlan, wireguard]
```

---

## Concepts
### VLANs: many networks on one switch
A plain switch is one **broadcast domain**: an ARP request reaches every port. A **VLAN**
splits a switch into several independent switches in software. Each port belongs to a VLAN
(a number, 1-4094), and frames never cross from one VLAN to another. Crossing needs a router,
exactly as between two physical switches.
- An **access port** carries one VLAN, untagged. The host plugged into it has no idea VLANs
  exist: the switch adds the VLAN on the way in and removes it on the way out.
- A **trunk port** carries many VLANs over one link, typically between switches or to a router.
  Each frame gets a 4-byte **802.1Q tag** after the MAC addresses, holding the VLAN ID, so the far end can tell them apart.
- **Router on a stick:** one router interface on a trunk, with one *subinterface* per VLAN
  (`rtr-eth0.10`, `rtr-eth0.20`), each the gateway for its VLAN's subnet.

The usual rule is **one VLAN = one subnet = one broadcast domain**. Guest Wi-Fi, phones, cameras and
servers each get their own, so a broadcast storm or a compromised camera stays in its box.

### Bridges: a switch in the kernel
A Linux **bridge** (`br0`) is a software switch. It learns which MAC address lives behind which
port by watching source addresses (the **forwarding database**, or FDB). It sends a frame only to
the right port once it knows, and **floods** it to all ports until then. Docker's `docker0`,
libvirt's `virbr0` and Kubernetes CNI bridges are all this.

### Tunnels: encapsulation
```
GRE:        [outer IP 20][GRE 4][inner IP packet ...............]            Layer 3 over IP
VXLAN:      [outer IP 20][UDP 8][VXLAN 8][inner Ethernet 14][inner IP ...]   Layer 2 over UDP
WireGuard:  [outer IP 20][UDP 8][WG header 16][encrypted inner IP ...][tag 16]
```
- **The underlay** is the network the tunnel runs over (here, the wan). It only ever sees the
  outer header: tunnel endpoint to tunnel endpoint.
- **The overlay** is the network inside the tunnel. Its addresses (10.x) mean nothing to the underlay.
- **A tunnel endpoint** (here siteA/siteB; in VXLAN terms a **VTEP**) wraps and unwraps.
  A tunnel is just a network interface, so "send this subnet through the tunnel" is an ordinary route.
- **Layer 3 or Layer 2?** GRE and WireGuard carry IP packets, so the two sites are different
  subnets joined by routing. VXLAN carries Ethernet frames, so the two sites are *one* subnet,
  with ARP and broadcasts crossing the tunnel. That's what lets a VM keep its IP when it moves
  to another rack, and what lets a Kubernetes pod network span many nodes.
- **MTU:** the underlay link is 1500 bytes, and the wrapper takes its share, so the overlay gets less.
  If the overlay's MTU isn't lowered to match, big packets get dropped. Sometimes there's an ICMP
  error saying so, and sometimes (the black-hole case) nothing at all.

---

## Exercises
Run from `phase1/overlays` with the lab up (`sudo ./setup.sh`). Exercises 1-4 use part 1, and 5-9 use
part 2 with the tunnel mode each one names. `sudo ./setup.sh` resets everything if you break the lab.

### 1. Access ports hide VLANs, trunks show them
**Concept:** the VLAN tag exists only between switches (and router). Hosts send and
receive ordinary untagged frames.

**Try it:**
```bash
sudo ip netns exec ov-rtr tcpdump -eni rtr-eth0               # terminal 1: the trunk
sudo ip netns exec ov-h1 tcpdump -eni h1-eth0                 # terminal 2: h1's own link
sudo ip netns exec ov-h1 ip neigh flush all                   # terminal 3: forget cached MACs...
sudo ip netns exec ov-h1 ping -c2 10.10.0.2                   # ...then ping h2, in the same VLAN
```
**What you should see:** on the trunk, only h1's **ARP request**, shown as `ethertype 802.1Q (0x8100)`
followed by `vlan 10`. The pings themselves don't appear on the trunk at all. On `h1-eth0` the
same ARP and pings appear with **no** `vlan` field.

**Why:** the ARP request is a broadcast, so the switch floods it to every port in VLAN 10,
and the trunk is one of them. It tags the copy, so the router knows which VLAN it came from. The pings are
unicast between h1 and h2, and the switch has learned both MACs, so it sends them only between
those two ports. The access port strips the tag before anything reaches h1.

**Check yourself:** h3 (VLAN 20) is plugged into the same switch. Does it ever see h1's ARP broadcast?
<details><summary>Answer</summary>No. Flooding stays inside the VLAN, so ARP from VLAN 10 reaches only VLAN 10
ports (h2 and the trunk). That isolation is the whole point: the same switch, but separate broadcast domains.</details>

### 2. Router on a stick: every inter-VLAN packet crosses the trunk twice
**Concept:** to get from VLAN 10 to VLAN 20, a packet goes up the trunk to the router and
straight back down the same trunk in the other VLAN.

**Try it:**
```bash
sudo ip netns exec ov-rtr tcpdump -eni rtr-eth0 icmp           # terminal 1
sudo ip netns exec ov-h1 ping -c1 10.20.0.3                     # terminal 2: h1 (VLAN 10) to h3 (VLAN 20)
```
**What you should see:** four lines for one ping. The request appears tagged `vlan 10`
(h1 → router), then the same request tagged `vlan 20` (router → h3). The reply is the same
in reverse. Also compare the TTLs with `-v`: the copy in VLAN 20 has TTL 63, because it was routed.

**Why:** the router receives on `rtr-eth0.10`, routes (decrementing the TTL), and sends out
`rtr-eth0.20`. Both subinterfaces are the same physical port, so the trunk carries the traffic
twice. On a busy network that hairpin is a bottleneck, which is why real campuses use
**Layer 3 switches** that route between VLANs internally.

**Check yourself:** `test.sh` checks that `ping -t1` (TTL 1) works from h1 to h2 but fails from
h1 to h3. Explain both results.
<details><summary>Answer</summary>Within a VLAN there's no router: the switch forwards frames without
touching the IP header, so TTL 1 is enough. To reach h3 the packet must be routed, and the router
decrements TTL 1 to 0, drops it, and sends back "time exceeded". That's how traceroute discovers hops.</details>

### 3. How a switch learns
**Concept:** a bridge's forwarding database maps (MAC address, VLAN) to a port. It's learned from
the *source* address of every frame and forgotten after 300 s of silence.

**Try it:**
```bash
sudo ip netns exec ov-sw bridge fdb show br br0 | grep -v permanent
for h in 1 2 3 4; do sudo ip netns exec ov-h$h ip -br link show h$h-eth0; done   # each host's MAC
sudo ip netns exec ov-h4 ping -c1 10.20.0.3
sudo ip netns exec ov-sw bridge fdb show br br0 | grep -v permanent
```
**What you should see:** entries like `<mac> dev sw-h4 vlan 20 master br0`, one per host that has
sent anything, each tagged with its VLAN. The router's MAC appears on `sw-rtr` **twice**, once in
VLAN 10 and once in VLAN 20.

**Why:** the FDB is per VLAN, because the same MAC may legitimately appear in several VLANs.
The router uses one MAC on both subinterfaces. Until a MAC is learned, frames to it are flooded
(in that VLAN only), which is also why an attacker who fills the FDB with fake MACs turns a switch
into a hub ("MAC flooding").

**Check yourself:** why is the switch learning from *source* addresses, not destinations?
<details><summary>Answer</summary>A frame's source address proves where that MAC is: it just came in on
that port. The destination says nothing about location. That's also why the first frame to an
unknown MAC must be flooded, and why the reply (whose source is the unknown host) fixes it.</details>

### 4. Wrong VLAN, right IP: the classic misconfiguration
**Concept:** Layer 2 and Layer 3 have to agree. A host in the wrong VLAN keeps its IP address,
but nobody in its new broadcast domain shares its subnet.

**Try it:** move h3's port into VLAN 10 without touching h3 itself.
```bash
sudo ip netns exec ov-sw bridge vlan del dev sw-h3 vid 20
sudo ip netns exec ov-sw bridge vlan add dev sw-h3 vid 10 pvid untagged
sudo ip netns exec ov-rtr tcpdump -eni rtr-eth0 'arp or icmp'  # terminal 2
sudo ip netns exec ov-h3 ping -c2 -W2 10.20.0.4                # its old VLAN-mate
sudo ip netns exec ov-h3 ping -c2 -W2 10.10.0.1                # its new VLAN-mate
```
**What you should see:** both pings fail, in two different ways:
- **To 10.20.0.4:** `vlan 10` ARP requests for 10.20.0.4, and no reply.
- **To 10.10.0.1:** h3 ARPs for its gateway 10.20.0.254 *in VLAN 10*, and the router **does** answer.
  The echo request goes up the trunk and back down to h1, and h1 replies. The reply then leaves the router
  in `vlan 20`, as an ARP request for 10.20.0.3 that nobody answers.

(Put it back afterwards: swap `10` and `20` in the two `bridge vlan` commands.)

**Why:** h3 still believes it's on 10.20.0.0/24.
- **10.20.0.4** is in its subnet, so h3 ARPs for it directly. But that host is in VLAN 20, so the
  broadcast never reaches it.
- **10.10.0.1** isn't in its subnet, so h3 sends to its gateway. Linux answers ARP for *any* of its
  addresses on *any* interface by default (`arp_ignore=0`, the "weak host" model), so the router replies
  on `rtr-eth0.10` and the request gets through. But the router routes the reply by its destination,
  10.20.0.3, out of the VLAN 20 subinterface, where h3 no longer is. Half of the conversation works,
  which is exactly what makes VLAN mistakes confusing.

**Check yourself:** on a real network, a user moves desks and "the internet stopped working",
but the link light is on and their IP looks normal. What do you check first?
<details><summary>Answer</summary>The VLAN of the new switch port, against the subnet of their address
(or whether DHCP gave them a new one). Link up plus a correct-looking IP, with no ARP replies from the
gateway, is the signature of a wrong VLAN.</details>

### 5. Why private networks need tunnels (`./tunnel.sh none`)
**Concept:** private addresses (10/8, 172.16/12, 192.168/16) aren't routed on the internet.
Two sites using them can't reach each other directly, whatever their own routers do.

**Try it:**
```bash
sudo ./tunnel.sh none
sudo ip netns exec ov-hA ping -c2 10.2.0.10
sudo ip netns exec ov-wan ip route
```
**What you should see:** the pings simply time out, with no error message. The wan's routing table
has only its two connected /30s, and `sudo ip netns exec ov-wan ip route get 10.1.0.10` answers
`Network is unreachable`.

**Why:** siteA sends everything it doesn't know to its default route, the wan. The wan has no route for
10.2.0.0/24, so it drops the packet. It can't even send an ICMP "net unreachable" back, because it has no
route to 10.1.0.10 either. The internet only routes publicly registered address space. NAT works for
*outgoing* connections (netns-router), but nothing on the internet can reach *into* 10.2.0.0/24.

**Check yourself:** could you fix this by adding `ip route add 10.2.0.0/24 via 198.51.100.6` on the
wan? Why doesn't that work on the real internet?
<details><summary>Answer</summary>In this lab, yes: you own the wan. On the internet, you'd need every
ISP between the sites to carry your private route, and thousands of companies use the same 10.x ranges,
so the routes would collide. A tunnel needs only the two endpoints to cooperate, and the underlay only
ever sees their public addresses.</details>

### 6. GRE: a packet inside a packet (`./tunnel.sh gre`)
**Concept:** GRE puts the whole inner IP packet behind a new outer IP header (protocol 47)
and a 4-byte GRE header. The underlay routes it by the outer header only.

**Try it:**
```bash
sudo ./tunnel.sh gre && sudo ./test.sh
sudo ip netns exec ov-wan tcpdump -ni wan-A                     # terminal 1: what the internet sees
sudo ip netns exec ov-hA ping -c2 10.2.0.10                     # terminal 2
sudo ip netns exec ov-hA ping -c2 -M do -s 1448 10.2.0.10       # 1448 + 8 ICMP + 20 IP = 1476
sudo ip netns exec ov-hA ping -c2 -M do -s 1449 10.2.0.10       # one byte too many
```
**What you should see:** in the wan capture, lines like
`IP 198.51.100.1 > 198.51.100.5: GREv0, length 88: IP 10.1.0.10 > 10.2.0.10: ICMP echo request`:
two IP headers, with the inner one fully readable. The 1448 ping works. The 1449 ping gets
`Frag needed and DF set (mtu = 1476)` from siteA (10.1.0.1), and then `message too long`.

**Why:** `length 88` is the 84-byte inner packet plus the 4-byte GRE header. The outer 20-byte IP
header brings it to 108 on the wire, which is why the tunnel's MTU is 1500 − 24 = 1476. GRE adds no
encryption: anyone on the path reads the inner traffic.

**Check yourself:** GRE is IP protocol 47, with no ports. Why does that make GRE awkward behind NAT,
and how do the UDP-based tunnels avoid it?
<details><summary>Answer</summary>NAT tells connections apart by port numbers, and GRE has none, so a NAT
device can't map more than one GRE tunnel per public address (and many simply drop protocol 47).
VXLAN and WireGuard run over UDP, which every NAT and firewall already knows how to handle.</details>

### 7. VXLAN: one subnet, two sites, and a silent MTU drop (`./tunnel.sh vxlan`)
**Concept:** VXLAN carries Ethernet frames inside UDP. Bridged at both ends, it makes vA and
vB neighbors on 10.50.0.0/24, ARPing each other through the wan. Each tunnel has a 24-bit
**VNI** (here 100), so one underlay can carry 16 million separate overlays, against VLANs' 4094.

**Try it:**
```bash
sudo ./tunnel.sh vxlan && sudo ./test.sh
sudo ip netns exec ov-wan tcpdump -ni wan-A udp port 4789          # terminal 1
sudo ip netns exec ov-vA ping -c2 10.50.0.2                          # terminal 2
sudo ip netns exec ov-siteA bridge fdb show dev vxlan100              # which MAC lives behind which remote VTEP
sudo ip netns exec ov-vA ping -c2 -W2 -M do -s 1422 10.50.0.2        # 1450 bytes: fits
sudo ip netns exec ov-vA ping -c2 -W2 -M do -s 1423 10.50.0.2        # 1451 bytes: what happens?
```
**What you should see:**
- The wan shows `VXLAN, flags [I] (0x08), vni 100` and then a complete inner frame: an **ARP**
  request for 10.50.0.2 travels inside UDP. The FDB shows vB's MAC with `dst 198.51.100.5`: the bridge
  learned that this MAC lives behind the far VTEP.
- The 1422 ping works. The 1423 ping **just times out**, with no error message at all.

**Why:** the 1451-byte packet leaves vA (its MTU is 1500), reaches br100, and is too big for
vxlan100 (MTU 1450). A **bridge doesn't route**, so it can't send an ICMP "too big" message. It
silently drops the frame. This is the classic overlay black hole, and the reason Kubernetes CNIs
like flannel set the pod MTU to 1450. Fix it the same way:
`sudo ip netns exec ov-vA ip link set vA-eth0 mtu 1450` (and on vB). Now the 1423 ping fails at once with
`message too long`, locally, which is an error you can see and that TCP adapts to.

**Check yourself:** why is VXLAN carried in UDP rather than directly in IP like GRE, and why does the
sending VTEP pick a *different source port* for different inner flows?
<details><summary>Answer</summary>UDP passes through NATs and firewalls, and routers that balance load
across several paths (ECMP) hash on the outer IPs and ports. The VTEP derives the UDP source port from a
hash of the inner flow, so different inner connections spread across the paths, while each connection
stays on one path (no reordering). Also: on AWS the VPC MTU is 9001, so a VXLAN overlay there loses
nothing noticeable. Most "1450" problems come from overlays on 1500-byte networks.</details>

### 8. WireGuard: the same tunnel, encrypted (`./tunnel.sh wireguard`)
**Concept:** WireGuard encrypts each inner packet and sends it over UDP. Peers are identified by
public keys, and each peer's **allowed-ips** is both its routing table entry and its access
list: a packet decrypted from a peer is dropped unless its source is in that peer's allowed-ips.

**Try it:**
```bash
sudo ./tunnel.sh wireguard && sudo ./test.sh
sudo ip netns exec ov-wan tcpdump -ni wan-A -X udp port 51820    # terminal 1
sudo ip netns exec ov-hA ping -c2 10.2.0.10                       # terminal 2
sudo ip netns exec ov-siteA wg show                                 # keys, endpoint, latest handshake, bytes
# Now shrink siteB's view of site A to just the tunnel address:
sudo ip netns exec ov-siteB wg set wg0 peer "$(sudo cat /run/overlay-lab/wg-a.pub)" allowed-ips 10.255.2.1/32
sudo ip netns exec ov-hA ping -c2 -W2 10.2.0.10
```
**What you should see:** UDP between 198.51.100.1 and .5 on port 51820, with unreadable payloads.
The first exchange is the **handshake**: a 148-byte initiation and a 92-byte response, fixed sizes in
the protocol. Each ping then rides in a `length 128` datagram: the 84-byte packet, padded to 96, plus
32 bytes of WireGuard overhead. Nothing says ICMP, and no 10.x address appears anywhere. After the
`allowed-ips` change, pings fail although the tunnel and its handshake are fine.

**Why:** siteB still decrypts hA's packets, but their source (10.1.0.10) is no longer in site A's
allowed-ips, so it drops them. This is **cryptokey routing**: the key decides which addresses a peer may
use. GRE would have accepted a spoofed inner source without question. Put it back with
`sudo ./tunnel.sh wireguard` (it also generates new keys).

**Check yourself:** WireGuard's MTU is 1420 here, though its IPv4 overhead is only 60 bytes
(1500 − 60 = 1440). Why the extra 20?
<details><summary>Answer</summary>The default assumes the worst-case underlay, IPv6, whose header is 40
bytes rather than 20: 1500 − 40 − 8 (UDP) − 32 (WireGuard) = 1420. One safe default saves you from
having to know which IP version the far end connects over (and roaming peers can switch).</details>

### 9. The tunnel MTU black hole, and MSS clamping (`./tunnel.sh gre`)
**Concept:** Path MTU Discovery depends on ICMP "fragmentation needed" messages getting back
to the sender. Firewalls that drop "all ICMP" break it. Then a connection's handshake works, small
requests work, and the first full-size data packet vanishes, along with every retransmission.
**MSS clamping**, where the tunnel router rewrites the MSS option in passing SYNs, avoids needing
PMTUD for TCP at all.

**Try it:** start from a fresh lab (`sudo ./setup.sh`). A host that has already downloaded through the
tunnel remembers the smaller path MTU for about 10 minutes, which would hide the black hole.
```bash
sudo ./setup.sh && sudo ./tunnel.sh gre
# Terminal 1: a web server on hB with a 2 MB file
sudo ip netns exec ov-hB sh -c 'mkdir -p /tmp/ovweb && head -c 2000000 /dev/urandom > /tmp/ovweb/big.bin && cd /tmp/ovweb && exec python3 -m http.server 8080'
# Terminal 2:
get() { sudo ip netns exec ov-hA curl -s -o /dev/null --max-time 8 -w '%{size_download} bytes, %{time_total} s\n' http://10.2.0.10:8080/big.bin; }
sudo ip netns exec ov-siteB iptables -A OUTPUT -p icmp --icmp-type fragmentation-needed -j DROP
get                                                                     # a. ?
sudo ip netns exec ov-siteA iptables -t mangle -A FORWARD -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
get                                                                     # b. ?
sudo ip netns exec ov-hB tcpdump -ni hB-eth0 'tcp[tcpflags] & tcp-syn != 0'   # terminal 3, then run get again
sudo ip netns exec ov-siteA iptables -t mangle -F FORWARD               # c. remove the clamp...
sudo ip netns exec ov-siteB iptables -D OUTPUT -p icmp --icmp-type fragmentation-needed -j DROP   # ...and the ICMP block
get                                                                     # d. ?
```
**What you should see:**
- **a:** curl gives up after 8 s, with almost nothing downloaded.
- **b:** 2000000 bytes in well under a second, with the ICMP still blocked. Terminal 3 shows hA's SYN
  arriving with `mss 1436`, not 1460.
- **d:** 2000000 bytes again, with no clamp. PMTUD works once its ICMP gets through.

**Why:** in **a**, hA's SYN says MSS 1460 (its own MTU is 1500), so hB sends 1500-byte segments
with DF set. siteB can't fit them into gre1 (1476), and its "fragmentation needed" reply is dropped,
so hB never learns. It retransmits the same oversized segment until curl gives up. The handshake and the
HTTP request are small, so they get through, which is what makes this failure so confusing in real life.
In **b**, siteA rewrites the MSS in hA's SYN as it heads into the tunnel (1476 − 40 = 1436), so hB never
sends anything too big. In **d**, siteB's ICMP reaches hB, which lowers its path MTU to 1476 and resends.
This is the same failure as casebook case 03, and the reason almost every VPN router clamps MSS by default.

**Check yourself:** MSS clamping fixed TCP. What happens to a big UDP-based transfer (or
QUIC/HTTP3) through the same tunnel?
<details><summary>Answer</summary>Nothing fixes it. MSS is a TCP option, and UDP has no handshake for a
router to rewrite. UDP applications rely on PMTUD or keep their packets small: QUIC starts at 1200 bytes and
probes upward itself (DPLPMTUD). That's why "allow ICMP fragmentation-needed" belongs in every firewall
policy. Run <code>sudo ./setup.sh</code> to remove the rules from this exercise.</details>

---

## Known limitations
- Written against Ubuntu 22.04/24.04 kernels (GRE, VXLAN and WireGuard are all built in). Not yet run end to end: paste `test.sh` output above once you have.
- IPv4 only, static unicast VXLAN (one remote VTEP). Real deployments add multicast or an EVPN/BGP control plane so VTEPs discover each other.
- No 802.1Q-in-802.1Q (QinQ), no STP. There are no loops to protect against in this topology.
- The tcpdump lines above are what to look for, not verbatim output: exact formatting varies with the tcpdump version.
