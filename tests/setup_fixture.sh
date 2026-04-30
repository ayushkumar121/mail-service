#!/bin/bash
# Creates maildir/testuser/INBOX with 2 test messages (no flags = unseen)
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MAILDIR="$SCRIPT_DIR/../maildir/testuser/INBOX"

rm -rf "$MAILDIR"
mkdir -p "$MAILDIR"

printf "From: sender@example.com\r\nTo: testuser\r\nSubject: Test Message 1\r\n\r\nThis is body 1.\r\n" \
    > "$MAILDIR/1.eml"

printf "From: sender@example.com\r\nTo: testuser\r\nSubject: Test Message 2\r\n\r\nThis is body 2.\r\n" \
    > "$MAILDIR/2.eml"

echo "Fixture ready: $MAILDIR"
ls "$MAILDIR"
