#!/usr/bin/env bash
set -euo pipefail

apt update
apt install -y build-essential git jq curl cron

ufw allow 22/tcp
ufw allow 25/tcp
ufw allow 143/tcp
ufw allow 8080/tcp
ufw --force enable

cp mail-service.service /etc/systemd/system/mail-service.service
cp config.json          /etc/mail-service.json
mkdir -p "$(jq -r '.server.maildir' config.json)"

make

mkdir -p /var/log/mail-service
chmod +x /opt/mail-service/scrape-metrics.sh
cp mail-service-metrics.cron /etc/cron.d/mail-service-metrics
chmod 644 /etc/cron.d/mail-service-metrics
systemctl enable --now cron

systemctl daemon-reload
systemctl enable --now mail-service
systemctl status --no-pager -n 5 mail-service
