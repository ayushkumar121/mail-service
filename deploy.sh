#!/usr/bin/env bash
set -euo pipefail
cd /opt/mail-service
git fetch --depth 1 origin main
git reset --hard origin/main
make

mkdir -p /var/log/mail-service
chmod +x /opt/mail-service/scrape-metrics.sh
cp mail-service-metrics.cron /etc/cron.d/mail-service-metrics
chmod 644 /etc/cron.d/mail-service-metrics
systemctl restart cron

systemctl restart mail-service
systemctl status mail-service