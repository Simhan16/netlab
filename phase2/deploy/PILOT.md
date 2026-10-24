# Pilot playbook: deploying for a real user

Phase 2.2. Getting a real team to depend on this is the FDE signal; the code is just
what makes it possible. Candidates: campus IT's help desk, the residence-hall network
team, a student org that runs servers or events with Wi-Fi.

## 1. Scoping conversation (before writing more code)
Ask, and write the answers down here:
1. What network complaints do you get most? Roughly how many per week? *(baseline volume)*
2. Walk me through the last one. Who noticed, who looked at it, what did they check, how long until it was resolved? *(baseline triage time)*
3. Who would upload a capture? Are they comfortable running Wireshark/tcpdump, or do they need a one-command capture script?
4. Where do you want answers: a web page, Slack, the ticketing system?
5. What data are you allowed to send off-campus? Are IPs and hostnames OK? *(privacy constraints; may require the rules-only engine)*
6. What would make this not worth using? *(failure criteria)*

Notes:
```
[date] [who]  …
```

## 2. Define success up front
| Metric | Baseline (from scoping) | Target | How it's measured |
|---|---|---|---|
| Median time from capture to diagnosis | [X] min | [Y] min | `latency_ms` in usage log vs. their current process |
| Diagnoses users marked correct | n/a | [X]% | `/v1/feedback` entries |
| Tickets resolved without escalation | [X]% | [Y]% | their ticket system, before vs. during the pilot |
| Weekly active users | 0 | [N] | distinct `caller` in usage log |

Never fill a number in without a source. Leave `[X]` until it's measured.

## 3. Rollout
1. **Week 1:** one champion user, one API key. Sit with them for the first three real captures and watch where they get confused.
2. **Weeks 2-3:** fix what they hit (expect: capture instructions, wording of summaries, unsupported pcapng variants). Ship small changes fast and tell them what changed.
3. **Week 4+:** widen to the rest of the team, turn on Slack alerts, and review the usage report weekly.

Give users a one-liner for capturing:
```bash
sudo tcpdump -i any -w problem.pcap -c 20000   # reproduce the issue, then Ctrl-C
```

## 4. Feedback loop
- Every response has a `request_id`. Ask users to send feedback when it's wrong:
  ```bash
  curl -X POST https://$PW_DOMAIN/v1/feedback -H "X-API-Key: $KEY" -H 'content-type: application/json' \
       -d '{"request_id": "...", "correct": false, "actual_label": "dns_unreachable", "comment": "it was the DHCP server"}'
  ```
- With permission, turn wrong diagnoses into new casebook entries. That's how the eval set grows past lab conditions.

## 5. Weekly log
| Week | Uploads | Users | Marked correct | Changes shipped | Quote / observation |
|---|---|---|---|---|---|
| 1 | | | | | |

`packetwhisperer usage --log usage.jsonl` produces the first three columns.
