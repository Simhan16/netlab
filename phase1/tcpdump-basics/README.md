# tcpdump-basics: reading packets (start here)

Step 1.0, the chapter before everything else. No C++ in this one. You learn to **see**
network traffic and to read what you see:
1. What is inside a packet: the three kinds of addresses (Layer 2 MAC, Layer 3 IP, Layer 4 port).
2. tcpdump: capturing on the Mac and on the EC2 box, and reading its one-line output.
3. Wireshark on the Mac: opening a capture from EC2 and digging into it.

Every later chapter assumes you can do these three things. The netns lab, the DNS
resolver and mini-tcpdump all ask you to "capture it and look".

**What you need:** a Mac (tcpdump is built in), [Wireshark](https://www.wireshark.org/download.html)
(`brew install --cask wireshark`) and, for part 5, SSH access to your Ubuntu EC2 instance.

## The practice capture
`practice.pcap` (18 packets, 1.7 KB) is in this folder, so you can start without root and
without EC2. It's a hand-built capture of a laptop (192.168.10.10) doing four things:
1. **ARP:** finding its gateway's MAC address.
2. **DNS:** looking up `www.example.com`.
3. **HTTP:** fetching one page over TCP port 80: handshake, request, response, teardown.
4. **Two failures:** a mistyped hostname (`wwww.example.com`), then a connection to a closed port (8443).

It's synthetic: `make_practice_pcap.py` builds every byte by hand (with correct
checksums), and you can read the script alongside the packets. The addresses are
documentation ranges, so none of them belong to a real machine. Real captures are messier:
part 5 has you make one.

---

## Part 1: what's in a packet
### Layers wrap each other
Sending data means wrapping it in headers, one per layer, like envelopes inside envelopes.
Packet 8 of the practice capture (the HTTP request) is 132 bytes:
```
┌────────────────── Ethernet frame: 132 bytes ──────────────────────────────┐
│ Ethernet hdr │ IPv4 hdr  │ TCP hdr   │ HTTP request: "GET / HTTP/1.1..."   │
│   14 bytes   │ 20 bytes  │ 20 bytes  │ 78 bytes  (the "payload")           │
│  Layer 2     │ Layer 3   │ Layer 4   │ Layer 7 (application)               │
│  MAC → MAC   │ IP → IP   │ port→port │                                     │
└────────────────────────────────────────────────────────────────────────────┘
```
Each header has a field saying what comes next. The Ethernet **EtherType** `0x0800` means
"IPv4 follows" (`0x0806` means ARP). The IP **protocol** field `6` means TCP (`17` means UDP).
That's how tcpdump and Wireshark know how to decode each layer.

Each layer has its own address, and each answers a different question:

| Layer | Address | Size | Answers | Scope |
|---|---|---|---|---|
| 2: link (Ethernet, Wi-Fi) | MAC, `02:00:0a:00:00:0a` | 48 bits | which network card *on this wire* gets the frame? | one hop: the local network only |
| 3: network (IP) | IP, `192.168.10.10` | 32 bits (IPv4) | which *machine* in the world is it for? | end to end, across every router |
| 4: transport (TCP/UDP) | port, `51244` | 16 bits | which *program* on that machine gets it? | the two endpoints only |

Think of a letter to an apartment: the street address gets it to the building (IP), the
apartment number to the right person (port). The MAC is the delivery van for one leg of the
trip, swapped at every depot.

### Layer 2: MAC addresses
**Concept:** a MAC address is burned into (or assigned to) each network interface. Switches
deliver frames by MAC, and **only within one local network**. A MAC never crosses a router.
Two special cases:
- `ff:ff:ff:ff:ff:ff` is **broadcast**: every device on the LAN receives it.
- The first byte's second-lowest bit marks a **locally administered** address. Virtual NICs
  (VMs, containers, EC2, this capture's `02:...` addresses) use these, and phones randomize them
  for privacy. Factory-assigned MACs start with the vendor's prefix (the **OUI**), and Wireshark
  shows the vendor name.

**ARP** connects Layer 3 to Layer 2. To send to an IP on its own LAN, a host broadcasts
"who has 192.168.10.1?", the owner replies with its MAC, and the answer is cached. Packets 1-2:
```
ARP, Request who-has 192.168.10.1 tell 192.168.10.10, length 28
ARP, Reply 192.168.10.1 is-at 02:00:0a:00:00:01, length 28
```

**The key idea:** when the destination is *not* on the local network, the frame goes to the
**gateway's** MAC, with the far-away IP inside. `tcpdump -e` prints the MACs:
```
$ tcpdump -ner practice.pcap
02:00:0a:00:00:0a > ff:ff:ff:ff:ff:ff, ethertype ARP (0x0806), length 42: Request who-has 192.168.10.1 tell 192.168.10.10, length 28
02:00:0a:00:00:01 > 02:00:0a:00:00:0a, ethertype ARP (0x0806), length 42: Reply 192.168.10.1 is-at 02:00:0a:00:00:01, length 28
02:00:0a:00:00:0a > 02:00:0a:00:00:01, ethertype IPv4 (0x0800), length 75: 192.168.10.10.53124 > 1.1.1.1.53: ...
```
The DNS query goes *to* 1.1.1.1 (IP) *via* `02:00:0a:00:00:01` (the gateway's MAC). Every router
on the way strips the Ethernet header and writes a new one for the next hop. The IP header
travels the whole way, almost unchanged.

### Layer 3: IP addresses
**Concept:** an IP address identifies a machine (strictly, an interface) end to end. The
**prefix length** splits it into network and host parts. `192.168.10.10/24` means the first 24
bits (`192.168.10`) name the network, so `192.168.10.0`-`192.168.10.255` are "local". A host
makes one decision per packet:
- **Destination in my subnet?** ARP for the destination itself and send to it directly.
- **Otherwise:** send to the **default gateway** (router) and let it forward.

Other fields in the IP header worth knowing (see them with `tcpdump -v`):
- **TTL (time to live):** every router subtracts 1, and the packet is dropped at 0 (that's how
  `traceroute` works). Senders start at 64 (Linux, macOS) or 128 (Windows), so a reply arriving
  with TTL 52 has probably crossed 12 routers.
- **Private ranges:** `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16` are for internal networks
  and aren't routed on the internet. A **NAT** router rewrites them to a public address on the way out.
- **DF (don't fragment)** and **length:** these matter later, for MTU problems (netns-router exercise 4).

```
$ tcpdump -nvr practice.pcap 'port 53'
IP (tos 0x0, ttl 64, id 7239, offset 0, flags [DF], proto UDP (17), length 61)
    192.168.10.10.53124 > 1.1.1.1.53: 19754+ A? www.example.com. (33)
IP (tos 0x0, ttl 57, id 7240, offset 0, flags [DF], proto UDP (17), length 77)
    1.1.1.1.53 > 192.168.10.10.53124: 19754 1/0/0 www.example.com. A 203.0.113.10 (49)
```
Outgoing packets have TTL 64 (fresh from this machine). Replies from 1.1.1.1 arrive with 57,
about 7 hops away. The web server's replies arrive with 52, about 12 hops away.

**On EC2:** your instance has a private address (`172.31.x.x` in a default VPC). The public IP
you SSH to is NAT done by AWS outside the instance, so it **never appears in a capture taken on
the instance**. This confuses everyone the first time.

### Layer 4: ports
**Concept:** a port picks out one program (one **socket**) on a machine. Servers listen on
**well-known ports**: 22 SSH, 53 DNS, 80 HTTP, 443 HTTPS. Clients get a random **ephemeral
port** from the OS, from 32768-60999 on Linux and 49152-65535 on macOS. A connection is identified by its
**5-tuple** (protocol, source IP, source port, destination IP, destination port). Open two
browser tabs to the same site and the two connections differ only in their source port.

tcpdump prints the port after the IP, separated by a dot:
```
192.168.10.10.51244 > 203.0.113.10.80
└──── IP ────┘└port┘   └─── IP ───┘└port┘
```
The two transport protocols:
- **UDP** (DNS, video calls, QUIC) is just ports, a length and a checksum. No connection, no
  ordering, no retransmission: each datagram stands alone.
- **TCP** (web, SSH, databases) adds a connection: a handshake, **sequence numbers** for every
  byte, acknowledgements, retransmission and flow control. Those fields are what most of
  troubleshooting reads.

ARP has no IP header and no ports at all: it lives entirely in Layer 2.

**Check yourself:** packets 3 and 5 go to different IP addresses (1.1.1.1 and 203.0.113.10).
Why do they have the same destination MAC?
<details><summary>Answer</summary>Neither IP is on the laptop's /24, so both go to the default
gateway. The destination MAC names the next hop (the gateway, <code>02:00:0a:00:00:01</code>), and
the destination IP names the final machine.</details>

**Check yourself:** a server on port 443 has 10,000 clients connected at once. How does its
kernel know which socket an arriving packet belongs to, when all of them have destination port 443?
<details><summary>Answer</summary>By the whole 5-tuple. The server's IP and port are the same for
every connection, but each client's IP and source port differ, so every connection's tuple is unique.</details>

---

## Part 2: tcpdump
tcpdump asks the kernel for a **copy** of every packet passing an interface. It's passive: it
doesn't block, change or slow traffic. It needs root (capturing reads everyone's traffic),
except when reading a saved file with `-r`.

It's on every Mac at `/usr/sbin/tcpdump`. On Ubuntu, install it with `sudo apt install tcpdump`
if `which tcpdump` finds nothing.

### Reading the output
```
$ tcpdump -nr practice.pcap
05:00:00.012800 IP 192.168.10.10.51244 > 203.0.113.10.80: Flags [S], seq 3090211548, win 64240, options [mss 1460], length 0
└── time ─────┘    └─ source ip.port ┘   └ dest ip.port ┘  └flags┘ └ sequence # ┘ └window┘   └── TCP options ┘   └payload┘
```
- **Time:** when the packet was captured, in local time (`-tttt` adds the date; `-ttt` shows the
  gap since the previous packet, which is handy for spotting delays).
- **`>`:** the direction. The left side sent it.
- **Flags:** which TCP control bits are set. See the table below.
- **seq / ack:** byte counters. tcpdump prints the real numbers on the SYN, then switches to
  **relative** numbers starting at 1 (`seq 1:79` = bytes 1-78 of this connection). `-S` keeps them absolute.
- **win:** how many more bytes the receiver can accept right now (flow control).
- **length:** the payload size, not counting headers. `length 0` is a pure control packet.

| tcpdump | Flags | Meaning |
|---|---|---|
| `[S]` | SYN | "let's connect": the first packet of every TCP connection |
| `[S.]` | SYN + ACK | "OK, let's connect": the server's half of the handshake |
| `[.]` | ACK | an acknowledgement with no data, or a data segment without PSH |
| `[P.]` | PSH + ACK | data. PSH means "hand this to the app now" |
| `[F.]` | FIN + ACK | "I'm done sending": a graceful close, one per side |
| `[R]`, `[R.]` | RST | "no such connection / go away": an abrupt reset |

DNS lines have their own format:
```
192.168.10.10.53124 > 1.1.1.1.53: 19754+ A? www.example.com. (33)
1.1.1.1.53 > 192.168.10.10.53124: 19754 1/0/0 A 203.0.113.10 (49)
```
`19754` is the query ID that pairs a reply with its question, `+` means recursion was
requested, and `A?` asks for an IPv4 address. `1/0/0` counts the answer, authority and
additional records, and `(33)` is the DNS message size.

### The options you'll actually use
| Option | Does |
|---|---|
| `-i ens5` / `-i any` | capture on this interface (`-D` lists them; `any` is Linux-only) |
| `-n` | don't turn IPs into hostnames. **Always use it**: lookups are slow and generate DNS traffic of their own |
| `-c 20` | stop after 20 packets |
| `-w file.pcap` | save raw packets to a file instead of printing (then open it in Wireshark) |
| `-r file.pcap` | read a saved file (no root needed) |
| `-e` | also print the Layer 2 header (MAC addresses) |
| `-v`, `-vv` | more detail: TTL, IP ID, checksums, options |
| `-A` / `-X` / `-XX` | print the payload as ASCII / hex+ASCII / hex+ASCII including the Ethernet header |
| `-tttt`, `-ttt` | full date and time / time since the previous packet |
| `-S` | absolute TCP sequence numbers |

### Capture filters (BPF)
The expression at the end of the command decides which packets get captured at all. The
kernel applies it, so filtered-out packets cost almost nothing. Always quote it.
```bash
'host 1.1.1.1'                       # to or from this IP
'src host 192.168.10.10'             # only from it
'net 192.168.10.0/24'                # a whole subnet
'port 53'  'tcp port 443'  'udp'     # by port / protocol
'icmp'  'arp'
'host 203.0.113.10 and tcp port 80'  # combine with and / or / not, group with ( )
'not port 22'                        # everything except SSH (see below)
'tcp[tcpflags] & (tcp-syn|tcp-rst) != 0'   # only SYN and RST packets: connection attempts and resets
```

### Capturing live on the EC2 box
```bash
ip -br addr                       # find the interface with your 172.31.x.x address (often ens5)
sudo tcpdump -ni ens5 -c 20 'not port 22'
```
In a second SSH session, generate some traffic: `curl -s http://example.com > /dev/null`.

**Why `not port 22`?** You're connected over SSH, and tcpdump's output travels back to you over
that SSH connection. Every line printed creates SSH packets, which get captured and printed, which
creates more SSH packets: a feedback loop that floods your screen. Excluding port 22 breaks it.

Saving to a file on Ubuntu:
```bash
sudo tcpdump -ni ens5 -w ~/first.pcap -Z $USER 'not port 22'      # Ctrl-C to stop
```
`-Z $USER` matters. For safety, tcpdump drops root right after opening the interface, and
creates the output file *after* that. On Ubuntu it becomes the `tcpdump` user by default, which
isn't allowed to write in your home directory, so you get `Permission denied`. `-Z $USER` makes
it become you instead, and you own the file, which `scp` needs in part 3.

### Capturing live on the Mac
```bash
ifconfig -l                                   # interfaces: en0 is usually Wi-Fi, lo0 loopback
sudo tcpdump -ni en0 -c 20 'tcp port 443'
sudo tcpdump -ni pktap,en0 -k NP -c 20        # macOS-only: show which process sent each packet
```
`-k` is an Apple addition that prints packet metadata. `N` is the process name and `P` its
PID. It's very handy for "what on my laptop is talking to that IP?"

---

## Part 3: Wireshark on the Mac
tcpdump is great on a server you've SSH'd into. Wireshark is better for *understanding* a
capture: it decodes hundreds of protocols, shows every field, and reassembles conversations.
The usual workflow is **capture with tcpdump on the server, analyze in Wireshark on your laptop**.

### Getting a capture from EC2 to the Mac
```bash
# On the Mac. Use the same key and host you SSH with.
scp -i ~/.ssh/<your-key>.pem ubuntu@<ec2-public-dns>:first.pcap ~/Downloads/
open -a Wireshark ~/Downloads/first.pcap
```
Or skip the file and stream packets straight into Wireshark while they happen. `-U` flushes
each packet immediately, and `-w -` writes the pcap to stdout:
```bash
ssh -i ~/.ssh/<your-key>.pem ubuntu@<ec2-public-dns> "sudo tcpdump -ni ens5 -U -w - 'not port 22'" \
  | /Applications/Wireshark.app/Contents/MacOS/Wireshark -k -i -
```
This works because Ubuntu on EC2 allows `sudo` without a password. If it prompts for one, use
the scp route.

Opening files needs no special permissions. To capture live *in* Wireshark on the Mac, its
installer adds a helper ("ChmodBPF") that lets your user read the capture devices. If
Wireshark lists no interfaces, re-run "Install ChmodBPF" from the Wireshark disk image.

### The window
- **Packet list (top):** one row per packet, like tcpdump's lines. Colors are hints: light
  purple for TCP, light blue for UDP, black with red text for problems (resets, retransmissions).
- **Packet details (middle):** the selected packet as a tree, one branch per layer: `Frame`,
  `Ethernet II`, `Internet Protocol`, `Transmission Control Protocol`, then the application.
  This is Part 1's envelopes, unfolded. Expand a branch to see every field.
- **Packet bytes (bottom):** the raw hex. Click a field in the details and its bytes are
  highlighted here.

### Display filters ≠ capture filters
Wireshark's filter bar uses a **different language** from tcpdump. Capture filters (BPF)
decide what gets recorded. Display filters only hide packets that are already captured, and
they can reach into any decoded field.

| Want | tcpdump capture filter | Wireshark display filter |
|---|---|---|
| one host | `host 1.1.1.1` | `ip.addr == 1.1.1.1` |
| one port | `tcp port 80` | `tcp.port == 80` |
| DNS | `port 53` | `dns` |
| ARP | `arp` | `arp` |
| new connections | `'tcp[tcpflags] == tcp-syn'` | `tcp.flags.syn == 1 && tcp.flags.ack == 0` |
| resets | `'tcp[tcpflags] & tcp-rst != 0'` | `tcp.flags.reset == 1` |
| failed lookups | (not possible) | `dns.flags.rcode == 3` |
| anything TCP thinks is wrong | (not possible) | `tcp.analysis.flags` |
| HTTP requests | (not possible) | `http.request` |

Tip: right-click any field in the details pane → **Apply as Filter**, and Wireshark writes the
filter for you. Right-click → **Apply as Column** adds that field as a column in the packet list.

### Four features worth learning first
- **Follow → TCP Stream** (right-click a packet): the whole conversation as text, client in red,
  server in blue. It's the fastest way to read an HTTP exchange.
- **Statistics → Conversations:** every pair of endpoints, at each layer (Ethernet, IPv4, TCP,
  UDP), with packets, bytes and duration. It's your "who talked to whom" map.
- **Statistics → Flow Graph:** a ladder diagram of packets between hosts over time, like the
  handshake drawings in textbooks.
- **Analyze → Expert Information:** Wireshark's list of things that look wrong (resets,
  retransmissions, zero windows, malformed packets), grouped by severity.

Wireshark shows TCP sequence numbers **relative** (starting at 0) by default, like tcpdump's
relative numbers. **View → Time Display Format → Seconds Since Previous Displayed Packet** is the
quickest way to see where time is spent.

For the command line, Wireshark ships `tshark` (same decoders, text output) inside the app:
`/Applications/Wireshark.app/Contents/MacOS/tshark -r practice.pcap`.

---

## Exercises
Exercises 1-4 use `practice.pcap` on the Mac. Run them from this folder. Exercise 5 needs EC2.

### 1. Find the three addresses in one packet
**Concept:** a packet is just bytes. The headers sit at fixed offsets, and tools like tcpdump
do nothing more magical than reading them in order.

**Try it:** print the HTTP request (the only packet to port 80 with PSH set) in hex, Ethernet
header included:
```bash
tcpdump -nr practice.pcap -XX 'tcp dst port 80 and tcp[tcpflags] & tcp-push != 0'
```
**What you should see:**
```
	0x0000:  0200 0a00 0001 0200 0a00 000a 0800 4500  ..............E.
	0x0010:  0076 1c4c 4000 4006 1779 c0a8 0a0a cb00  .v.L@.@..y......
	0x0020:  710a c82c 0050 b830 e2dd 47f9 7dca 5018  q..,.P.0..G.}.P.
	0x0030:  01f6 ff1f 0000 4745 5420 2f20 4854 5450  ......GET./.HTTP
```
Mark these on the dump:
- bytes 0-5: `0200 0a00 0001`, the destination MAC (the gateway)
- bytes 6-11: `0200 0a00 000a`, the source MAC (the laptop)
- bytes 12-13: `0800`, EtherType IPv4
- byte 14: `45`, IP version 4 with a 5×4 = 20-byte header
- byte 22: `40`, TTL 64. Byte 23: `06`, protocol TCP
- bytes 26-29: `c0a8 0a0a`, the source IP 192.168.10.10. Bytes 30-33: `cb00 710a`, the destination IP 203.0.113.10
- bytes 34-35: `c82c`, source port 51244. Bytes 36-37: `0050`, destination port 80
- byte 54 onward: `4745 5420...`, which is "GET " in ASCII

**Why:** the line also shows `seq 3090211549:3090211627` instead of `seq 1:79`. tcpdump can
only print relative numbers when it has seen the connection's SYN, and this filter hid it.

**Check yourself:** convert `c0a8 0a0a` to dotted decimal yourself. What would the bytes of
`10.0.0.1` look like?
<details><summary>Answer</summary>c0=192, a8=168, 0a=10, 0a=10, so 192.168.10.10. 10.0.0.1 is
<code>0a00 0001</code>.</details>

### 2. MAC addresses stop at the router
**Concept:** Layer 2 is local, Layer 3 is end to end. The destination MAC tells you the next
hop, and the destination IP tells you where the packet is ultimately going.

**Try it:**
```bash
tcpdump -ner practice.pcap | cut -c 17-80
```
**What you should see:** every packet to 1.1.1.1 and to 203.0.113.10 has destination MAC
`02:00:0a:00:00:01`, the gateway, the same MAC that answered the ARP request. Every reply
arrives *from* that MAC, whichever server sent it. The laptop's frames never carry a server's MAC.

**Why:** neither server is on 192.168.10.0/24, so the laptop hands everything to its gateway.
The laptop can't even learn the servers' MACs: they're on other networks, and ARP broadcasts
don't cross routers.

**Check yourself:** if the laptop pinged 192.168.10.20 (another host on the same LAN), what
would the ARP request and the destination MAC be?
<details><summary>Answer</summary>It would ARP for 192.168.10.20 itself ("who-has 192.168.10.20")
and send the ping straight to that host's MAC. No gateway is involved inside a subnet.</details>

### 3. Read the whole story with tcpdump
**Concept:** troubleshooting is mostly reading one conversation at a time and asking "what was
expected next, and did it happen?" Filters isolate the conversation, and timestamps show
where the time went.

**Try it:**
```bash
tcpdump -nr practice.pcap 'udp port 53'                         # every DNS question and answer
tcpdump -nr practice.pcap 'tcp port 80'                         # one complete HTTP connection
tcpdump -nr practice.pcap 'tcp[tcpflags] & (tcp-syn|tcp-rst) != 0'   # connection attempts and resets
tcpdump -nr practice.pcap -ttt 'tcp port 80'                    # gaps between packets
```
Answer from the output:
1. How long did the DNS lookup take? (the reply's time minus the query's time)
2. How long did the TCP handshake take (SYN → SYN-ACK)? That's one **round-trip time (RTT)** to the server.
3. Who closed the HTTP connection first? Why did that side close?
4. What went wrong in the last two conversations?

**What you should see:**
1. 11 ms (`.001200` → `.012200`).
2. 24 ms (`.012800` → `.036800`).
3. The server: its `[F.]` comes first. Its response carried `Connection: close` (see it with `tcpdump -nr practice.pcap -A 'tcp src port 80'`).
4. `NXDomain`: the name `wwww.example.com` doesn't exist. Then a `[S]` to port 8443 answered by `[R.]`: the port is closed.

**Why:** in the healthy connection every step gets its expected reply. The two failures each
end with an explicit "no" from the other side, which is the easy case. A firewall that
silently drops packets gives no answer at all, and you'd see the SYN repeated with growing gaps
(casebook case 06).

**Check yourself:** the RST to port 8443 came back after 24 ms, the same as the web server's
handshake. What does that tell you?
<details><summary>Answer</summary>The host itself is up and reachable: the reset came from its
kernel, one RTT away, because nothing listens on 8443. A reset arriving much faster than the
RTT would hint that something closer (a firewall or proxy) sent it instead.</details>

### 4. The same capture in Wireshark
**Concept:** Wireshark decodes every field tcpdump summarizes, and it tracks conversations, so
it can point at problems for you.

**Try it:** `open -a Wireshark practice.pcap`, then:
1. Click packet 8 and expand `Ethernet II`, `Internet Protocol Version 4` and
   `Transmission Control Protocol`. Find the MACs, the TTL and the ports from exercise 1. Click
   each one and watch its bytes highlight in the bottom pane.
2. Type these into the display filter bar one at a time (press Enter): `arp`, `dns`,
   `tcp.port == 80`, `tcp.flags.reset == 1`, `dns.flags.rcode == 3`.
3. Right-click packet 8 → **Follow → TCP Stream**. Read the request and the HTML response. Close it.
4. Open **Statistics → Flow Graph** and tick "Limit to display filter" with `tcp.port == 80`.
5. Open **Statistics → Conversations** and look at the IPv4 and TCP tabs.
6. Select the NXDOMAIN response (packet 16). Expand `Domain Name System → Authoritative nameservers`
   and find the SOA's "Minimum TTL".

**What you should see:** the Flow Graph draws the handshake, the request, the response and the
teardown as arrows between the two hosts. Conversations lists two IPv4 pairs (with 1.1.1.1 and
with 203.0.113.10) and two TCP conversations (ports 80 and 8443). The SOA's minimum TTL is
3600 seconds.

**Why:** the 3600 is the **negative-cache time**: resolvers will remember "wwww.example.com does
not exist" for an hour, even if someone creates the name a minute later.

**Check yourself:** packet 18 shows in Wireshark as `Seq=1 Ack=1`, but tcpdump printed
`seq 0, ack 2811004371`. Which is right?
<details><summary>Answer</summary>Both. Wireshark shows numbers relative to each side's starting
sequence number, and tcpdump shows the raw values. Turn relative numbers off under
Preferences → Protocols → TCP → "Relative sequence numbers" to see the raw ones in Wireshark.</details>

### 5. Capture your own traffic on EC2 and analyze it on the Mac
**Concept:** a real capture contains things the practice file doesn't: TLS encryption,
TCP options, extra packets and background noise. Reading one end to end is the real skill.

**Try it:**
```bash
# On EC2, SSH session 1:
ip -br addr; ip route; ip neigh       # your IP, your gateway, the gateway's MAC
sudo tcpdump -ni ens5 -w ~/first.pcap -Z $USER 'not port 22'
# On EC2, SSH session 2:
curl -s http://example.com > /dev/null; curl -s https://example.com > /dev/null
# Session 1: Ctrl-C. Then on the Mac:
scp -i ~/.ssh/<your-key>.pem ubuntu@<ec2-public-dns>:first.pcap ~/Downloads/
open -a Wireshark ~/Downloads/first.pcap
```
Find and write down:

| Question | Your answer |
|---|---|
| Your instance's MAC and private IP (`ip -br link`, `ip -br addr`) | [X] |
| The gateway's IP and MAC (`ip route`, `ip neigh`) | [X] |
| Destination MAC on your packets to example.com: is it the gateway's? | [X] |
| DNS: which resolver did EC2 ask, and how long did it take to answer? | [X] |
| Handshake RTT to example.com, port 80 vs port 443 | [X] |
| TTL on the server's replies, and the hop count it implies | [X] |
| The `Server Name Indication` in the TLS Client Hello (filter: `tls.handshake.type == 1`) | [X] |

**What you should see:** the HTTP connection reads like practice.pcap. The HTTPS one shows a
handshake, then `Client Hello` and `Server Hello`, then only "Application Data": the page
itself is encrypted. The DNS resolver is your VPC's resolver, not 1.1.1.1: usually the VPC
base address plus 2, or 169.254.169.253.

**Why:** TLS encrypts everything after the handshake, but the hostname you asked for travels in
the clear in the Client Hello (SNI). That's how proxies and firewalls (and mini-tcpdump) can
tell which site an HTTPS connection is for.

**Check yourself:** the SYN from your instance says `mss 8961`, not 1460. Why would an EC2
instance advertise such a large segment size?
<details><summary>Answer</summary>Within a VPC, most current instance types use <b>jumbo frames</b>
with an MTU of 9001 bytes. MSS = MTU − 20 (IP) − 20 (TCP) = 8961. Traffic leaving the VPC to the
internet still gets limited to about 1500 along the way. That's exactly where MTU problems
come from (netns-router exercise 4, casebook case 03). If your instance shows 1460, check
<code>ip link</code>: its MTU is 1500.</details>

---

## What's next
- **socket-basics (1.0b):** write the clients and servers that make this traffic, then capture them.
- **netns-router (1.1):** build your own LAN, router and gateway, and watch ARP, MACs and TTLs change across it.
- **echo-server (1.2):** write the program at one end of a TCP connection and capture its handshake.
- **mini-tcpdump (1.4):** write your own tcpdump in C++. Reading this chapter's hex dump is its starting point.

## Known limitations
- `practice.pcap` is idealized: no TCP timestamps, SACK or window scaling options, no IPv6, and
  no background noise. Use exercise 5 for real traffic.
- The EC2 instructions assume Ubuntu and an interface named `ens5`. Check yours with `ip -br addr`.
- The Mac commands were written against the built-in macOS tcpdump and Wireshark 4.2. Menu names can move between Wireshark versions.
