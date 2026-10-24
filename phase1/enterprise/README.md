# enterprise: Acme Corp, a corporate network in Docker

Phase 1.7, the capstone of Phase 1's networking. Everything earlier in this repo taught one
mechanism at a time. This is what they look like **assembled the way a real company runs them**:
an HQ with an internet edge firewall, a DMZ, a web proxy, segmented internal networks, internal
DNS and dynamic routing, plus a branch office over a site-to-site VPN and a remote worker on a
remote-access VPN. And then it breaks, through help desk tickets you troubleshoot from symptoms alone,
which is the actual job of a Forward Deployed Engineer at a customer site.

**Before this:** netns-router (routing, NAT, firewalls), overlays (WireGuard, MTU, MSS clamping), and
ideally the casebook. **Runs on:** the Ubuntu EC2 box, with Docker. It uses 12 containers. A
**t3.large (2 vCPU, 8 GB) or bigger** leaves room for captures and for running other labs alongside. Check
the real footprint on yours with `docker stats --no-stream`: [X] MB total.

## The network
```
                                   ┌───────────────── the internet (acme-inet, 203.0.113.0/24) ─────────────────┐
                                   │   203.0.113.1 = "the ISP" (the EC2 host, NATing out to the real internet)   │
                                   │                                                                             │
                     remote 203.0.113.50                 edge-fw 203.0.113.10                       branch-fw 203.0.113.20
                    (home worker, WireGuard   ═══ wg-ra ═══╡  ║ ╞═══ wg-site (site-to-site VPN, OSPF) ═══╡
                     split tunnel)                         │  ║                                         │ lan 10.20.10.1
                                          DMZ 172.16.10.0/24  ║ transit 10.255.0.0/24                   │
                                  ┌───────────┴──────┐        ║                                  branch office 10.20.10.0/24
                             www .10         proxy .20        ║                                         │
                          (public site)    (Squid :3128)   core-rtr (OSPF, east-west firewall)        bws1 .11
                                                    ┌─────────╨──────────┬──────────────────┐
                                        users 10.10.10.0/24   servers 10.10.20.0/24   mgmt 10.10.99.0/24
                                           ws1 .11  ws2 .12     dns .10  intranet .20     jumpbox .10
```

| Zone | Subnet | Who's there | Gateway |
|---|---|---|---|
| internet | 203.0.113.0/24 | edge-fw, branch-fw, remote, and "the ISP" at .1 | .1 |
| DMZ | 172.16.10.0/24 | `www` (public website), `proxy` (Squid web proxy) | edge-fw .1 |
| transit | 10.255.0.0/24 | the link between edge-fw (.1) and core-rtr (.2) | n/a |
| users | 10.10.10.0/24 | `ws1`, `ws2` (staff laptops) | core-rtr .1 |
| servers | 10.10.20.0/24 | `dns` (internal DNS), `intranet` (web on :80, admin console on :8080) | core-rtr .1 |
| mgmt | 10.10.99.0/24 | `jumpbox` (the IT admins' box) | core-rtr .1 |
| branch | 10.20.10.0/24 | `bws1` (branch laptop) | branch-fw .1 |
| VPN | 10.255.1.0/30, 10.255.2.0/24 | site-to-site tunnel, remote-access clients | n/a |

### The policy
This is the part a real network team argues about. **North-south** (in and out of the
company) is enforced by edge-fw. **East-west** (between internal segments) is enforced by core-rtr.

| From → To | Allowed | Why |
|---|---|---|
| staff (HQ + branch) → internet web | **only via the proxy** | visibility, URL policy and logging; direct web is blocked at the edge |
| staff → blocked site (`example.org`) | no (proxy returns 403) | stand-in for a URL-filtering category |
| staff → anything, ICMP | yes | ping and traceroute for troubleshooting |
| anyone inside → DNS (10.10.20.10), intranet :80/443 | yes | shared services |
| anyone except mgmt → intranet admin :8080 | no | admin interfaces are for IT only |
| mgmt (jumpbox) → anything | yes, including direct internet | IT needs to fix things when the proxy is broken |
| DNS server → internet :53 | yes | it forwards outside names to public resolvers |
| internet → 203.0.113.10:80 | yes, port-forwarded to `www` | the public website |
| DMZ → inside | no | if the public web server is compromised, it must not become a way in |
| branch ↔ HQ | yes, over the site-to-site VPN | one company. The branch's internet traffic also goes via HQ ("backhaul") |
| remote worker → intranet :80/443 and DNS | yes, over the remote-access VPN | least privilege for laptops outside |

## Run
```bash
sudo apt install -y docker.io docker-compose-v2
sudo usermod -aG docker $USER            # then log out and back in
cd phase1/enterprise
./lab.sh up                              # builds the image (a few minutes the first time), makes VPN keys, starts everything
./lab.sh test                            # the policy matrix above, end to end (needs internet from the instance)
./lab.sh shell ws1                       # be a user: curl, dig, ping, traceroute...
./lab.sh down
```
Every box has the usual tools: `ip`, `ping`, `traceroute`, `mtr`, `tcpdump`, `dig`, `curl`, `nc`,
`conntrack`, `iptables`, and on the routers `vtysh` (FRR's Cisco-like CLI) and `wg`. The role scripts in
`node/roles/` are short and commented: read the one for each box you're inspecting.

## Sample output
<!-- Paste a real ./lab.sh test run from your instance here. -->
```
[paste ./lab.sh test output]
```

---

## How Docker is made to behave like a real network
Docker's defaults are designed for apps, not network labs. Each one is an assumption worth
understanding, because you'll meet the same defaults when you deploy into a customer's container platform.

| Docker default | Why it's wrong for an enterprise | What this lab does |
|---|---|---|
| Each network is a Linux bridge on the host, and the host takes an address on it (the "gateway") | Then the host, not your router, is every segment's gateway | The gateway is `.254`, and each container's default route is replaced with the enterprise router at `.1` (`node/entrypoint.sh`) |
| Docker NATs every network out to the internet | Internal segments would have a way out that skips the firewall | NAT is disabled (`enable_ip_masquerade: false`) on everything except `inet` |
| Containers use Docker's DNS (127.0.0.11), which knows container names | Real machines use the company's DNS server | `/etc/resolv.conf` points at 10.10.20.10 (or a public resolver in the DMZ) |
| Interfaces are `eth0`, `eth1`... in no guaranteed order | Firewall rules and captures need stable names | Each interface is renamed after its segment (`users`, `transit`, `inet`...) by address, so `tcpdump -i users` works |
| Containers can't change routes or firewalls | Routers have to | `NET_ADMIN` (plus `ip_forward=1` on routers) |

Every segment is also visible **from the host**: the bridges are named `acme-users`, `acme-transit`
and so on, so `sudo tcpdump -ni acme-transit` on the EC2 box watches the HQ backbone like a network tap.

---

## Exercises
Bring the lab up and pass `./lab.sh test` first. Run commands as `docker exec <box> <command>`, or open
a shell with `./lab.sh shell <box>`.

### 1. Walk the network with traceroute
**Concept:** traceroute sends packets with TTL 1, 2, 3... and every router that drops one
reports back with an ICMP "time exceeded", so the replies map the path. (Use `-I` for ICMP probes:
the firewalls here allow ICMP, but not traceroute's default UDP probes.)

**Try it:**
```bash
docker exec ws1 traceroute -n -I intranet.corp.acme
docker exec ws1 traceroute -n -I 172.16.10.10        # the DMZ web server
docker exec ws1 traceroute -n -I 1.1.1.1
docker exec bws1 traceroute -n -I intranet.corp.acme
```
**What you should see:** intranet is 2 hops (core-rtr, then the server). The DMZ adds edge-fw
(via its transit address, 10.255.0.1). The internet path continues through 203.0.113.1 (the EC2 host)
and then AWS's and the internet's routers. From the branch: branch-fw, then **10.255.1.1** (edge-fw's
tunnel address), then core-rtr, then the server.

**Why:** each router answers from the interface the probe *arrived* on, which is why edge-fw shows
up as 10.255.0.1 from HQ and as 10.255.1.1 from the branch.

**Check yourself:** branch-fw and edge-fw are separated by an "internet" (the acme-inet bridge). Why
does the branch traceroute show them as adjacent?
<details><summary>Answer</summary>The tunnel hides the underlay. Inside WireGuard, branch-fw → edge-fw is
one hop: the TTL-limited packet is encrypted and carried in a fresh outer packet, so no router in between
ever sees (or decrements) the inner TTL. In real life the "one hop" might cross 15 ISP routers, and that's
why VPN latency surprises people.</details>

### 2. Docker under the hood
**Concept:** each Docker network is a Linux bridge on the host, and each container interface is
one end of a veth pair whose other end is plugged into that bridge: exactly the netns-router lab, built by Docker.

**Try it** (on the EC2 host, not in a container):
```bash
ip -br link | grep acme                          # one bridge per segment
bridge link | grep -c acme-users                 # veth ends plugged into the users bridge
docker network inspect acme-users --format '{{range .Containers}}{{.Name}} {{.IPv4Address}}{{"\n"}}{{end}}'
docker exec ws1 ip -br addr; docker exec ws1 ip route
sudo tcpdump -ni acme-transit -c 10              # tap the HQ backbone while ws1 browses
```
**What you should see:** `acme-users` and the others, each with the host's `.254` address. ws1's
interface is named `users`, with its default route via 10.10.10.1 (core-rtr), not Docker's .254. On
`acme-transit`, OSPF hellos (`OSPFv2, Hello`) every 5 s between 10.255.0.1 and 10.255.0.2, and any traffic leaving HQ.

**Why:** a container is a set of namespaces, and its network namespace is wired with veth into a host bridge,
like the lab you built by hand. The host is attached to every bridge, which is why we had to take
routing away from it (`.254` is never anyone's gateway) and why it makes a perfect capture point.

**Check yourself:** the host has an address on every segment and IP forwarding on (Docker turns it on).
What stops ws1 from reaching the DMZ through the *host*, skipping both firewalls?
<details><summary>Answer</summary>Two things. ws1's default route points at core-rtr, not .254. And even if ws1
sent traffic to .254, Docker's own iptables rules on the host (the DOCKER-ISOLATION chains) drop traffic
routed between two Docker bridges. Look with <code>sudo iptables -L DOCKER-ISOLATION-STAGE-1 -n -v</code>.
Relying on that is fragile, so the lab doesn't: the routes do the real work.</details>

### 3. OSPF: routes nobody typed
**Concept:** OSPF routers find neighbors with hello packets, flood the links they know about
to each other (the **link-state database**), and each computes shortest paths from the same map.
Nobody configured a route to the branch on core-rtr: it learned one.

**Try it:**
```bash
docker exec edge-fw vtysh -c 'show ip ospf neighbor'
docker exec core-rtr vtysh -c 'show ip route ospf'
docker exec branch-fw vtysh -c 'show ip route'          # O = OSPF, K = kernel, C = connected
docker exec edge-fw vtysh -c 'show ip ospf database'
# A router dies silently (no link-down, no goodbye): freeze it and time the reaction.
docker exec branch-fw sh -c 'while sleep 1; do date +%T; ip route show 10.10.20.0/24; done'   # terminal 2
docker pause core-rtr; sleep 40; docker unpause core-rtr
```
**What you should see:** edge-fw has two neighbors in state `Full`: 10.255.0.2 (core, on transit) and 10.255.1.2
(branch, on wg-site). The branch routes `O>* 10.10.20.0/24` via wg-site, and its default
`O>* 0.0.0.0/0` comes from edge-fw's `default-information originate`. After the pause, the branch's
route to 10.10.20.0/24 disappears roughly 20 s later, and comes back a few seconds after the unpause. Time it from terminal 2: [X] s.

**Why:** `docker pause` freezes the process, so core-rtr sends no more hellos, but its link stays up.
edge-fw only notices when the **dead interval** (20 s here, 40 s by default) passes without a hello. It then
recomputes and floods the change, and the branch drops the route. A failure that takes the link down is
detected at once instead. Real networks add **BFD** to detect silent failures in milliseconds.

**Check yourself:** core-rtr's users, servers and mgmt interfaces are **passive**. What would change if they weren't?
<details><summary>Answer</summary>core-rtr would send OSPF hellos onto those segments every 5 s. That's useless (no
routers live there) and dangerous: any laptop running an OSPF daemon could form an adjacency and inject
routes, for instance a fake default route that pulls all of HQ's traffic through it. Passive means
"advertise this subnet, but never talk OSPF on it".</details>

### 4. Egress: everything through the proxy
**Concept:** most companies don't let laptops talk to the web directly. A **forward proxy** makes every
web request visible, applies URL policy, and logs it. The edge firewall blocks direct web traffic, so the proxy isn't optional.

**Try it:**
```bash
docker exec proxy tail -f /var/log/squid/access.log                     # terminal 2
docker exec ws1 curl -sS -o /dev/null -w '%{http_code}\n' https://example.com
docker exec ws1 curl -sS -o /dev/null -w '%{http_code}\n' http://example.org
docker exec ws1 curl --noproxy '*' -sS --max-time 5 https://example.com
docker exec edge-fw iptables -L FORWARD -v -n --line-numbers | tail -3
docker exec jumpbox curl -sS -o /dev/null -w '%{http_code}\n' https://example.com
```
**What you should see:** the first request is `200`, and the log shows `TCP_TUNNEL/200 ... CONNECT example.com:443`.
The second is `403`, logged as `TCP_DENIED/403`. The direct attempt times out, and the edge's `default deny`
counter goes up. The jumpbox, which has no proxy, gets `200` directly.

**Why:** for HTTPS the proxy sees only `CONNECT example.com:443`, the hostname, never the
page, because the rest is encrypted end to end. That's still enough for domain policy and logging.
(Companies that inspect content do "TLS interception" with their own CA installed on every laptop, which is a big
privacy and security trade-off.)

**Check yourself:** the proxy setting is just an environment variable on ws1 (`env | grep -i proxy`).
What would happen if the edge firewall allowed direct web traffic too?
<details><summary>Answer</summary>Any program that ignores the setting (or a user who unsets it) would bypass the
proxy: no policy, no logs. A proxy is only a control if the firewall makes it the <i>only</i> way out. Ask any customer
whose "web filter" was trivially bypassed.</details>

### 5. The DMZ and the port forward
**Concept:** public servers live in a **DMZ**, a segment the internet can reach a little of, and
that can't reach inside at all. The firewall's **DNAT** maps its public address to the server's private one.

**Try it:**
```bash
docker exec remote curl -sS http://203.0.113.10                      # from the internet
docker exec edge-fw conntrack -L -p tcp --dport 80 2>/dev/null | head -3
docker exec www curl -sS --max-time 4 http://10.10.20.20             # from the DMZ to the inside
```
**What you should see:** the public page. The conntrack entry shows both halves of the translation:
the original direction `src=203.0.113.50 dst=203.0.113.10 dport=80`, and the reply direction from
`src=172.16.10.10`. The DMZ-to-intranet request times out.

**Why:** conntrack remembers each translated connection and rewrites the replies back, so the client never
learns the server's real address. The DMZ rule is the important one: the web server is the box most likely to be
compromised, and the DMZ makes sure that breaking into it isn't breaking into the company.

**Check yourself:** www's replies to the internet are allowed, but www can't open new connections out
(try `docker exec www curl -sS --max-time 5 https://example.com`). Why is blocking *outbound* from the DMZ worth it?
<details><summary>Answer</summary>An attacker who compromises a server usually wants to download tools and call home to a
command-and-control server. Blocking new outbound connections breaks both. Replies still work, because they're
part of connections the internet started (ESTABLISHED in conntrack).</details>

### 6. Split DNS, and why hairpin NAT fails
**Concept:** the company website has one public address (203.0.113.10). Inside, staff use the same
*name*, but internal DNS gives them the server's DMZ address. That's **split-horizon DNS**. Without it,
staff would connect to the public address from inside, which needs the firewall to "hairpin" the traffic
back in, and many firewalls don't.

**Try it:**
```bash
docker exec ws1 dig +short www.acme.example                          # the internal view
docker exec remote dig +short www.acme.example @1.1.1.1              # the public view, from outside (empty: .example is reserved)
docker exec ws1 dig +short +tries=1 +time=3 example.com @1.1.1.1      # can staff bypass internal DNS? ?
docker exec ws1 curl -sS --max-time 5 http://www.acme.example        # by name: works
docker exec ws1 curl --noproxy '*' -sS --max-time 5 http://203.0.113.10   # by public IP, from inside: ?
```
**What you should see:** internal DNS answers `172.16.10.10`, and the public resolver has no answer. ws1's
query straight to 1.1.1.1 times out: only the DNS server may send DNS out. By name, the page loads. By public IP, it times out.

**Why:** the port forward only applies to packets arriving on the *internet* interface (`-i inet`).
ws1's packet for 203.0.113.10 arrives on transit, isn't translated, and is addressed to edge-fw itself,
whose INPUT policy drops it. Real fixes are split DNS (used here) or "NAT reflection" (also translating
inside traffic, plus SNAT so replies come back through the firewall).

**Check yourself:** on a real domain, the public DNS answer for www would be 203.0.113.10. If an HQ laptop
used 1.1.1.1 as its resolver instead of the company's, what breaks, and what does the user see?
<details><summary>Answer</summary>They'd get the public address, and the hairpin would fail: the company website "is down"
for them, but works for everyone else and from home. A classic ticket. That's one reason this lab's edge firewall lets
only the DNS server send DNS out (you saw ws1's query to 1.1.1.1 time out): it forces every laptop onto internal DNS.
The fix for a misconfigured laptop is DHCP or device management, not opening the firewall.</details>

### 7. The branch: site-to-site VPN with routing on top
**Concept:** the branch is joined by one WireGuard tunnel, and OSPF runs *inside* the tunnel, so the
branch learns HQ's networks (and its default route) automatically. The underlay only ever sees encrypted UDP.

**Try it:**
```bash
sudo tcpdump -ni acme-inet -c 6 'udp port 51820'                     # on the host: the "internet" between the sites
docker exec bws1 curl -sS http://intranet.corp.acme
docker exec branch-fw wg show
docker exec intranet tcpdump -ni servers -c 2 'tcp[tcpflags] & tcp-syn != 0'   # terminal 2, then curl from bws1 again
```
**What you should see:** UDP between 203.0.113.20 and 203.0.113.10, with no trace of 10.x addresses or
HTTP. `wg show` reports a recent handshake and growing transfer counters. The SYN from bws1 arrives at the intranet
server with **mss 1380**, not 1460.

**Why:** branch-fw clamps the MSS in passing SYNs to fit the tunnel (WireGuard MTU 1420 − 40 = 1380). That
prevents exactly the black hole from overlays exercise 9, without depending on ICMP.

**Check yourself:** edge-fw's WireGuard peer for the branch lists `224.0.0.5/32` and `224.0.0.6/32` in allowed-ips
(`node/roles/edge.sh`). What breaks without them, and how would you spot it?
<details><summary>Answer</summary>OSPF sends its hellos to the multicast groups 224.0.0.5/6. WireGuard only sends a packet to a
peer whose allowed-ips include the destination, so edge-fw's hellos would be dropped. The branch would never see
a hello, the adjacency would never form, and the branch would have no routes. All while <code>wg show</code> reports
a perfectly healthy tunnel. "The VPN is up but nothing works" is a routing problem until proven otherwise.</details>

### 8. The remote worker: split tunnel
**Concept:** a remote-access VPN connects one laptop. With a **split tunnel**, only company
ranges go through it, and Netflix goes straight out the home connection. A **full tunnel** sends everything
through HQ: more control, more load on HQ, more latency.

**Try it:**
```bash
docker exec remote ip route                                          # what goes where
docker exec remote curl -sS http://10.10.20.20                       # intranet through the VPN
docker exec remote dig +short intranet.corp.acme @10.10.20.10        # internal DNS through the VPN
docker exec remote traceroute -n -I -m 3 1.1.1.1                     # internet: not via HQ
docker exec remote curl -sS --max-time 4 http://10.10.20.20:8080     # admin console: ?
```
**What you should see:** `10.10.0.0/16 dev wg0` and a default route via 203.0.113.1. The intranet and internal
DNS work. traceroute to 1.1.1.1 goes straight to 203.0.113.1, not to 10.255.2.1. The admin console times out.

**Why:** the client's allowed-ips and its routes are the split. The edge firewall still filters what
remote users may reach once inside: only the intranet's web ports and DNS. Being on the VPN is not
the same as being trusted.

**Check yourself:** with this split tunnel, the remote user's own DNS resolver (1.1.1.1) can't resolve
`intranet.corp.acme`. What do real VPN clients do about it?
<details><summary>Answer</summary>They configure <b>split DNS</b> on the laptop: queries for corp.acme go to the company's DNS
through the tunnel, and everything else goes to the normal resolver (systemd-resolved, macOS resolver domains, Windows NRPT).
Getting this wrong is one of the commonest VPN tickets: "the VPN connects, but the intranet doesn't load".</details>

---

## Help desk tickets
The real skill: a user describes a symptom, and you find the cause. Inject a ticket with
`./lab.sh ticket N`, then troubleshoot **without reading the scripts**. Work from the symptom, test
your hypotheses with `ping`, `dig`, `curl -v`, `traceroute`, `tcpdump`, `iptables -L -v -n`, `vtysh`
and `wg show`, find the box and the cause, and write a one-paragraph root cause like the casebook's.
`./lab.sh fix` undoes all of them.

A method that works: **reproduce** it yourself, then decide **who is affected and who isn't**, then **follow the
packet** hop by hop, then find **what changed**.

**Ticket 1:** "The internet is down for everyone at HQ *and* at the branch. The intranet doesn't load either.
But IT says they can ping 1.1.1.1 just fine."
<details><summary>Root cause</summary>DNS, at core-rtr. Two DROP rules inserted at the top of its FORWARD chain block port 53
to 10.10.20.10. Clues: <code>ping 1.1.1.1</code> and <code>curl http://10.10.20.20</code> (by IP) work, while
<code>dig intranet.corp.acme</code> times out. <code>iptables -L FORWARD -v -n --line-numbers</code> on core-rtr shows the
rules with climbing counters. Lesson: "the internet is down" is very often "DNS is down".</details>

**Ticket 2:** "The branch office can't reach anything: not the intranet, not the web. HQ is fine. The branch
router says the VPN is connected."
<details><summary>Root cause</summary>An OSPF hello-interval mismatch on branch-fw's tunnel interface (10 s against 5 s). Routers
refuse adjacencies with mismatched timers. <code>wg show</code> shows recent handshakes (the tunnel <i>is</i> up), but
<code>vtysh -c 'show ip ospf neighbor'</code> is empty on branch-fw, and <code>show ip ospf interface wg-site</code>
shows the different hello. With no adjacency the branch has no routes, not even a default.</details>

**Ticket 3:** "No website works for anyone, through the proxy or not. The intranet is fine. Customers say our
public website still works."
<details><summary>Root cause</summary>edge-fw lost its source NAT (the POSTROUTING chain was flushed). Outbound packets leave
with private source addresses (watch <code>docker exec edge-fw tcpdump -ni inet icmp</code> during a ping:
<code>10.10.10.11 > 1.1.1.1</code>), and no reply can ever come back to a private address. Inbound still works because the
DNAT rule is in PREROUTING, untouched. <code>iptables -t nat -L -v -n</code> shows it.</details>

**Ticket 4:** "My laptop (ws2) can't reach anything, not even the intranet. My colleague on ws1, at the next
desk, is fine."
<details><summary>Root cause</summary>ws2's default gateway is 10.10.10.99, an address nobody has. <code>ip route</code> shows it,
and <code>ip neigh</code> shows 10.10.10.99 as INCOMPLETE/FAILED: ARP never gets an answer. One user broken
and their neighbor fine means look at that machine first, not the network.</details>

**Ticket 5:** "The web is broken for all staff, HQ and branch. IT's jumpbox can browse fine. Ping to the
internet works for everyone."
<details><summary>Root cause</summary>The proxy is down (the container was stopped). <code>curl -v https://example.com</code>
on ws1 fails while connecting to proxy.corp.acme:3128. The jumpbox works because it doesn't use the proxy. When one group
is broken and another isn't, list what's different about their paths: here, a single dependency.</details>

---

## Known limitations
- Not yet run end to end: written for Ubuntu 24.04 with Docker 24-28. Paste `./lab.sh test` output above once you have.
- Single points of failure everywhere (one edge, one core, one ISP): real HQs pair them (VRRP, dual ISPs with BGP). That's a good extension.
- No DHCP (Docker assigns addresses), no Active Directory or 802.1X, and no IPv6.
- The WireGuard keys live in `state/` (gitignored). Delete it to make new ones.
- The web checks in `test.sh` need internet access from the instance.
