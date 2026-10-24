# The resolver works, but the configured hostname doesn't exist (a typo'd
# config value). The server answers quickly -- with NXDOMAIN.
CASE_LABEL=dns_nxdomain
CASE_DESCRIPTION="app is configured with a hostname that doesn't exist; DNS answers NXDOMAIN"
CAPTURE_NS=$LAN_A_NS
CAPTURE_IF=a-eth0

case_trigger() { in_ns "$LAN_A_NS" curl -sS -o /dev/null --max-time 8 "https://api-$RANDOM.netlab-typo.invalid"; }
