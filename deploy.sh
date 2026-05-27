#!/usr/bin/env bash
set -euxo pipefail
cd /opt/mail-service
git fetch --depth 1 origin main
git reset --hard origin/main
make

mkdir -p "$(jq -r '.server.logdir' config.json)"

systemctl restart mail-service
systemctl status mail-service