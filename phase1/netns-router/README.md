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

## Exercises (good tcpdump practice for 1.4)
1. Run `tcpdump -ni r-b` in the router while lan-b pings lan-a. The requests arrive and nothing comes back. Then watch the DROP counter climb in `iptables -L FORWARD -v`.
2. Run `tcpdump -ni ens5 icmp` on the host while lan-a pings 1.1.1.1. The source is the instance IP, not 192.168.10.10. That's NAT.
3. Delete the host return route (`ip route del 192.168.10.0/24`) and see which hop the replies die at.
4. Set the MTU on `r-up` to 1000 and try `curl` again. This is a preview of the Casebook's MTU black hole.

## Known limitations
- The firewall filters only FORWARD. lan-b can still ping the router's lan-a-side address (192.168.10.1), because packets to the router itself go through INPUT. Try it and explain why.
- IPv4 only. No DHCP: addresses are static.
- Uses `iptables` (the nft backend on modern Ubuntu). The nftables rewrite is a good follow-up.
- If your VPC or another tool already uses 192.168.10/20/255.x, edit `config.sh`.
- The host rules are inserted at the top of FORWARD to get past ufw/docker DROP policies. Teardown removes exactly those rules.
