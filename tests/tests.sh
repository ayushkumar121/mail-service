#!/bin/bash
# SMTP + IMAP test suite — uses curl and nc
# Run from the project root: make test
# Requires: server on localhost:143, fixture seeded by setup_fixture.sh

HOST="localhost"
PORT=143
USER="testuser"
PASS="anypassword"
IMAP_URL="imap://$HOST:$PORT"

PASS_COUNT=0
FAIL_COUNT=0

# Wait until the IMAP port accepts connections (avoids flakes when the
# server is still binding when the first test fires).
for i in $(seq 1 50); do
    if (echo > /dev/tcp/$HOST/$PORT) 2>/dev/null; then break; fi
    sleep 0.1
done

run_test() {
    local name="$1"
    local expected="$2"
    local result="$3"
    if echo "$result" | grep -q "$expected"; then
        echo "PASS  $name"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "FAIL  $name"
        echo "      expected pattern: $expected"
        echo "      got:"
        echo "$result" | sed 's/^/        /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

nc_send() {
    # %b interprets \r\n as actual CR+LF; server closes conn on LOGOUT so nc exits naturally
    printf '%b' "$1" | nc "$HOST" "$PORT" 2>/dev/null
}

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

# | 1 | Greeting contains CAPABILITY
T=$(curl -sv --url "$IMAP_URL" 2>&1 | head -20)
run_test "1  greeting: CAPABILITY in banner"  "CAPABILITY"      "$T"

# | 2 | CAPABILITY command
T=$(nc_send $'a1 CAPABILITY\r\na2 LOGOUT\r\n')
run_test "2  CAPABILITY command"              "OK CAPABILITY"   "$T"

# | 3 | LOGIN accepted
T=$(curl -sv --user "$USER:$PASS" --url "$IMAP_URL/" 2>&1)
run_test "3  LOGIN accepted"                  "OK LOGIN"        "$T"

# | 4 | LIST returns INBOX
run_test "4  LIST returns INBOX"              "INBOX"           "$T"

# | L1 | LIST all mailboxes (*)
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 LIST \"\" \"*\"\r\na3 LOGOUT\r\n")
run_test "L1  LIST *: INBOX present"             "INBOX"           "$T"
run_test "L1b LIST *: OK LIST"                   "OK LIST"         "$T"

# | L2 | LIST exact match
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 LIST \"\" \"INBOX\"\r\na3 LOGOUT\r\n")
run_test "L2  LIST exact INBOX"                  "INBOX"           "$T"

# | L3 | LIST no match
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 LIST \"\" \"Nonexistent\"\r\na3 LOGOUT\r\n")
run_test "L3  LIST no match: OK LIST"            "OK LIST"         "$T"
if echo "$T" | grep -q "^\* LIST"; then
    echo "FAIL  L3b LIST no match: unexpected * LIST line"
    FAIL_COUNT=$((FAIL_COUNT + 1))
else
    echo "PASS  L3b LIST no match: no * LIST line emitted"
    PASS_COUNT=$((PASS_COUNT + 1))
fi

# | L4 | LIST hierarchy probe (LIST "" "")
T=$(nc_send $'a1 LIST "" ""\r\na2 LOGOUT\r\n')
run_test "L4  LIST hierarchy probe: Noselect"    "Noselect"        "$T"

# | L5 | LIST wildcard %
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 LIST \"\" \"%\"\r\na3 LOGOUT\r\n")
run_test "L5  LIST %: INBOX present"             "INBOX"           "$T"

# | 5 | SELECT INBOX (curl sends LIST for folder URLs, so use nc for SELECT)
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 SELECT INBOX\r\na3 LOGOUT\r\n")
run_test "5  SELECT: OK SELECT"               "SELECT completed" "$T"
run_test "5b SELECT: EXISTS count"            "EXISTS"          "$T"

# | 6 | FETCH message 1
T=$(curl -sv --user "$USER:$PASS" --url "$IMAP_URL/INBOX/;UID=1" 2>&1)
run_test "6  FETCH message"                   "FETCH"           "$T"

# | 7 | STATUS
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 STATUS INBOX (MESSAGES UNSEEN RECENT)\r\na3 LOGOUT\r\n")
run_test "7  STATUS MESSAGES"                 "MESSAGES"        "$T"
run_test "7b STATUS UNSEEN"                   "UNSEEN"          "$T"

# | 8 | NOOP
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 SELECT INBOX\r\na3 NOOP\r\na4 LOGOUT\r\n")
run_test "8  NOOP"                            "OK NOOP"         "$T"

# | 9 | STORE \Seen
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 SELECT INBOX\r\na3 STORE 1 +FLAGS (\\\\Seen)\r\na4 LOGOUT\r\n")
run_test "9  STORE +FLAGS Seen: FETCH reply"  "FETCH"           "$T"
run_test "9b STORE +FLAGS Seen: flag present" "Seen"            "$T"

# Verify Maildir rename happened on disk
MAILDIR="$(dirname "$0")/../maildir/testuser/INBOX"
SEEN_FILE=$(ls "$MAILDIR" | grep "F=[^,.]*S" 2>/dev/null | head -1)
if [ -n "$SEEN_FILE" ]; then
    echo "PASS  9c STORE: Maildir filename has F=...S... suffix ($SEEN_FILE)"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  9c STORE: no F=...S... suffix found in $MAILDIR"
    ls "$MAILDIR"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# | 10 | STORE \Deleted (mark msg 2 for expunge)
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 SELECT INBOX\r\na3 STORE 2 +FLAGS (\\\\Deleted)\r\na4 LOGOUT\r\n")
run_test "10  STORE +FLAGS Deleted"           "Deleted"         "$T"

# | 11 | EXPUNGE
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 SELECT INBOX\r\na3 EXPUNGE\r\na4 LOGOUT\r\n")
run_test "11  EXPUNGE response"               "EXPUNGE"         "$T"
run_test "11b EXPUNGE OK"                     "OK EXPUNGE"      "$T"

# Verify file deleted from disk
DELETED_FILE=$(ls "$MAILDIR" | grep "F=[^,.]*T" 2>/dev/null | head -1)
if [ -z "$DELETED_FILE" ]; then
    echo "PASS  11c EXPUNGE: deleted file removed from Maildir"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  11c EXPUNGE: file with T flag still present: $DELETED_FILE"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# | F1 | CREATE folder
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 CREATE Archive\r\na3 LIST \"\" \"*\"\r\na4 LOGOUT\r\n")
run_test "F1  CREATE: OK CREATE"               "OK CREATE"       "$T"
run_test "F1b CREATE: appears in LIST"         "Archive"         "$T"
if [ -d "$(dirname "$0")/../maildir/$USER/Archive" ]; then
    echo "PASS  F1c CREATE: directory exists on disk"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  F1c CREATE: directory missing on disk"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# | F2 | CREATE duplicate
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 CREATE Archive\r\na3 LOGOUT\r\n")
run_test "F2  CREATE duplicate: NO"            "NO mailbox already exists"  "$T"

# | F3 | CREATE INBOX rejected
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 CREATE INBOX\r\na3 LOGOUT\r\n")
run_test "F3  CREATE INBOX: NO"                "NO"              "$T"

# | F4 | RENAME
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 RENAME Archive Saved\r\na3 LIST \"\" \"*\"\r\na4 LOGOUT\r\n")
run_test "F4  RENAME: OK RENAME"               "OK RENAME"       "$T"
run_test "F4b RENAME: new name in LIST"        "Saved"           "$T"
if [ ! -d "$(dirname "$0")/../maildir/$USER/Archive" ] && [ -d "$(dirname "$0")/../maildir/$USER/Saved" ]; then
    echo "PASS  F4c RENAME: disk reflects rename"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  F4c RENAME: disk state wrong"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# | F5 | DELETE
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 DELETE Saved\r\na3 LOGOUT\r\n")
run_test "F5  DELETE: OK DELETE"               "OK DELETE"       "$T"
if [ ! -d "$(dirname "$0")/../maildir/$USER/Saved" ]; then
    echo "PASS  F5b DELETE: directory removed from disk"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  F5b DELETE: directory still present"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# | F6 | DELETE INBOX rejected
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 DELETE INBOX\r\na3 LOGOUT\r\n")
run_test "F6  DELETE INBOX: NO"                "NO"              "$T"

# | F7 | DELETE nonexistent
T=$(nc_send $"a1 LOGIN $USER $PASS\r\na2 DELETE Nonexistent\r\na3 LOGOUT\r\n")
run_test "F7  DELETE missing: NO"              "NO mailbox does not exist"  "$T"

# | A1 | APPEND a message
APPEND_BODY=$'From: a@example.com\r\nTo: b@example.com\r\nSubject: Appended\r\n\r\nHello via APPEND'
APPEND_LEN=${#APPEND_BODY}
nc_send $"a1 LOGIN $USER $PASS\r\na2 CREATE Drafts\r\na3 LOGOUT\r\n" >/dev/null
T=$(printf '%b' "a1 LOGIN $USER $PASS\r\na2 APPEND Drafts (\\\\Seen) {$APPEND_LEN}\r\n${APPEND_BODY}\r\na3 LOGOUT\r\n" | nc "$HOST" "$PORT" 2>/dev/null)
run_test "A1  APPEND: + continuation"          "+ Ready"         "$T"
run_test "A1b APPEND: OK APPEND"               "OK APPEND"       "$T"
APPEND_FILE=$(ls "$(dirname "$0")/../maildir/$USER/Drafts" 2>/dev/null | grep "F=[^,.]*S" | head -1)
if [ -n "$APPEND_FILE" ]; then
    echo "PASS  A1c APPEND: file written with F=...S... ($APPEND_FILE)"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  A1c APPEND: no file with F=...S... found in Drafts"
    ls "$(dirname "$0")/../maildir/$USER/Drafts" 2>/dev/null
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# | A2 | APPEND to nonexistent mailbox auto-creates
rm -rf "$(dirname "$0")/../maildir/$USER/Auto"
T=$(printf '%b' "a1 LOGIN $USER $PASS\r\na2 APPEND Auto {5}\r\nhello\r\na3 LOGOUT\r\n" | nc "$HOST" "$PORT" 2>/dev/null)
run_test "A2  APPEND missing: auto-created"    "OK APPEND"       "$T"
if [ -d "$(dirname "$0")/../maildir/$USER/Auto" ]; then
    echo "PASS  A2b APPEND: auto-created folder on disk"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  A2b APPEND: folder not auto-created"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi
rm -rf "$(dirname "$0")/../maildir/$USER/Auto"

nc_send $"a1 LOGIN $USER $PASS\r\na2 DELETE Drafts\r\na3 LOGOUT\r\n" >/dev/null

# | 12 | LOGOUT BYE
T=$(nc_send $'a1 LOGOUT\r\n')
run_test "12  LOGOUT BYE"                     "BYE"             "$T"

# ---------------------------------------------------------------------------
# SMTP tests
# ---------------------------------------------------------------------------
SMTP_USER="smtpuser@localhost"
SMTP_FROM="sender@example.com"
SMTP_SUBJECT="SMTP test message"
SMTP_BODY="Hello from the SMTP test suite."

smtp_send() {
    local from="$1" rcpt="$2" subject="$3" body="$4"
    curl -s smtp://localhost:25 \
        --mail-from "$from" \
        --mail-rcpt "$rcpt" \
        --upload-file - <<EOF
From: $from
To: $rcpt
Subject: $subject

$body
EOF
}

# | S1 | Send a message via SMTP
smtp_send "$SMTP_FROM" "$SMTP_USER" "$SMTP_SUBJECT" "$SMTP_BODY" >/dev/null 2>&1
sleep 0.1  # give server time to write the file
SMTP_MAILDIR="$(dirname "$0")/../maildir/$SMTP_USER/INBOX"
EML_COUNT=$(find "$SMTP_MAILDIR" -name "*.eml" 2>/dev/null | wc -l | tr -d ' ')
if [ "$EML_COUNT" -gt 0 ]; then
    echo "PASS  S1  SMTP: message delivered to Maildir ($EML_COUNT .eml file(s))"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  S1  SMTP: no .eml found in $SMTP_MAILDIR"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# | S2 | IMAP SELECT sees the delivered message
T=$(nc_send $"a1 LOGIN $SMTP_USER x\r\na2 SELECT INBOX\r\na3 LOGOUT\r\n")
run_test "S2  SMTP→IMAP: EXISTS > 0"          "1 EXISTS"        "$T"
run_test "S2b SMTP→IMAP: RECENT > 0"          "1 RECENT"        "$T"

# | S3 | IMAP SEARCH finds the message
T=$(nc_send $"a1 LOGIN $SMTP_USER x\r\na2 SELECT INBOX\r\na3 SEARCH ALL\r\na4 LOGOUT\r\n")
run_test "S3  SMTP→IMAP: SEARCH ALL returns 1" "SEARCH 1"       "$T"

# | S4 | IMAP FETCH returns the correct subject
T=$(nc_send $"a1 LOGIN $SMTP_USER x\r\na2 SELECT INBOX\r\na3 FETCH 1 BODY[]\r\na4 LOGOUT\r\n")
run_test "S4  SMTP→IMAP: FETCH has subject"    "$SMTP_SUBJECT"  "$T"
run_test "S4b SMTP→IMAP: FETCH has body"       "$SMTP_BODY"     "$T"
run_test "S4c SMTP→IMAP: FETCH has From"       "$SMTP_FROM"     "$T"

# | S5 | Send a second message, check EXISTS updates
smtp_send "$SMTP_FROM" "$SMTP_USER" "Second message" "Body 2." >/dev/null
T=$(nc_send $"a1 LOGIN $SMTP_USER x\r\na2 SELECT INBOX\r\na3 LOGOUT\r\n")
run_test "S5  SMTP: second message → 2 EXISTS" "2 EXISTS"       "$T"

# | S6 | UID FETCH via curl — check From (present in all delivered messages)
T=$(curl -s -u "$SMTP_USER:anypass" "imap://localhost:143/INBOX/;UID=1" 2>&1)
run_test "S6  UID FETCH via curl"              "$SMTP_FROM"     "$T"

# ---------------------------------------------------------------------------
echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed"
[ "$FAIL_COUNT" -eq 0 ]
