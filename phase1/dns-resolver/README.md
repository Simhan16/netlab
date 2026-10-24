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

## Exercises
These work on any machine with internet access. Run them from `phase1`.

### 1. Ask a root server yourself
**Concept:** DNS is a tree of **delegations**. The root knows who runs `.com`, the `.com`
servers know who runs `example.com`, and only that domain's own servers know the answer.
A server that doesn't have the answer returns a **referral**: NS records saying "ask them",
plus **glue**, the A records of those nameservers.

**Try it:**
```bash
./build/dns-resolver/dnsq @198.41.0.4 example.com +norec     # a.root-servers.net, no recursion
./build/dns-resolver/dnsq example.com +trace                 # let dnsq walk the whole tree
```
**What you should see:** the root's reply has `ANSWER: 0`, 13 `com. NS ...gtld-servers.net.`
records in AUTHORITY, and their addresses in ADDITIONAL. The flags have no `aa`
(authoritative) and no `ra`: root servers don't recurse. `+trace` then shows three hops:
root → a `.com` server → example.com's own nameservers, which finally answer.

**Why:** `+norec` clears the RD (recursion desired) bit, so you get exactly what a
recursive resolver like 1.1.1.1 sees on each step when it resolves a name for you.

**Check yourself:** why is glue necessary? Think about a domain whose nameserver is
`ns1.example.com`.
<details><summary>Answer</summary>To find ns1.example.com's address you'd have to ask
example.com's nameserver, which is ns1.example.com. That's circular. The parent zone breaks the
loop by including the address (glue) with the referral.</details>

### 2. Watch UDP fall back to TCP
**Concept:** classic DNS over UDP is limited to **512 bytes**. A bigger answer comes back
cut short with the **TC (truncated)** bit set, and the client must retry over TCP. TCP has
no message boundaries, so each DNS message is prefixed with a 2-byte length.

**Try it:**
```bash
sudo tcpdump -ni any 'host 198.41.0.4 and port 53'           # terminal 1
./build/dns-resolver/dnsq @198.41.0.4 example.com +norec     # terminal 2
```
**What you should see:** `;; truncated (TC=1), retrying over TCP`. In tcpdump: one UDP query
and a short reply, then a full TCP handshake to port 53, the query again, and a larger reply.

**Why:** 13 NS records plus 26 glue addresses don't fit in 512 bytes. `dnsq` doesn't send
EDNS0, the extension that advertises a bigger UDP buffer. So it hits the classic limit.

**Check yourself:** why does `dig @198.41.0.4 example.com +norec` usually *not* fall back to TCP?
<details><summary>Answer</summary>dig sends an EDNS0 OPT record advertising a ~1232-byte (or
larger) buffer, so the full referral fits in one UDP reply. Adding EDNS0 to dnsq is a good
extension exercise.</details>

### 3. "Doesn't exist" comes in two flavors
**Concept:** **NXDOMAIN** means the *name* doesn't exist. **NODATA** (NOERROR with zero
answers) means the name exists but has no record of that *type*. Both include the zone's
**SOA** record. Its last number is how long resolvers may cache the negative answer.

**Try it:**
```bash
./build/dns-resolver/dnsq @1.1.1.1 doesnotexist-48213.wikipedia.org   # NXDOMAIN
./build/dns-resolver/dnsq @1.1.1.1 www.example.com MX                 # NODATA
./build/dns-resolver/dnsq @1.1.1.1 doesnotexist-48213.example.com     # surprise
```
**What you should see:** the first is `status: NXDOMAIN` with `wikipedia.org. SOA ...` in
AUTHORITY. The second is `status: NOERROR`, `ANSWER: 0`, with an SOA. The third, a name that
certainly doesn't exist, also says NOERROR.

**Why:** applications treat these differently. NXDOMAIN means "stop, wrong name"; NODATA means
"try another type" (e.g. A after AAAA). The third case is a DNSSEC technique some providers,
including Cloudflare, use: answering NODATA for nonexistent names so they don't have to
prove non-existence with signed records.

**Check yourself:** a user typo'd a hostname and DNS answered NXDOMAIN. They fix the typo,
but a different misspelling they tried earlier still fails for an hour after the record is
added. Why?
<details><summary>Answer</summary>Negative caching. Resolvers keep the NXDOMAIN for the SOA's
negative TTL (the last SOA field, 3600 s for wikipedia.org above), even after the name starts
existing.</details>

### 4. Why the ID check matters
**Concept:** UDP has no handshake, so anyone can send a packet *claiming* to come from your DNS
server. A resolver accepts the first reply that matches its query's 16-bit **ID**, source
**port** and question. Off-path cache poisoning (the 2008 Kaminsky attack) is a race to forge
that match first.

**Try it:** read `matches()` and `query_udp()` in `src/main.cpp`. List every check a forged
reply must pass. Then comment out the ID comparison, rebuild, and ask yourself what an
attacker now has to guess.

**Why:** the ID gives 65,536 possibilities, and a random source port multiplies that by about
28,000. Before 2008, many resolvers used a fixed source port, so 1 forged packet in 65,536
won. Attackers could send thousands per second.

**Check yourself:** with ID and port both random, about how many guesses does an attacker need
for a 50% chance?
<details><summary>Answer</summary>About half of 65,536 × 28,000 ≈ 1.8 billion combinations,
so roughly 900 million forged packets, all arriving before the real reply. Randomization
makes blind forging impractical, not impossible. DNSSEC closes the gap by signing answers, so a
forged reply fails validation however well it's guessed.</details>

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
