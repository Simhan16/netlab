"""Root-cause labels shared by the casebook, both diagnosers and the evals.

Adding a casebook case with a new label means adding it here too, otherwise
the LLM's output schema won't allow it and the eval can never score it.
"""

LABELS: dict[str, str] = {
    "healthy": "No network problem visible in the capture.",
    "dns_unreachable": "DNS queries get no response: resolver down, wrong resolver address, or port 53 blocked.",
    "dns_nxdomain": "DNS answers that the requested name does not exist (NXDOMAIN).",
    "mtu_blackhole": (
        "Path MTU black hole: full-size packets silently dropped on a smaller-MTU hop because the ICMP "
        "'fragmentation needed' messages are filtered; small packets and the handshake still work."
    ),
    "tls_handshake_failure": "TCP connects but the TLS handshake fails (certificate, hostname or protocol problem).",
    "asymmetric_routing": "Replies take a different path than requests and are dropped on the way back.",
    "firewall_drop": "Connection attempts silently dropped: no reply, no RST, no ICMP.",
    "connection_refused": "The host is reachable but nothing listens on the port: SYN answered with RST.",
    "packet_loss": "Random packet loss: many retransmissions, but transfers eventually complete.",
    "high_latency": "High round-trip time without significant loss.",
    "unknown": "The capture doesn't contain enough evidence to pick any of the above.",
}

NEXT_STEPS: dict[str, list[str]] = {
    "healthy": ["No network action needed; if users still see a problem, look at the application or server."],
    "dns_unreachable": [
        "Check which DNS server the client uses (resolv.conf / DHCP option 6) and that it is up.",
        "Verify UDP and TCP port 53 are allowed between the client and that server.",
    ],
    "dns_nxdomain": [
        "Check the hostname in the client's configuration for typos.",
        "Confirm the record exists in the zone (and in the right view, if split-horizon DNS is used).",
    ],
    "mtu_blackhole": [
        "Allow ICMP type 3 code 4 (and ICMPv6 packet-too-big) through firewalls on the path.",
        "Or clamp TCP MSS on the router to the path MTU (iptables TCPMSS --clamp-mss-to-pmtu).",
    ],
    "tls_handshake_failure": [
        "Check the server certificate covers the hostname clients use (SAN), is unexpired, and chains to a "
        "trusted CA.",
        "Compare client and server TLS versions and cipher suites.",
    ],
    "asymmetric_routing": [
        "Trace both directions of the path (traceroute from each side) and find where they diverge.",
        "Fix the stray route, or use loose reverse-path filtering where the asymmetry is intended.",
    ],
    "firewall_drop": [
        "Find the rule dropping the traffic: security groups, network ACLs, host firewalls.",
        "If the traffic should be blocked, consider REJECT so clients fail fast instead of timing out.",
    ],
    "connection_refused": [
        "Check the service is running and listening on that port and address (ss -ltnp).",
        "Check the client is using the right port.",
    ],
    "packet_loss": [
        "Locate the lossy hop (mtr, interface error/drop counters), especially Wi-Fi or saturated links.",
    ],
    "high_latency": [
        "Find where the delay is added (mtr); check for VPN/satellite hops or bufferbloat on congested links.",
        "Reduce round trips: connection reuse, HTTP/2, TLS 1.3, or serve from closer to users.",
    ],
    "unknown": ["Capture closer to the client, for longer, while reproducing the problem."],
}
