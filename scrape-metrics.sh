#!/usr/bin/env bash
set -euo pipefail

PORT=$(jq -r '.server.http.port // 8080' /etc/mail-service.json 2>/dev/null || echo 8080)
LOG_DIR=/var/log/mail-service
LOG_FILE="$LOG_DIR/metrics.log"

mkdir -p "$LOG_DIR"
TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)

{
  echo "# scrape $TS"
  if ! curl -fsS --max-time 10 "http://127.0.0.1:${PORT}/metrics"; then
    echo "# scrape FAILED at $TS"
  fi
  echo
} >> "$LOG_FILE"
