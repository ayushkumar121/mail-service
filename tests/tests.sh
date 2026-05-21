#!/bin/bash
# Smoke tests for the live mail-service.
# Run from the project root via `make test`.
#
# Ports / creds come from tests/config.json:
#   SMTP   port 2525  (plaintext + STARTTLS)
#   IMAP   port 9993  (implicit TLS)
#   HTTPS  port 8443
#   user/pass: testuser@ayush-kumar.com / testpass

HOST="localhost"
SMTP_PORT=2525
IMAP_PORT=9993
HTTP_PORT=8443
USER="testuser@ayush-kumar.com"
PASS="testpass"

PASS_COUNT=0
FAIL_COUNT=0

for i in $(seq 1 50); do
    if (echo > /dev/tcp/$HOST/$SMTP_PORT) 2>/dev/null; then break; fi
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

run_test_not() {
    local name="$1"
    local forbidden="$2"
    local result="$3"
    if echo "$result" | grep -q "$forbidden"; then
        echo "FAIL  $name"
        echo "      should NOT contain: $forbidden"
        echo "      got:"
        echo "$result" | sed 's/^/        /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    else
        echo "PASS  $name"
        PASS_COUNT=$((PASS_COUNT + 1))
    fi
}

B64_AUTH=$(printf '\0%s\0%s' "$USER" "$PASS" | base64 | tr -d '\n')

# ---------------------------------------------------------------------------
# SMTP plaintext (no STARTTLS) — inbound MX role
# ---------------------------------------------------------------------------

T=$(printf 'EHLO test\r\nQUIT\r\n' | nc -w 2 $HOST $SMTP_PORT)
run_test     "smtp/1  greeting"                 "220 .* ESMTP ready"  "$T"
run_test     "smtp/2  EHLO advertises STARTTLS" "250 STARTTLS"        "$T"
run_test_not "smtp/3  no AUTH before TLS"       "AUTH PLAIN"          "$T"

T=$(printf 'EHLO test\r\nMAIL FROM:<x@x>\r\nRCPT TO:<spammer@gmail.com>\r\nQUIT\r\n' \
    | nc -w 2 $HOST $SMTP_PORT)
run_test "smtp/4  relay denied without auth"    "554 5.7.1 Relay access denied" "$T"

T=$(printf 'EHLO test\r\nMAIL FROM:<sender@example.com>\r\nRCPT TO:<%s>\r\nQUIT\r\n' "$USER" \
    | nc -w 2 $HOST $SMTP_PORT)
run_test     "smtp/5  local recipient accepted unauth" "250 OK" "$T"
run_test_not "smtp/6  local recipient not refused"     "554"    "$T"

T=$(printf 'EHLO test\r\nAUTH PLAIN %s\r\nQUIT\r\n' "$B64_AUTH" \
    | nc -w 2 $HOST $SMTP_PORT)
run_test "smtp/7  AUTH before STARTTLS refused" "530 5.7.0" "$T"

# ---------------------------------------------------------------------------
# SMTP STARTTLS + AUTH + relay (Thunderbird path)
# ---------------------------------------------------------------------------

T=$(
  { printf 'EHLO t\r\n'; sleep 0.3;
    printf 'AUTH PLAIN %s\r\nMAIL FROM:<%s>\r\nRCPT TO:<friend@example.com>\r\nQUIT\r\n' \
           "$B64_AUTH" "$USER";
    sleep 0.5;
  } | openssl s_client -connect $HOST:$SMTP_PORT -starttls smtp -quiet 2>/dev/null
)
run_test "smtp/8  EHLO after STARTTLS advertises AUTH" "250 AUTH PLAIN"                  "$T"
run_test "smtp/9  AUTH PLAIN succeeds"                 "235 .* Authentication successful" "$T"
run_test "smtp/10 relay allowed when authenticated"    "250 OK"                          "$T"

# DATA body roundtrip — exercises bufio_read_until_into.
SUBJECT="bufio-smoke-$$"
T=$(
  { printf 'EHLO t\r\n'; sleep 0.3;
    printf 'AUTH PLAIN %s\r\nMAIL FROM:<sender@example.com>\r\nRCPT TO:<%s>\r\nDATA\r\nSubject: %s\r\n\r\nHello.\r\n.\r\nQUIT\r\n' \
           "$B64_AUTH" "$USER" "$SUBJECT";
    sleep 0.5;
  } | openssl s_client -connect $HOST:$SMTP_PORT -starttls smtp -quiet 2>/dev/null
)
run_test "smtp/11 DATA delivered to local maildir"     "250 OK queued"               "$T"

sleep 0.2
MAILDIR_FILE=$(grep -lr "Subject: $SUBJECT" "maildir/$USER/INBOX/" 2>/dev/null | head -1)
if [ -n "$MAILDIR_FILE" ]; then
    echo "PASS  smtp/12 maildir file written ($MAILDIR_FILE)"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "FAIL  smtp/12 maildir file written"
    echo "      no .eml containing 'Subject: $SUBJECT' under maildir/$USER/INBOX/"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# ---------------------------------------------------------------------------
# HTTPS /metrics
# ---------------------------------------------------------------------------

T=$(curl -sk --max-time 5 https://$HOST:$HTTP_PORT/metrics)
run_test "http/1  /metrics responds over HTTPS" "mail_received_total" "$T"

# ---------------------------------------------------------------------------
# IMAPS
# ---------------------------------------------------------------------------

T=$(printf 'a LOGOUT\r\n' | openssl s_client -connect $HOST:$IMAP_PORT -quiet 2>/dev/null)
run_test "imap/1  greeting over TLS"   "\\* OK \\[CAPABILITY"    "$T"
run_test "imap/2  LOGOUT completes"    "a OK LOGOUT completed"   "$T"

T=$(
  { printf 'a LOGIN %s %s\r\nb SELECT INBOX\r\nc LOGOUT\r\n' "$USER" "$PASS"; sleep 0.3; } \
    | openssl s_client -connect $HOST:$IMAP_PORT -quiet 2>/dev/null
)
run_test "imap/3  LOGIN succeeds"        "a OK LOGIN completed"           "$T"
run_test "imap/4  SELECT INBOX succeeds" "b OK \\[READ-WRITE\\] SELECT"   "$T"

echo
echo "================================================================"
echo "$PASS_COUNT passed, $FAIL_COUNT failed"
echo "================================================================"
exit $((FAIL_COUNT > 0 ? 1 : 0))
