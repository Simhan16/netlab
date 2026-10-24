# 04-tls-handshake-failure

**Label:** `tls_handshake_failure`

## Root cause
TCP connects, but the TLS handshake is aborted: the certificate doesn't match the requested hostname (also: expired cert, unknown CA, protocol/cipher mismatch).

## Expected signature in the mini-tcpdump summary
- TCP flow to 8443 reaches `open`/`closed` quickly with very little data
- `tls.sni` = the name the client asked for (app.lab)
- A fatal alert from the client (e.g. bad_certificate / certificate_unknown) right after the ServerHello
- `app_data_records` = 0: no application data ever flowed

## Real-world fix
Serve a certificate whose SAN covers the hostname clients use, or fix the client's hostname/SNI.

## Observed
<!-- Fill in after `sudo ./run_case.sh 04-tls-handshake-failure`: paste the relevant lines from
`mini-tcpdump -r pcaps/04-tls-handshake-failure/run1.pcap -q` and note anything that differed from the expected signature. -->
