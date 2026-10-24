# netns-router

Two isolated LANs and a router with a stateful firewall, all inside one Linux box using
network namespaces, NATed out to the internet. It's a home/office network you can build,
break and capture with `tcpdump` in seconds, with no extra VMs.

```
  lan-a (trusted)                router ns                    host (EC2)
  192.168.10.10 ── a-eth0 ═ r-a ─┐ 192.168.10.1
                                 ├─ FORWARD firewall
  192.168.20.10 ── b-eth0 ═ r-b ─┘ 192.168.20.1
  lan-b (guest)                  r-up 192.168.255.2 ═ h-up 192.168.255.1 ── NAT ── ens5 ── internet
```
`═` = veth pair (virtual cable)

| Policy | Result |
|---|---|
| lan-a → anywhere | allowed |
| lan-b → internet | allowed |
| lan-b → lan-a (new connections) | **dropped** |
| replies to allowed connections | allowed (conntrack ESTABLISHED) |
| internet → LANs (new connections) | dropped |

## Concepts this teaches
- **Network namespaces:** each one is an independent network stack, effectively a separate machine.
- **veth pairs:** virtual Ethernet cables between namespaces.
- **Routing:** default gateways, plus the *return route* on the host (the classic reason "packets go out but never come back").
- **IP forwarding:** Linux only routes packets once `net.ipv4.ip_forward=1`, and that setting is per namespace.
- **Stateful firewalling:** `conntrack` lets replies through without opening the reverse direction.
- **NAT (MASQUERADE):** private source addresses are rewritten to the instance's IP on egress.

## Requirements
Ubuntu 22.04/24.04 (tested target: an EC2 instance) with `iproute2`, `iptables`, `curl` and root.
No AWS changes are needed: NAT makes all traffic leave with the instance's own IP, so the
source/dest check can stay on.

## Run
```bash
sudo ./setup.sh      # build (re-runnable; tears down first)
sudo ./test.sh       # 8 checks + firewall counters
sudo ./teardown.sh   # remove everything, restore ip_forward
```
If uplink detection picks the wrong interface: `sudo UPLINK_IF=ens5 ./setup.sh`.

Poke around by hand:
```bash
sudo ip netns exec lan-a bash        # a shell "inside" lan-a
ip addr; ip route; ping 192.168.20.10
sudo ip netns exec router tcpdump -ni r-b icmp   # watch guest traffic hit the router
```

## Sample output
<!-- Paste the real output of `sudo ./test.sh` from your instance here. -->
```
[paste test.sh output]
```

## Exercises
Run these from `phase1/netns-router` with the lab up (`sudo ./setup.sh`). Each one uses
`tcpdump` to make an invisible mechanism visible, which is also good practice for Phase 1.4.
Open a second SSH session so you can capture in one and generate traffic in the other.

### 1. A stateful firewall drop, seen from both sides
**Concept:** a firewall rule acts on one *direction* of a conversation. The router allows
lan-a → lan-b but drops lan-b → lan-a, unless the packet belongs to a conversation lan-a
started. Linux tracks conversations in **conntrack**: the first packet is `NEW`, and its
replies are `ESTABLISHED`.

**Try it:**
```bash
sudo ip netns exec router tcpdump -ni r-b icmp            # terminal 1: the router's guest-side port
sudo ip netns exec lan-b ping -c 3 192.168.10.10          # terminal 2: guest → trusted (blocked)
sudo ip netns exec lan-a ping -c 3 192.168.20.10          # terminal 2: trusted → guest (allowed)
sudo ip netns exec router iptables -L FORWARD -v -n       # packet counters per rule
```
**What you should see:** in the first ping, echo *requests* arrive on `r-b` and nothing
goes back. In the second, requests leave through `r-b` toward lan-b and the replies come
back in through it. The `guest to trusted blocked` rule's counter rises by 3, and the
`ESTABLISHED,RELATED` counter rises during the second ping.

**Why:** the lan-b echo reply in the second ping matches the conntrack entry created by
lan-a's request, so the first rule (`ESTABLISHED,RELATED → ACCEPT`) lets it through before
the drop rule is ever checked. Rule order matters.

**Check yourself:** run `sudo ip netns exec router conntrack -L` (`apt install conntrack`)
right after the second ping. Which addresses does the entry list, and in what state?
<details><summary>Answer</summary>One ICMP entry: src=192.168.10.10 dst=192.168.20.10 for the
original direction, with the reverse tuple for replies. ICMP has no TCP-style states; the entry
just exists for ~30 s, and replies matching it count as ESTABLISHED.</details>

### 2. NAT, twice
**Concept:** private addresses like 192.168.x can't travel the internet. **Source NAT**
(MASQUERADE) rewrites the source address to the router's own address on the way out and
reverses it for replies, using conntrack to remember the mapping.

**Try it:**
```bash
sudo tcpdump -ni ens5 icmp                                 # terminal 1: the host's uplink
sudo ip netns exec lan-a ping -c 3 1.1.1.1                 # terminal 2
sudo conntrack -L -p icmp                                  # host's NAT table
ip -4 addr show ens5; curl -s https://checkip.amazonaws.com
```
**What you should see:** on `ens5` the pings come from the instance's **private** VPC
address (e.g. 172.31.x.x), not 192.168.10.10. `checkip` shows a *third* address: the
instance's public IP.

**Why:** the host masquerades 192.168.10.10 → its VPC address. Then the AWS internet gateway
does its own 1:1 NAT from the VPC address to the public IP. Your lab sits behind two layers
of NAT, which is exactly what happens to a laptop on home Wi-Fi behind a carrier's NAT.

**Check yourself:** two lan-a processes ping 1.1.1.1 at the same time. After NAT both
have the same source IP. How does the host know which reply belongs to which?
<details><summary>Answer</summary>Conntrack keys ICMP echo by its identifier field (TCP/UDP by
port). If two flows would collide after rewriting, NAT also rewrites the identifier or port so
each mapping stays unique.</details>

### 3. The missing return route
**Concept:** routing is decided **per packet, per hop, per direction**. A request getting
there says nothing about whether the reply can get back. "It goes out but nothing comes
back" is the most common symptom in network debugging.

**Try it:**
```bash
sudo ip route del 192.168.10.0/24                          # break the host's route back to lan-a
sudo tcpdump -ni any 'icmp and host 1.1.1.1'               # terminal 1
sudo ip netns exec lan-a ping -c 3 1.1.1.1                 # terminal 2
sudo ip netns exec lan-a ping -c 3 192.168.20.10           # still works. Why?
sudo ip route add 192.168.10.0/24 via 192.168.255.2        # repair
```
**What you should see:** requests go out `ens5` and replies come back in, de-NATed to
192.168.10.10. Then the host sends the replies **back out `ens5`** toward the VPC router,
which drops them, instead of into `h-up`. (If the host's FORWARD policy is DROP, from ufw or
Docker, the host drops them itself. Same lesson.) The lan-a → lan-b ping keeps working.

**Why:** after un-NATing, the host looks up 192.168.10.10 and, with no specific route,
falls back to the default route via `ens5`. lan-a → lan-b never touches the host: the
router namespace is directly connected to both LANs.

**Check yourself:** which single command, run on the host, would have told you the reply's
next hop without capturing anything?
<details><summary>Answer</summary><code>ip route get 192.168.10.10</code>: it prints the
interface and gateway the kernel would use.</details>

### 4. Path MTU Discovery working, then failing
**Concept:** each link has an **MTU**, the largest IP packet it carries. When a packet
with the Don't-Fragment (DF) bit set is too big for the next link, the router drops it and
sends back ICMP "fragmentation needed" quoting the MTU. The sender then shrinks its packets.
That's **Path MTU Discovery (PMTUD)**, and it only works if the ICMP gets back.

**Try it:**
```bash
sudo ip netns exec lan-a ip route get 192.168.20.10              # before: no mtu listed
sudo ip -n router link set r-b mtu 1200                          # a small hop between router and lan-b
sudo ip netns exec router tcpdump -ni r-a icmp                   # terminal 1: router's lan-a-side port
sudo ip netns exec lan-a ping -c 3 -M do -s 1300 192.168.20.10   # terminal 2: 1300 + 28 header bytes > 1200
sudo ip netns exec lan-a ip route get 192.168.20.10              # after: note "mtu 1200"
sudo ip netns exec lan-a ping -c 3 -M do -s 1172 192.168.20.10   # 1172 + 28 = 1200: fits exactly
sudo ip -n router link set r-b mtu 1500                          # repair
```
**What you should see:** the first 1300-byte ping gets `Frag needed and DF set (mtu = 1200)`
from 192.168.10.1, and tcpdump shows that ICMP message leaving the router. The next ones fail
locally with `message too long, mtu=1200`, and the 1172-byte ping succeeds. Afterwards, `ip route get` shows a
cached `mtu 1200` for that destination: lan-a learned the path MTU.

**Why:** `-M do` sets DF, and `-s 1300` makes a 1328-byte IP packet. The router can't
forward it onto the 1200-byte link, so it drops it and sends ICMP back to lan-a, whose
kernel records the smaller MTU. TCP does the same thing invisibly for every connection.
Casebook case 03 shows what happens when a firewall eats that ICMP.

**Check yourself:** why does ping keep failing even after lan-a learns the MTU, when TCP
would recover?
<details><summary>Answer</summary>You asked ping to send exactly 1300 bytes with DF; it can't
shrink a datagram you sized. TCP chooses its own segment sizes, so it just sends smaller ones.</details>

## Known limitations
- The firewall filters only FORWARD. lan-b can still ping the router's lan-a-side address (192.168.10.1), because packets to the router itself go through INPUT. Try it and explain why.
- IPv4 only. No DHCP: addresses are static.
- Uses `iptables` (the nft backend on modern Ubuntu). The nftables rewrite is a good follow-up.
- If your VPC or another tool already uses 192.168.10/20/255.x, edit `config.sh`.
- The host rules are inserted at the top of FORWARD to get past ufw/docker DROP policies. Teardown removes exactly those rules.
