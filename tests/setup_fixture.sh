#!/bin/bash
# Creates test maildir fixtures
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# IMAP test user — 2 pre-seeded messages
MAILDIR="$SCRIPT_DIR/../maildir/testuser/INBOX"
rm -rf "$MAILDIR"
mkdir -p "$MAILDIR"
printf "From: sender@example.com\r\nTo: testuser\r\nSubject: Test Message 1\r\n\r\nThis is body 1.\r\n" \
    > "$MAILDIR/1.eml"
printf "From: sender@example.com\r\nTo: testuser\r\nSubject: Test Message 2\r\n\r\nThis is body 2.\r\n" \
    > "$MAILDIR/2.eml"
echo "Fixture ready: $MAILDIR"
ls "$MAILDIR"

# SMTP test user — empty inbox (SMTP will create it and deliver into it)
SMTP_MAILDIR="$SCRIPT_DIR/../maildir/smtpuser@localhost/INBOX"
rm -rf "$SCRIPT_DIR/../maildir/smtpuser@localhost"
echo "Cleared SMTP maildir for smtpuser@localhost"
