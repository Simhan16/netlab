# http-proxies: HTTP, HTTPS and the proxies in between

Step 1.3b, after dns-resolver. Most traffic an FDE debugs at a customer is HTTP, and it almost
never goes straight from client to server: corporate **forward proxies**, **load balancers**,
**CDNs** and **API gateways** sit in the path, and each can rewrite, block, slow down or answer
on the server's behalf. "The API returns 502" is a question about *which hop* said so.

This chapter has three parts:
1. **HTTP itself**: the wire format, framing, connections, caching, HTTPS and SNI.
2. **Proxies**: the kinds, the request forms they use, what they change, how clients find them, and which errors are theirs.
3. **Build them**: an HTTP/1.1 client, a forward proxy and a reverse proxy, each checked by tests that target classic mistakes.

Everything runs on the Mac (Python 3.11+, `curl`, `nc`, `openssl`) with no root.

```
http-proxies/
├── origin.py      a small HTTP/1.1 server to experiment against (/hello, /headers, /chunked, /cache, /slow...)
├── httpwire.py    finished HTTP plumbing used by steps 2 and 3
├── exercises/     ← you edit these: step1_client.py, step2_forward_proxy.py, step3_reverse_proxy.py
├── solutions/     reference versions. Look after a real attempt.
└── check.py       python3 check.py 1|2|3|all [--solution]
```
Start the origin server in its own terminal and leave it running: `python3 origin.py` (http://127.0.0.1:8001).
`python3 origin.py --help` lists its endpoints.

---

## Part 1: HTTP
### A request and a response
HTTP/1.1 is text over a TCP connection. The client sends a **request**: a request line, headers,
a blank line, and optionally a body. The server sends back a **response** in the same shape.
Typed by hand with `nc` against origin.py (real output):
```
$ (printf 'GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n'; sleep 1) | nc 127.0.0.1 8001
HTTP/1.1 200 OK                                  ← status line: version, status code, reason
Server: netlab-origin/1.0
Date: Sun, 25 Oct 2026 00:30:51 GMT
X-Served-By: origin
Content-Type: text/plain; charset=utf-8
Content-Length: 18                               ← exactly 18 bytes of body follow the blank line
                                                 ← blank line (\r\n\r\n) ends the head
hello from origin
```
Every line ends in `\r\n`. The request line was `GET /hello HTTP/1.1`, plus one header: **`Host`**.
HTTP/1.1 requires Host, because one IP address commonly serves thousands of websites (virtual
hosting), and Host is the only way the server knows which one you want. Leave it out and origin.py
answers `HTTP/1.1 400 Bad Request`.

### Methods and status codes
| Method | Meaning | Safe to retry? |
|---|---|---|
| `GET` / `HEAD` | fetch (HEAD: headers only) | yes |
| `POST` | submit or create; the server decides what happens | **no**: retrying may charge a card twice |
| `PUT` / `DELETE` | replace / remove a resource | yes (idempotent: doing it twice = once) |
| `CONNECT` | "open a tunnel to host:port": used with proxies | n/a |
| `OPTIONS` | what's allowed here? (CORS preflight) | yes |

| Class | Meaning | Ones you'll see |
|---|---|---|
| 1xx | informational | `101 Switching Protocols` (WebSockets) |
| 2xx | success | `200 OK`, `201 Created`, `204 No Content` |
| 3xx | go elsewhere / use your copy | `301`/`302`/`307`/`308` redirects, `304 Not Modified` |
| 4xx | **the client's** fault | `400` malformed, `401` log in, `403` forbidden, `404`, `407` proxy auth, `429` slow down |
| 5xx | **the server's** fault, *or a proxy's* | `500` bug, `502` bad gateway, `503` unavailable, `504` gateway timeout |

### Headers that matter
| Header | Says |
|---|---|
| `Host` | which website (required in HTTP/1.1) |
| `Content-Length` / `Transfer-Encoding: chunked` | where the body ends (see framing below) |
| `Connection: close` / `keep-alive` | whether this TCP connection carries more requests |
| `Content-Type` | what the body is: `text/html`, `application/json`... |
| `Location` | where a redirect points |
| `Cache-Control`, `ETag`, `If-None-Match`, `Last-Modified` | caching and conditional requests |
| `Set-Cookie` / `Cookie` | state: the server sets a cookie, and the browser sends it back on every request |
| `Authorization` / `Proxy-Authorization` | credentials for the server / for a proxy |
| `Via`, `X-Forwarded-For`, `Forwarded` | proxies announcing themselves and the real client |

### Framing: where does a message end?
TCP is a byte stream (socket-basics step 4), so HTTP must say where each body ends. In order:
1. **No body:** responses to HEAD, `1xx`, `204` and `304`, whatever the headers say.
2. **`Transfer-Encoding: chunked`:** a series of `<size in hex>\r\n<data>\r\n`, ended by a
   zero-size chunk. The server can start sending before it knows the total length (streaming,
   generated pages). origin.py's `/chunked`, as raw bytes (`curl -s --raw http://127.0.0.1:8001/chunked | od -c`):
   ```
   c \r \n f i r s t   c h u n k \n \r \n            ← 0xc = 12 bytes
   1 a \r \n s e c o n d   c h u n k ,   a   b i t   l a t e r \n \r \n     ← 0x1a = 26 bytes
   f \r \n t h i r d   a n d   l a s t \n \r \n       ← 0xf = 15 bytes
   0 \r \n \r \n                                       ← the end
   ```
3. **`Content-Length: N`:** exactly N bytes.
4. **Neither:** the body runs until the server closes the connection.

Getting this wrong doesn't just garble one response: with keep-alive, the *next* response gets parsed
from the wrong byte. (Disagreements between a proxy and a server about where a request ends are exactly
how **request smuggling** attacks work.)

### Connections: 1.0, 1.1, 2 and 3
- **HTTP/1.0:** one request per TCP connection. The server closes it after responding.
- **HTTP/1.1:** **keep-alive** by default, so several requests reuse one connection, saving a TCP (and TLS)
  handshake each time. But requests on one connection are answered strictly in order, so one slow
  response delays the ones behind it (**head-of-line blocking**). Browsers open about 6 connections per site to work around it.
  ```
  $ curl -sv http://127.0.0.1:8001/hello http://127.0.0.1:8001/headers -o /dev/null -o /dev/null
  * Connected to 127.0.0.1 (127.0.0.1) port 8001
  > GET /hello HTTP/1.1
  * Connection #0 to host 127.0.0.1 left intact
  * Re-using existing connection with host 127.0.0.1
  > GET /headers HTTP/1.1
  ```
- **HTTP/2:** the same semantics (methods, headers, status codes) in a **binary** format, with many
  requests **multiplexed** as streams over one TLS connection, and compressed headers. Used by
  almost every HTTPS site: `curl -v https://example.com` shows `ALPN: server accepted h2`.
- **HTTP/3:** HTTP/2's ideas over **QUIC**, a UDP-based transport with TLS built in. A lost packet only stalls
  the stream it belongs to, not the whole connection. Firewalls that block UDP 443 silently push browsers back to HTTP/2.

### State and caching
- **Cookies.** HTTP is stateless. `Set-Cookie: session=abc123` makes the client send
  `Cookie: session=abc123` on later requests, which is how logins persist (`curl -c jar /cookie`, then `curl -b jar /whoami`).
- **Caching.** `Cache-Control: max-age=60` lets any cache (the browser, a proxy, a CDN) reuse the response for 60 s
  without asking. After that, a **conditional request** (`If-None-Match: "v1"`, the `ETag` it got) lets the server
  answer `304 Not Modified` with no body: "your copy is still good".
- **Redirects.** `302 Found` + `Location: /hello` sends the client elsewhere. `curl -L` follows them.

### HTTPS: HTTP inside TLS
HTTPS is the same HTTP, sent inside a **TLS** session on port 443. Before any HTTP, the TLS handshake:
1. The client's **ClientHello** lists its TLS versions and ciphers, the **SNI** (Server Name Indication: the
   hostname it wants) and **ALPN** (which protocols it speaks: `h2`, `http/1.1`).
2. The server picks the certificate for that SNI and proves it holds the private key. The client checks that
   the certificate chains up to a trusted CA and names the host.
3. Keys are agreed, and everything after this, the HTTP request line, headers, cookies and body, is encrypted.

What a network observer, or a proxy that only tunnels, still sees: the IPs, the ports, the **SNI hostname**
(in the clear in the ClientHello, which is how firewalls filter HTTPS by domain), and the sizes and timing of
packets. Not the URL path, the headers or the content.

---

## Part 2: proxies
### The kinds
```
forward proxy (works for the clients)        reverse proxy (works for the servers)

  laptop ─┐                                                     ┌─ app server a
  laptop ─┼──▶ proxy ──▶ the internet       client ──▶ proxy ──┼─ app server b
  laptop ─┘   (Squid, Zscaler)                (nginx, ALB,      └─ app server c
                                               Cloudflare)
```
| Kind | Who configures it | Typical jobs | Examples |
|---|---|---|---|
| **Forward proxy** (explicit) | the client is told to use it | egress control, URL filtering, logging, caching | Squid, Zscaler, Blue Coat |
| **Transparent / intercepting** | the network redirects traffic to it; the client doesn't know | the same, without configuring each client | firewalls doing "web filtering" |
| **Reverse proxy / load balancer** | the website operator | TLS termination, routing by path, spreading load, health checks, caching | nginx, HAProxy, Envoy, AWS ALB |
| **CDN / API gateway** | the operator, as a service | caching near users, DDoS protection, auth, rate limits | Cloudflare, Akamai, Kong |
| **SOCKS proxy** | the client | relays any TCP connection, with no HTTP awareness | `ssh -D`, Tor |

### How a client talks to a forward proxy
A client using a proxy connects to the *proxy*, and has to say where it really wants to go:
- **Plain HTTP: absolute-form.** The request line carries the full URL: `GET http://example.com/page HTTP/1.1`.
  The proxy makes the request itself and relays the response, so it sees and can change everything.
- **HTTPS: `CONNECT` (authority-form).** `CONNECT example.com:443 HTTP/1.1`. The proxy opens a TCP connection,
  answers `200 Connection Established`, and from then on blindly copies bytes both ways. The TLS handshake and
  everything after it go through the tunnel encrypted. The proxy sees only the hostname and port, and the byte counts.

Real `curl -v` through this chapter's proxy (step 2's solution):
```
$ curl -sv -x http://127.0.0.1:3128 http://127.0.0.1:8001/hello
* Connected to 127.0.0.1 (127.0.0.1) port 3128              ← the proxy, not the origin
> GET http://127.0.0.1:8001/hello HTTP/1.1                  ← absolute-form
> Host: 127.0.0.1:8001
> Proxy-Connection: Keep-Alive
< HTTP/1.1 200 OK
< X-Served-By: origin

$ curl -sv -x http://127.0.0.1:3128 https://example.org     ← example.org is on the blocklist
> CONNECT example.org:443 HTTP/1.1
< HTTP/1.1 403 Forbidden
* CONNECT tunnel failed, response 403
```

### What proxies change
- **Hop-by-hop headers** describe one connection, not the message: `Connection`, `Keep-Alive`,
  `Proxy-Connection`, `Proxy-Authorization`, `TE`, `Trailer`, `Transfer-Encoding`, `Upgrade`, and any header
  *named* in `Connection`. A proxy must strip them and use its own.
- **`Via: 1.1 proxyname`** is added by each proxy on the way, a breadcrumb trail.
- **`X-Forwarded-For: client, proxy1, ...`**: behind a reverse proxy, the server's TCP peer is the proxy,
  so the real client's IP travels in this header (and `X-Forwarded-Proto`, `X-Forwarded-Host`, or the
  standard `Forwarded`). Each hop **appends**. Only trust the entries added by proxies you control:
  anyone can send a fake first entry.
- **TLS termination:** a reverse proxy usually decrypts HTTPS, so it can route by path, and talks to the
  backends over plain HTTP or new TLS.
- **TLS interception** (on forward proxies): the proxy completes the client's TLS handshake itself, with
  certificates minted by a company CA installed on every laptop, so it can inspect HTTPS. It's powerful and
  invasive, and it breaks apps that pin certificates.

### How clients find a forward proxy
- **Environment variables:** `http_proxy`, `https_proxy`, `no_proxy` (used by curl, pip, apt, most CLIs).
  Gotchas: curl ignores *uppercase* `HTTP_PROXY` (a safety measure, after the "httpoxy" bug), while `HTTPS_PROXY`
  works in either case. And `no_proxy` matching differs between tools (domains, suffixes, CIDRs).
- **PAC files:** a JavaScript function `FindProxyForURL(url, host)` that browsers fetch and run to pick a proxy
  per URL, sometimes found automatically via **WPAD**.
- **Per-tool flags:** `curl -x http://proxy:3128`, `curl --noproxy '*'`, or app settings.

### Errors, and which hop sent them
| Status | Usually from | Means |
|---|---|---|
| `400` | whichever hop couldn't parse the request | malformed request |
| `403` | the proxy's policy, or the server | forbidden: check whose body or headers it carries |
| `407 Proxy Authentication Required` | a forward proxy | send `Proxy-Authorization` |
| `502 Bad Gateway` | a proxy | it couldn't reach the next hop, or got garbage from it |
| `503 Service Unavailable` | the server, or a proxy with no healthy backends | overloaded or in maintenance |
| `504 Gateway Timeout` | a proxy | the next hop accepted the request but didn't answer in time |

To tell which hop answered, look at the **response headers and body**. A proxy-generated error carries the
proxy's `Server`/`Via` headers and its own error page (this chapter's proxies add `X-Proxy-Error: netlab`),
and lacks the backend's headers (origin.py always sends `X-Served-By`).

---

## Exercises
Run from this folder with `python3 origin.py` running in another terminal.

### 1. Speak HTTP by hand
**Concept:** HTTP/1.1 is plain text, and a server only needs a request line, a Host header and a blank line.

**Try it:**
```bash
(printf 'GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n'; sleep 1) | nc 127.0.0.1 8001
(printf 'GET /hello HTTP/1.1\r\n\r\n'; sleep 1) | nc 127.0.0.1 8001        # no Host
(printf 'GET /hello HTTP/1.0\r\n\r\n'; sleep 1) | nc 127.0.0.1 8001        # HTTP/1.0, no Host
nc 127.0.0.1 8001        # now type it yourself: GET /headers HTTP/1.1, Host: x, then an empty line
```
**What you should see:** a `200` with `hello from origin`. Then `HTTP/1.1 400 Bad Request`. Then a
`200` again, because HTTP/1.0 didn't require Host. In the interactive session, the connection **stays
open** after the response (keep-alive), so you can type a second request.

**Why:** the server's status line says `HTTP/1.1` even when answering a 1.0 request: it reports the highest
version it supports, but must reply in terms the client understands, so it closes the connection
afterwards (1.0 has no keep-alive by default).

**Check yourself:** you type `GET /hello HTTP/1.1` and `Host: x` and press Enter once. Nothing happens. Why?
<details><summary>Answer</summary>The head isn't finished: the server waits for the blank line
(<code>\r\n\r\n</code>) that ends the headers. One more Enter sends it. That blank line is how every HTTP/1.x
parser, including the one you'll write in step 1, finds the end of the head.</details>

### 2. Framing and keep-alive
**Concept:** keep-alive only works if both sides agree, byte for byte, where each message ends.

**Try it:**
```bash
curl -sv http://127.0.0.1:8001/hello http://127.0.0.1:8001/headers -o /dev/null -o /dev/null 2>&1 | grep -E 'Connected|Re-using|^> GET'
curl -s --raw http://127.0.0.1:8001/chunked | od -c | head
curl -sv http://127.0.0.1:8001/status/204 2>&1 | grep -iE '^< (HTTP|content-length)'
```
**What you should see:** `Connected` once and `Re-using existing connection` for the second URL. The raw
chunked body, with hex sizes `c`, `1a` and `f` and a final `0`. The 204 has no Content-Length at all.

**Why:** curl knew where `/hello` ended (Content-Length: 18), so it could send the next request on the same
connection. The chunked response has no length up front: each chunk carries its own size.

**Check yourself:** a server sends a body with **neither** Content-Length nor chunked encoding. Can the client
reuse the connection afterwards?
<details><summary>Answer</summary>No. Rule 4: the body ends only when the server closes the connection, so that
close is the only "end of message" signal, and the connection is gone. This is why HTTP/1.1 servers use chunked
encoding for streamed responses: it keeps the connection reusable.</details>

**Now do build step 1** (below): an HTTP/1.1 client that gets all four framing rules right.

### 3. Caching and conditional requests
**Concept:** caches avoid re-downloading. `Cache-Control` says how long a response stays fresh, and the `ETag`
lets a client ask "has it changed?" cheaply.

**Try it:**
```bash
curl -si http://127.0.0.1:8001/cache | head -8
curl -si -H 'If-None-Match: "v1"' http://127.0.0.1:8001/cache | head -6
curl -si -H 'If-None-Match: "v0"' http://127.0.0.1:8001/cache | head -1
```
**What you should see:** a `200` with `ETag: "v1"` and `Cache-Control: max-age=60`. With the matching
`If-None-Match`: `HTTP/1.1 304 Not Modified` and no body. With a stale ETag: `200` again.

**Why:** the 304 costs one round trip and a few hundred bytes instead of the whole body. CDNs and browsers live on this.

**Check yourself:** a user reports that a page "doesn't update" after a deploy, but works in a private window.
What's the likely cause, and the server-side fix?
<details><summary>Answer</summary>Their browser (or a proxy) is serving a cached copy that's still "fresh" under a
long <code>max-age</code>. A private window has an empty cache. Fixes: a shorter max-age or <code>no-cache</code>
(always revalidate with the ETag) for HTML, and versioned file names (<code>app.3f9c.js</code>) for assets, so a
new deploy means new URLs.</details>

### 4. HTTPS: what TLS hides, and what it doesn't
**Concept:** the hostname travels in the clear (SNI), so the server can pick the right certificate, and so
can every firewall on the path.

**Try it:**
```bash
curl -sv https://example.com -o /dev/null 2>&1 | grep -E 'SSL connection|ALPN|subject|issuer'
echo | openssl s_client -connect example.com:443 -servername example.com 2>/dev/null | grep -E '^subject=|^issuer=|Verify return'
echo | openssl s_client -connect example.com:443 -servername wrong.invalid 2>&1 | grep -E 'alert|^subject='
```
**What you should see** (real output, October 2026):
```
subject=CN = example.com
issuer=C = US, O = SSL Corporation, CN = Cloudflare TLS Issuing ECC CA 3
Verify return code: 0 (ok)
...ssl3_read_bytes:sslv3 alert handshake failure:...:SSL alert number 40
```
curl reports TLS 1.3 and `ALPN: server accepted h2`. With the wrong SNI, Cloudflare aborts the handshake:
it has no certificate for that name.

**Why:** one IP serves many TLS sites, and the SNI is how it chooses. It's also what makes HTTPS filterable
by domain, and what mini-tcpdump (1.4) and PacketWhisperer extract from captures.

**Check yourself:** a firewall blocks `*.example.org` for HTTPS without any certificate tricks. What is it looking
at, and how could a client get around it?
<details><summary>Answer</summary>The SNI in the ClientHello (and often the DNS lookup before it). A client can dodge
it by sending a different or empty SNI to a server that accepts that, or by using <b>Encrypted Client Hello</b> (ECH),
which encrypts the SNI. That's why ECH is controversial with network operators.</details>

### 5. A forward proxy in action
**Concept:** with a proxy configured, the client never connects to the website: plain HTTP goes as an
absolute-form request the proxy re-issues, and HTTPS goes through a CONNECT tunnel.

**Try it** (step 2's solution, until yours passes its checks):
```bash
python3 solutions/step2_forward_proxy.py 3128 --block example.org        # terminal 2
curl -s -x http://127.0.0.1:3128 http://127.0.0.1:8001/headers           # what the origin received
curl -sv -x http://127.0.0.1:3128 https://example.com -o /dev/null 2>&1 | grep -E 'CONNECT|Established|ALPN'
curl -sv -x http://127.0.0.1:3128 https://example.org -o /dev/null 2>&1 | grep -E '^< HTTP|CONNECT tunnel failed'
https_proxy=http://127.0.0.1:3128 curl -s -o /dev/null -w '%{http_code}\n' https://example.com   # via the environment
```
**What you should see:** in `/headers`, the request line `GET /headers HTTP/1.1` (rewritten from absolute form),
`Via: 1.1 netlab-proxy`, and no `Proxy-Connection`. For HTTPS, `CONNECT example.com:443`, then
`200 Connection Established`, then a normal TLS handshake *through* the tunnel, with h2 negotiated end to end. For the
blocked site, `403` and `CONNECT tunnel failed`. Terminal 2 logs each request.

**Why:** for HTTPS the proxy logged `CONNECT example.com:443` and nothing more. Paths, headers and content were
encrypted inside the tunnel. URL filtering on HTTPS without interception is domain-level only.

**Check yourself:** the enterprise lab's laptops have `https_proxy` set, and its firewall blocks their direct web
traffic. A developer's tool ignores the environment variable. What do they see, and why is that the right outcome?
<details><summary>Answer</summary>Connection timeouts: the tool tries to reach the site directly, and the firewall
drops it. That's the point of pairing the proxy with a firewall rule. The fix is to configure the tool's own proxy
setting, not to open the firewall (enterprise exercise 4).</details>

**Now do build step 2.**

### 6. A reverse proxy and load balancer
**Concept:** to the client, the reverse proxy *is* the website. Behind it, requests are spread over backends,
and failed backends are skipped.

**Try it:**
```bash
python3 origin.py --port 8002 --name b                                    # terminal 2: a second backend
python3 solutions/step3_reverse_proxy.py 8080 --backend /=127.0.0.1:8001 --backend /=127.0.0.1:8002 --timeout 1   # terminal 3
for i in 1 2 3 4; do curl -s http://127.0.0.1:8080/hello; done
curl -s -H 'X-Forwarded-For: 203.0.113.7' http://127.0.0.1:8080/headers | grep -i forwarded
curl -s 'http://127.0.0.1:8080/slow?s=3'
# now stop backend b (Ctrl-C in terminal 2), and repeat the loop
```
**What you should see:** `hello from origin`, `hello from b`, alternating. The backend receives
`X-Forwarded-For: 203.0.113.7, 127.0.0.1`, plus `X-Forwarded-Proto` and `X-Forwarded-Host`. The slow request
gets `504 Gateway Timeout: ... did not answer within 1 s`. With b stopped, every request is served by origin,
and the proxy's log shows it skipping b.

**Why:** a refused connection means the request was never sent, so retrying it elsewhere is safe for any method.
A timeout after sending isn't retried: the backend may still be processing it, and for a POST, doing it twice is a bug.

**Check yourself:** an app behind this proxy logs every client as `127.0.0.1`. What's wrong, and why is just
trusting `X-Forwarded-For` dangerous?
<details><summary>Answer</summary>The app logs its TCP peer, which is the proxy. It should read the client from
X-Forwarded-For. But any client can send its own X-Forwarded-For with a fake address. Since the proxy
<i>appends</i>, the trustworthy entry is the one your own proxy added: the last one (or last N, with N trusted
proxies). Frameworks call this "trusted proxies" (nginx <code>set_real_ip_from</code>, Express <code>trust proxy</code>).</details>

**Now do build step 3.**

### 7. Who sent this error?
**Concept:** in a chain of client → proxy → load balancer → app, a 5xx can come from any hop, and the first
question is always *which hop generated it*.

**Try it** (with both proxies from exercises 5 and 6 running):
```bash
curl -si -x http://127.0.0.1:3128 http://127.0.0.1:9/ | head -8               # nothing listens on port 9
curl -si http://127.0.0.1:8001/status/502 | head -6                             # an app that *returns* 502
curl -si 'http://127.0.0.1:8080/slow?s=3' | head -8
```
**What you should see:** the first is `502 Bad Gateway` with `X-Proxy-Error: netlab` and a body explaining that
the proxy couldn't connect, and no `X-Served-By`. The second is a 502 *with* `X-Served-By: origin`: the
application itself said it. The third is a 504 from the reverse proxy.

**Why:** status codes alone don't say who is talking. Headers do: a proxy's error page has the proxy's headers, and
lacks the backend's. Real proxies differ (nginx says `Server: nginx`, ALB adds its own body, Cloudflare shows a
branded page with a Ray ID), but the method is the same.

**Check yourself:** users see intermittent 502s from a load balancer in front of three app servers. List two very
different root causes the 502 could mean.
<details><summary>Answer</summary>(1) A backend is down or crashing, and the load balancer can't connect to it (or
gets a reset). Look for a pattern by backend in the LB logs. (2) The backends close idle keep-alive connections
sooner than the load balancer expects, so the LB reuses a connection the backend just closed and gets a reset. The
fix is to make the backend's idle timeout <i>longer</i> than the LB's. This second one is a classic, and it's
invisible in the app's own logs.</details>

---

## Build it
Each step is a program you finish in `exercises/`. The tests in `check.py` each target one classic mistake:
```bash
python3 check.py 1                # your exercises/step1_client.py
python3 check.py 2 --solution     # what passing looks like
```

### Step 1: an HTTP/1.1 client with keep-alive
`fetch_all(host, port, paths)` sends every request over **one** connection and returns parsed responses. It's
really an exercise in framing: a `Reader` that hands out exact byte counts and lines from arbitrary TCP pieces, then
the four body rules.

**What the tests catch:** reading a header or body with one `recv()` (the test server sends 7 bytes at a time).
Chunk sizes with extensions and trailers. Waiting for a body on 204/304 (the 304 even carries a misleading
`Content-Length`). Opening a new connection per request. Leaving out `Host`.

### Step 2: a forward proxy
Absolute-form requests rewritten to origin-form, hop-by-hop headers stripped (including ones named in
`Connection`), `Via` added, request bodies forwarded, `CONNECT` tunnels, a blocklist that includes subdomains,
`502` when the server is unreachable, `400` for requests that aren't meant for a proxy, and one thread per client.

**What the tests catch:** forwarding `Proxy-Connection`. Sending the absolute URL on to the origin. A blocklist that
misses `www.` subdomains. A single-threaded proxy, where one slow download stalls everyone: the starter ships that
way on purpose.

### Step 3: a reverse proxy / load balancer
Longest-prefix routing on whole path segments, round robin, failover past backends that refuse connections,
`X-Forwarded-For/-Proto/-Host`, `502` when no backend is reachable, `504` when one is too slow, and `404` for unrouted paths.

**What the tests catch:** overwriting X-Forwarded-For instead of appending. `/apix` matching an `/api` route.
No failover. Waiting forever on a slow backend.

---

## Where this comes back
- **enterprise (1.7):** a real Squid forward proxy in front of every laptop, an egress firewall that makes it
  mandatory, URL policy, and its access log. Exercise 4 and help desk ticket 5.
- **mini-tcpdump (1.4):** extracts the SNI from TLS ClientHellos, the same field exercise 4 looked at.
- **PacketWhisperer (Phase 2):** its `tls_handshake_failure` diagnosis is exercise 4's wrong-name case, seen from a capture.

## Known limitations
- The proxies handle one request per client connection (they send `Connection: close`). Real ones keep connections
  alive on both sides and pool upstream connections.
- No HTTPS on the reverse proxy (no TLS termination), no chunked request bodies, no HTTP/2.
- The forward proxy has no authentication (407) and no caching. Squid in the enterprise lab shows both being configured.
- Exercise 4 needs internet access. Its output reflects example.com's certificate as of October 2026.
