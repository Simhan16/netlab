# Deploying PacketWhisperer

One small EC2 instance running Docker Compose: the API container plus Caddy for automatic
HTTPS. That's plenty for a pilot. Phase 2.3.

```
users ──HTTPS──► Caddy :443 ──► packetwhisperer :8000 ──► Claude API
                                     │
                                     ├─► /data/usage.jsonl (volume)
                                     └─► Slack webhook (problem diagnoses)
```

## 1. Launch the instance
- Ubuntu 24.04, `t3.small` (2 GB RAM is comfortable for 50 MB uploads). Use a separate instance from your netns lab box.
- Security group: inbound 22 (your IP only), 80 and 443 (anywhere, or just the campus range for a pilot).
- An Elastic IP, and a DNS A record (e.g. `pw.yourclub.org`) pointing at it. Caddy needs the record to get a certificate.
- User data: paste `ec2-user-data.sh` (set `REPO_URL` first). It installs Docker and clones the repo to `/opt/netlab`.

## 2. Configure and start
```bash
ssh ubuntu@<elastic-ip>
cd /opt/netlab/phase2/deploy
nano .env        # PW_DOMAIN, ANTHROPIC_API_KEY, PW_API_KEYS (one per pilot user), SLACK_WEBHOOK_URL
docker compose up -d --build
curl https://$PW_DOMAIN/healthz
```

## 3. Slack alerts
Slack → *Apps* → *Incoming Webhooks* → add to a channel (e.g. `#netops-alerts`), paste the URL
into `SLACK_WEBHOOK_URL`, then `docker compose up -d`. For Discord, use the channel webhook URL
with `/slack` appended.

## Operating it
| Task | Command |
|---|---|
| Logs | `docker compose logs -f packetwhisperer` |
| Usage report | `docker compose exec packetwhisperer packetwhisperer usage --log /data/usage.jsonl` |
| Back up usage data | `docker compose cp packetwhisperer:/data/usage.jsonl ./usage-$(date +%F).jsonl` |
| Update | `git pull && docker compose up -d --build` |
| Rotate a key | edit `PW_API_KEYS`, then `docker compose up -d` |

## Security notes
- Keep `PW_API_KEYS` set in production. With it empty, auth is disabled.
- Uploads are deleted right after analysis. The summary sent to Claude contains IP addresses, ports and DNS names. Put that in your pilot agreement.
- Upload size is capped twice: by Caddy (60 MB) and by the app (`PW_MAX_UPLOAD_MB`).
- The container runs as a non-root user and needs no capabilities. It only reads files; it never captures.
- Set an AWS budget alert and an Anthropic Console spend limit before handing out keys.

## Not done yet (good next steps)
- **IaC.** Express the instance, security group, EIP and DNS record in Terraform or CDK instead of console clicks.
- **Real auth.** Campus SSO (OIDC) instead of shared API keys, if the pilot grows.
- **Metrics.** Export request counts and latency to CloudWatch or Prometheus, and alert on error rate.
