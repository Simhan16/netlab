# 02-dns-nxdomain

**Label:** `dns_nxdomain`

## Root cause
DNS works but the name doesn't exist: a typo, a missing record, or a split-horizon zone queried from the wrong side.

## Expected signature in the mini-tcpdump summary
- `dns.failures` shows the name with rcode NXDOMAIN, answered in milliseconds
- Usually both A and AAAA queries fail
- No TCP flow to the target

## Real-world fix
Fix the hostname in config or create the record. Check search domains if the name is short.

## Observed
<!-- Fill in after `sudo ./run_case.sh 02-dns-nxdomain`: paste the relevant lines from
`mini-tcpdump -r pcaps/02-dns-nxdomain/run1.pcap -q` and note anything that differed from the expected signature. -->
