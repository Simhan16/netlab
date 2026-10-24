# 07-connection-refused

**Label:** `connection_refused`

## Root cause
The host is reachable but nothing listens on the port: service down, crashed, bound to localhost only, or wrong port.

## Expected signature in the mini-tcpdump summary
- State `refused`: SYN answered immediately by RST/ACK from the server
- Round trip is fast, which proves the path is fine
- Client usually retries once or twice and gives up

## Real-world fix
Start the service / fix the port / bind to 0.0.0.0. Check `ss -ltnp` on the server.

## Observed
<!-- Fill in after `sudo ./run_case.sh 07-connection-refused`: paste the relevant lines from
`mini-tcpdump -r pcaps/07-connection-refused/run1.pcap -q` and note anything that differed from the expected signature. -->
