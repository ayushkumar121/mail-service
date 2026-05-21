#!/usr/bin/env bash
set -euo pipefail

PORT=$(jq -r '.server.http.port // 443' /etc/mail-service.json 2>/dev/null || echo 443)
LOG_DIR=/var/log/mail-service
LOG_FILE="$LOG_DIR/metrics.log"

mkdir -p "$LOG_DIR"
TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)

{
  echo "# scrape $TS"
  if ! curl -fsSk --max-time 10 "https://127.0.0.1:${PORT}/metrics"; then
    echo "# scrape FAILED at $TS"
  fi
  echo
} >> "$LOG_FILE"
