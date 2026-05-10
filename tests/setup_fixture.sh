#!/bin/bash
# Creates test maildir fixtures
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# IMAP test user — wipe entire user dir so folders created by previous runs
# (Archive, Drafts, Trash, Auto, Saved) don't leak into the next run.
USER_DIR="$SCRIPT_DIR/../maildir/testuser"
MAILDIR="$USER_DIR/INBOX"
rm -rf "$USER_DIR"
mkdir -p "$MAILDIR"
printf "From: sender@example.com\r\nTo: testuser\r\nSubject: Test Message 1\r\n\r\nThis is body 1.\r\n" \
    > "$MAILDIR/1700000001.aaa.host;U=1.eml"
printf "From: sender@example.com\r\nTo: testuser\r\nSubject: Test Message 2\r\n\r\nThis is body 2.\r\n" \
    > "$MAILDIR/1700000002.bbb.host;U=2.eml"
echo "Fixture ready: $MAILDIR"
ls "$MAILDIR"

# SMTP test user — empty inbox (SMTP will create it and deliver into it)
SMTP_MAILDIR="$SCRIPT_DIR/../maildir/smtpuser@localhost/INBOX"
rm -rf "$SCRIPT_DIR/../maildir/smtpuser@localhost"
echo "Cleared SMTP maildir for smtpuser@localhost"
