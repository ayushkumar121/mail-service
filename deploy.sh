#!/usr/bin/env bash
set -euo pipefail
cd /opt/mail-service
git fetch --depth 1 origin main
git reset --hard origin/main
make
systemctl restart mail-service
systemctl status mail-service