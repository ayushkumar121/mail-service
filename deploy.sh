#!/bin/bash
set -e
cd /opt/mail-service
git fetch --depth 1 origin main
git reset --hard origin/main
make
systemctl restart mail-service
echo "deployed"