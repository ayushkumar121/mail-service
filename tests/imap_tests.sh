#!/bin/bash
# IMAP test suite — uses curl and nc
# Run from the project root: make test
# Requires: server on localhost:143, fixture seeded by setup_fixture.sh

HOST="localhost"
PORT=143
USER="testuser"
PASS="anypassword"
IMAP_URL="imap://$HOST:$PORT"

PASS_COUNT=0
FAIL_COUNT=0

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
run_test "5  SELECT: OK SELECT"               "OK SELECT"       "$T"
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
SEEN_FILE=$(ls "$MAILDIR" | grep ":2,.*S" 2>/dev/null | head -1)
if [ -n "$SEEN_FILE" ]; then
    echo "PASS  9c STORE: Maildir filename has :2,S suffix ($SEEN_FILE)"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  9c STORE: no :2,S suffix found in $MAILDIR"
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
DELETED_FILE=$(ls "$MAILDIR" | grep ":2,.*T" 2>/dev/null | head -1)
if [ -z "$DELETED_FILE" ]; then
    echo "PASS  11c EXPUNGE: deleted file removed from Maildir"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  11c EXPUNGE: file with T flag still present: $DELETED_FILE"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# | 12 | LOGOUT BYE
T=$(nc_send $'a1 LOGOUT\r\n')
run_test "12  LOGOUT BYE"                     "BYE"             "$T"

# ---------------------------------------------------------------------------
echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed"
[ "$FAIL_COUNT" -eq 0 ]
