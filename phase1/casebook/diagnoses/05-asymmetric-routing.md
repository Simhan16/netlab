# 05-asymmetric-routing

**Label:** `asymmetric_routing`

## Root cause
Requests and replies take different paths. A stateful firewall or reverse-path filter on the return path drops replies it never saw the request for (or that arrive on the 'wrong' interface).

## Expected signature in the mini-tcpdump summary
- From the server's view: repeated SYNs from the client (`syns` > 1) *and* repeated SYN-ACKs (`synacks` > 1)
- State `synack_unacked`: the server answers, but the client never ACKs
- The client's own capture would show `syn_unanswered`. Same failure, different vantage point.

## Real-world fix
Make routing symmetric (fix the extra route), or relax rp_filter to loose mode (2) where asymmetry is intentional.

## Observed
<!-- Fill in after `sudo ./run_case.sh 05-asymmetric-routing`: paste the relevant lines from
`mini-tcpdump -r pcaps/05-asymmetric-routing/run1.pcap -q` and note anything that differed from the expected signature. -->
