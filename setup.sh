#!/usr/bin/env bash
set -euxo pipefail

git fetch --depth 1 origin main
git reset --hard origin/main

apt update
apt install -y build-essential git jq curl pkg-config libssl-dev

ufw allow 22/tcp
ufw allow 25/tcp      # SMTP
ufw allow 443/tcp     # HTTPS
ufw allow 993/tcp     # IMAPS
ufw --force enable

cp mail-service.service /etc/systemd/system/mail-service.service
cp config.json          /etc/mail-service.json
mkdir -p "$(jq -r '.server.maildir' config.json)"
mkdir -p "$(jq -r '.server.logdir' config.json)"

make

systemctl daemon-reload
systemctl enable --now mail-service
systemctl status --no-pager -n 5 mail-service
