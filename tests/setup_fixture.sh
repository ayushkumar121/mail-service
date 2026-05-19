#!/bin/bash
# Creates test maildir fixtures
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Single configured account (matches tests/config.json auth block). The same
# mailbox is reused for the IMAP and SMTP sections; tests.sh clears INBOX
# between the two. Wipe the whole user dir so folders created by previous runs
# (Archive, Drafts, Trash, Auto, Saved) don't leak into the next run.
USER_DIR="$SCRIPT_DIR/../maildir/testuser@ayush-kumar.com"
MAILDIR="$USER_DIR/INBOX"
rm -rf "$USER_DIR"
mkdir -p "$MAILDIR"
printf "From: sender@example.com\r\nTo: testuser@ayush-kumar.com\r\nSubject: Test Message 1\r\n\r\nThis is body 1.\r\n" \
    > "$MAILDIR/1700000001.aaa.host;U=1.eml"
printf "From: sender@example.com\r\nTo: testuser@ayush-kumar.com\r\nSubject: Test Message 2\r\n\r\nThis is body 2.\r\n" \
    > "$MAILDIR/1700000002.bbb.host;U=2.eml"
echo "Fixture ready: $MAILDIR"
ls "$MAILDIR"
