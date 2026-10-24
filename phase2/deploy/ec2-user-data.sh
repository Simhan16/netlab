#!/bin/bash
# EC2 user data (Ubuntu 24.04): installs Docker and starts PacketWhisperer.
# Paste into "Advanced details → User data" when launching the instance, after
# filling in REPO_URL. Then SSH in, edit /opt/netlab/phase2/deploy/.env, and
# run `docker compose up -d --build` there (the service won't start without it).
set -euxo pipefail

REPO_URL="https://github.com/YOUR_USER/netlab.git"   # <-- your repo

apt-get update
apt-get install -y ca-certificates curl git
install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] \
https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$VERSION_CODENAME") stable" \
  >/etc/apt/sources.list.d/docker.list
apt-get update
apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
usermod -aG docker ubuntu

git clone "$REPO_URL" /opt/netlab
cp /opt/netlab/phase2/deploy/.env.example /opt/netlab/phase2/deploy/.env
chown -R ubuntu:ubuntu /opt/netlab
