#!/usr/bin/env bash
set -euo pipefail

cp mail-service.service /etc/systemd/system/mail-service.service
cp config.json          /etc/mail-service.json
mkdir -p /var/mail-service/maildir

systemctl daemon-reload
systemctl enable --now mail-service
systemctl status --no-pager -n 5 mail-service
