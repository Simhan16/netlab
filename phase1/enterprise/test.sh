#!/usr/bin/env bash
# Check Acme Corp end to end: everything the business needs works, and everything
# policy forbids is blocked. Run with the lab up (./lab.sh up). Needs internet access
# from the EC2 instance for the web checks.
set -uo pipefail

pass=0
fail=0
# check <ok|blocked> <description> <container> <command...>
check() {
  local expect=$1 desc=$2 box=$3 got
  shift 3
  if docker exec "$box" timeout 15 "$@" >/dev/null 2>&1; then got=ok; else got=blocked; fi
  if [[ $got == "$expect" ]]; then
    printf 'PASS  %-58s %s\n' "$desc" "$got"
    pass=$((pass + 1))
  else
    printf 'FAIL  %-58s expected %s, got %s\n' "$desc" "$expect" "$got"
    fail=$((fail + 1))
  fi
}
web() { echo curl -fsS -o /dev/null --max-time "${2:-8}" "$1"; }

printf 'waiting for OSPF (edge-fw needs 2 full neighbors, the branch needs a route to HQ) '
for ((i = 0; i < 45; i++)); do
  full=$(docker exec edge-fw vtysh -c 'show ip ospf neighbor' 2>/dev/null | grep -c Full || true)
  if (( full == 2 )) && docker exec branch-fw ip route show 10.10.20.0/24 2>/dev/null | grep -q wg-site; then
    echo "ok"
    break
  fi
  printf '.'
  sleep 2
done
(( i < 45 )) || echo " timed out: the routing checks below will fail"

echo
echo "HQ users"
check ok      "ws1 -> its gateway (core-rtr)"                    ws1 ping -c1 -W2 10.10.10.1
check ok      "ws1 resolves intranet.corp.acme (internal DNS)"   ws1 sh -c 'getent hosts intranet.corp.acme | grep -q 10.10.20.20'
check ok      "ws1 -> intranet web"                              ws1 $(web http://intranet.corp.acme)
check blocked "ws1 -> intranet admin :8080 (IT only)"            ws1 $(web http://intranet.corp.acme:8080 4)
check ok      "ws1 -> internet web, through the proxy"           ws1 $(web https://example.com 12)
check blocked "ws1 -> internet web directly (proxy bypass)"      ws1 curl --noproxy '*' -fsS -o /dev/null --max-time 5 https://example.com
check blocked "ws1 -> blocked site (proxy URL policy)"           ws1 $(web http://example.org 12)
check ok      "ws1 -> ping the internet (ICMP allowed)"          ws1 ping -c1 -W3 1.1.1.1
check ok      "ws1 -> public site by name (split DNS -> DMZ)"    ws1 $(web http://www.acme.example)

echo
echo "IT"
check ok      "jumpbox -> intranet admin :8080"                  jumpbox $(web http://intranet.corp.acme:8080)
check ok      "jumpbox -> internet web directly"                 jumpbox $(web https://example.com 12)

echo
echo "DMZ"
check ok      "internet -> public site via the port forward"     remote $(web http://203.0.113.10)
check blocked "www (DMZ) -> intranet (DMZ can't reach inside)"   www $(web http://10.10.20.20 4)

echo
echo "branch office (over the site-to-site VPN)"
check ok      "bws1 -> intranet"                                 bws1 $(web http://intranet.corp.acme)
check ok      "bws1 -> internet web, backhauled to HQ's proxy"   bws1 $(web https://example.com 12)

echo
echo "remote worker (remote-access VPN, split tunnel)"
check ok      "remote -> intranet through the VPN"               remote $(web http://10.10.20.20)
check blocked "remote -> intranet admin :8080"                   remote $(web http://10.10.20.20:8080 4)
check ok      "remote -> internet directly (not via HQ)"         remote $(web https://example.com 12)

echo
echo "$pass passed, $fail failed"
(( fail == 0 ))
