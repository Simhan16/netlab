# dns-resolver (`dnsq`)

A small `dig`. It builds DNS query packets byte by byte, parses responses (including
name compression), retries over TCP when a reply is truncated, and with `+trace` resolves
iteratively from a root server the way Unbound does. Phase 1.3.

The codec (`src/dns.{hpp,cpp}`) is a separate library with no sockets in it.
mini-tcpdump reuses it to decode DNS traffic.

## Build & run
```bash
cd phase1 && cmake -B build && cmake --build build -j
./build/dns-resolver/dnsq example.com                 # system resolver from /etc/resolv.conf
./build/dns-resolver/dnsq @1.1.1.1 example.com MX
./build/dns-resolver/dnsq example.com +trace          # root → .com → example.com's servers
./build/dns-resolver/dnsq @8.8.8.8 example.com +tcp
./build/dns-resolver/test_dns                          # codec unit tests
```
Exit codes: 0 NOERROR, 1 any other rcode, 2 network or usage error.

## What it teaches
- **Wire format.** A 12-byte header (id, flags, 4 counts), then question and resource records. Every integer is big-endian.
- **Name compression.** A name may end in a pointer (`0xC0 | offset`) back into the message. A hostile packet can make pointers loop, so the parser caps the number of jumps. Every read is bounds-checked: `test_dns` truncates a valid reply at every byte offset and requires a clean error each time.
- **Anti-spoofing.** A random query ID, a random source port (from the kernel), a connected UDP socket that only accepts the server's address, and a check that the reply's question matches ours.
- **TC bit → TCP.** Big answers (a root referral with 13 NS records and glue, DNSSEC) don't fit in UDP. TCP framing adds a 2-byte length prefix.
- **Iterative resolution.** No RD bit: follow NS referrals plus glue from ADDITIONAL, resolve nameserver names that have no glue, and chase CNAMEs.

## Sample output
Run on macOS while building this (the trace shows the TCP fallback from a root server):
```
$ dnsq example.com +trace
;; truncated (TC=1), retrying over TCP
example.com A @198.41.0.4 -> NOERROR, 0 answer(s), 13 NS (33.8 ms)
example.com A @192.41.162.30 -> NOERROR, 0 answer(s), 2 NS (9.3 ms)
example.com A @108.162.192.162 -> NOERROR, 2 answer(s), 0 NS (6.7 ms)

example.com A 172.66.147.243
example.com A 104.20.23.154
```

## Known limitations
- No EDNS0 (OPT record), so UDP answers are capped at 512 bytes and truncate more often than they would with `dig`.
- No DNSSEC validation and no caching.
- `+trace` uses IPv4 root/glue addresses only, with a hard-coded subset of root servers.
- RDATA decoding covers A, AAAA, NS, CNAME, PTR, MX, TXT and SOA. Other types print in RFC 3597 `\# len hex` form.
