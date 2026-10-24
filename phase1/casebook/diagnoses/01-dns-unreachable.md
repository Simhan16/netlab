# 01-dns-unreachable

**Label:** `dns_unreachable`

## Root cause
The configured resolver doesn't respond (wrong IP, resolver down, or UDP/53 blocked). The app fails before it ever opens a TCP connection.

## Expected signature in the mini-tcpdump summary
- `dns.unanswered` lists the same name, several attempts, to one server
- `dns.responses` is 0 (for that server)
- No TCP flow to the target exists at all, which is the key tell that this isn't a TCP problem

## Real-world fix
Point resolv.conf (or DHCP option 6) at a working resolver; check that UDP and TCP 53 are allowed outbound.

## Observed
<!-- Fill in after `sudo ./run_case.sh 01-dns-unreachable`: paste the relevant lines from
`mini-tcpdump -r pcaps/01-dns-unreachable/run1.pcap -q` and note anything that differed from the expected signature. -->
