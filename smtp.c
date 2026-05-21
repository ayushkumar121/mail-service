#define LOG_PREFIX "smtp: "
#include "smtp.h"
#include "maildir.h"
#include "metrics.h"

#include <resolv.h>
#include <arpa/nameser.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "config.h"

#define CRLF "\r\n"

#define SMTP_DEFAULT_PORT 25
#define SMTP_SOCKET_BACKLOG 1024

Error smtp_lookup_server(String domain, StringBuilder* host_smtp_server) {
    const char* domain_cstr = sv_to_tmp_c(domain);

    unsigned char answer[1024];
    const int n = res_query(domain_cstr, ns_c_in, ns_t_mx, answer, sizeof(answer));
    if (n <= 0) {
        return error(strerror(errno));
    }

    ns_msg msg;
    ns_initparse(answer, n, &msg);

    ns_rr rr;
    ns_parserr(&msg, ns_s_an, 0, &rr);

    char mx_host[256];
    dn_expand(ns_msg_base(msg), ns_msg_end(msg), ns_rr_rdata(rr) + 2, mx_host, sizeof(mx_host));

    sb_push_str(host_smtp_server, mx_host);

    return ErrorNil;
}

Error smtp_connect(BufIO* bio, String host, int port) {
    char* host_cstr = sv_to_tmp_c(host);
    char* port_cstr = tprintf("%d", port).data;

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res;
    if (getaddrinfo(host_cstr, port_cstr, &hints, &res) != 0) {
        return errorf("getaddrinfo failed for " SV_Fmt ": %s", SV_Arg(host), strerror(errno));
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return errorf("socket failed: %s", strerror(errno));
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        return errorf("connect failed to " SV_Fmt ": %d:%s", SV_Arg(host), port, strerror(errno));
    }

    DEBUG("Connected to: " SV_Fmt " %d", SV_Arg(host), port);

    *bio = bufio_new((void*)(intptr_t)fd, fd_raw_read, fd_raw_write);
    freeaddrinfo(res);
    return ErrorNil;
}

// Read a full multi-line SMTP reply. Final line is the one whose 4th byte is
// space (not dash). bio->read_buf holds the final line on return.
Error smtp_read_from_server(BufIO* bio) {
    for (;;) {
        Error err = bufio_read_until(bio, CRLF);
        if (has_error(err)) return err;
        String line = sb_to_sv(&bio->read_buf);
        DEBUG("recv: " SV_Fmt, SV_Arg(line));
        if (line.length >= 4 && line.data[3] == ' ') return ErrorNil;
        // dash-continuation; loop
    }
}

Error smtp_expect_response(BufIO* bio, int expected) {
    Error err = smtp_read_from_server(bio);
    if (has_error(err)) return err;

    const String response = sb_to_sv(&bio->read_buf);
    if (response.length < 3) return error("smtp response too short");

    char* endptr = NULL;
    const String code_sv = SV2(response.data, 3);
    int code = sv_to_int(code_sv, &endptr);

    if (code != expected) {
        return errorf(SV_Fmt, SV_Arg(bio->read_buf));
    }

    return ErrorNil;
}

Error smtp_deliver(String from, String to, String raw_msg) {
    String domain = sv_split_delim(to, '@').second;
    if (domain.length == 0) return errorf("deliver: no domain in '" SV_Fmt "'", SV_Arg(to));

    StringBuilder mx = {0};
    Error err = smtp_lookup_server(domain, &mx);
    if (has_error(err)) return err;
    String host = sb_to_sv(&mx);
    INFO("MX " SV_Fmt " -> " SV_Fmt, SV_Arg(domain), SV_Arg(host));

    BufIO bio = {0};
    err = smtp_connect(&bio, host, 25);
    if (has_error(err)) { sb_free(&mx); return err; }
    err = smtp_expect_response(&bio, 220); if (has_error(err)) return err;

    err = bufio_send_line(&bio, tprintf("EHLO " SV_Fmt, SV_Arg(get_hostname())));
    if (has_error(err)) return err;
    err = smtp_expect_response(&bio, 250); if (has_error(err)) return err;

    err = bufio_send_line(&bio, tprintf("MAIL FROM:<" SV_Fmt ">", SV_Arg(from)));
    if (has_error(err)) return err;
    err = smtp_expect_response(&bio, 250); if (has_error(err)) return err;

    err = bufio_send_line(&bio, tprintf("RCPT TO:<" SV_Fmt ">", SV_Arg(to)));
    if (has_error(err)) return err;
    err = smtp_expect_response(&bio, 250); if (has_error(err)) return err;

    err = bufio_send_line(&bio, SV("DATA"));
    if (has_error(err)) return err;
    err = smtp_expect_response(&bio, 354); if (has_error(err)) return err;

    err = bufio_send(&bio, raw_msg);
    if (has_error(err)) return err;
    // Ensure body ended with CRLF before the terminating "." line.
    if (raw_msg.length < 2 ||
        raw_msg.data[raw_msg.length - 2] != '\r' ||
        raw_msg.data[raw_msg.length - 1] != '\n') {
        err = bufio_send(&bio, SV("\r\n"));
        if (has_error(err)) return err;
    }
    err = bufio_send_line(&bio, SV("."));
    if (has_error(err)) return err;
    err = smtp_expect_response(&bio, 250); if (has_error(err)) return err;

    bufio_send_line(&bio, SV("QUIT"));
    close((int)(intptr_t)bio.file);
    bufio_free(&bio);
    sb_free(&mx);
    return ErrorNil;
}

typedef enum {
    SMTP_STATE_CONNECTED,
    SMTP_STATE_GREETED,
    SMTP_STATE_MAIL_FROM,
    SMTP_STATE_RCPT_TO,
    SMTP_STATE_DATA,
    SMTP_STATE_QUIT,
} SmtpSessionState;

typedef struct {
    int client_fd;
} SmtpClientArgs;

static void* handle_client(void* p) {
    SmtpClientArgs* args = (SmtpClientArgs*)p;
    const int client_fd = args->client_fd;
    free(args);

    // Start plaintext on connect. STARTTLS may upgrade us mid-session.
    SSL* ssl = NULL;
    BufIO bio = bufio_new((void*)(intptr_t)client_fd, fd_raw_read, fd_raw_write);

    bool tls_active    = false;
    bool authenticated = false;

    atomic_fetch_add(&smtp_connections_total, 1);
    atomic_fetch_add(&smtp_connections_active, 1);

    SmtpSessionState state = SMTP_STATE_CONNECTED;
    String mail_from = StringNil;
    String rcpt_to = StringNil;
    String ehlo_host = StringNil;

    // Greeting
    bufio_send_line(&bio, tprintf("220 " SV_Fmt " ESMTP ready", SV_Arg(get_hostname())));

    INFO("client connected");

    while (state != SMTP_STATE_QUIT) {
        Error err = bufio_read_until(&bio, CRLF);
        if (has_error(err)) {
            ERROR("recv failed: " SV_Fmt, SV_Arg(err.message));
            break;
        }

        const String line = sv_trim(sb_to_sv(&bio.read_buf));
        StringPair cmd_rest = sv_split_delim(line, ' ');
        const String cmd  = sv_trim(cmd_rest.first);
        const String rest = sv_trim(cmd_rest.second);

        if (sv_equal_ignore_case(cmd, SV("EHLO"))) {
            state = SMTP_STATE_GREETED;
            safe_free(ehlo_host.data);
            ehlo_host = sv_clone(rest);
            bufio_write_line(&bio, tprintf("250-" SV_Fmt " greets " SV_Fmt, SV_Arg(get_hostname()), SV_Arg(rest)));
            bufio_write_line(&bio, SV("250-SIZE 26214400"));
            bufio_write_line(&bio, SV("250-8BITMIME"));
            if (!tls_active) {
                bufio_write_line(&bio, SV("250 STARTTLS"));
            } else {
                // After TLS, advertise AUTH so clients (Thunderbird) can authenticate.
                bufio_write_line(&bio, SV("250 AUTH PLAIN"));
            }
            bufio_flush(&bio);

        } else if (sv_equal_ignore_case(cmd, SV("HELO"))) {
            state = SMTP_STATE_GREETED;
            safe_free(ehlo_host.data);
            ehlo_host = sv_clone(rest);
            bufio_send_line(&bio, tprintf("250 " SV_Fmt " greets " SV_Fmt, SV_Arg(get_hostname()), SV_Arg(rest)));

        } else if (sv_equal_ignore_case(cmd, SV("STARTTLS"))) {
            if (tls_active) {
                bufio_send_line(&bio, SV("503 5.5.1 TLS already active"));
                treset();
                continue;
            }
            // Send 220 in the clear and flush BEFORE upgrading.
            bufio_send_line(&bio, SV("220 2.0.0 Ready to start TLS"));

            // Drop any pipelined plaintext bytes (RFC 3207 — SMTP-injection guard).
            bio.overflow.length = 0;
            bio.read_buf.length = 0;

            ssl = SSL_new(tls_server_ctx());
            SSL_set_fd(ssl, client_fd);
            if (SSL_accept(ssl) <= 0) {
                ERROR("STARTTLS handshake failed");
                ERR_print_errors_fp(stderr);
                SSL_free(ssl);
                ssl = NULL;
                break;
            }
            // Swap the BufIO transport to TLS in place.
            bio.file     = ssl;
            bio.raw_read = ssl_raw_read;
            bio.raw_write = ssl_raw_write;
            tls_active = true;

            // RFC 3207: discard all SMTP state, client must re-EHLO.
            state = SMTP_STATE_CONNECTED;
            safe_free(mail_from.data); mail_from = StringNil;
            safe_free(rcpt_to.data);   rcpt_to   = StringNil;
            safe_free(ehlo_host.data); ehlo_host = StringNil;
            authenticated = false;

        } else if (sv_equal_ignore_case(cmd, SV("AUTH"))) {
            if (!tls_active) {
                bufio_send_line(&bio, SV("530 5.7.0 Must issue STARTTLS first"));
                treset();
                continue;
            }
            StringPair mech_arg = sv_split_delim(rest, ' ');
            String mech = sv_trim(mech_arg.first);
            String b64  = sv_trim(mech_arg.second);
            if (!sv_equal_ignore_case(mech, SV("PLAIN"))) {
                bufio_send_line(&bio, SV("504 5.5.4 Unrecognized authentication type"));
                treset();
                continue;
            }
            if (b64.length == 0) {
                bufio_send_line(&bio, SV("334 "));
                Error rerr = bufio_read_until(&bio, CRLF);
                if (has_error(rerr)) break;
                b64 = sv_trim(sb_to_sv(&bio.read_buf));
            }
            String decoded = base64_decode(b64);
            // RFC 4616: authzid \0 authcid \0 passwd  (authzid often empty)
            String authcid = StringNil, passwd = StringNil;
            size_t i = 0;
            while (i < decoded.length && decoded.data[i] != '\0') i++;  // skip authzid
            if (i < decoded.length) i++;                                 // skip NUL
            size_t auth_start = i;
            while (i < decoded.length && decoded.data[i] != '\0') i++;
            authcid = SV2(decoded.data + auth_start, i - auth_start);
            if (i < decoded.length) i++;
            passwd  = SV2(decoded.data + i, decoded.length - i);

            String want_user = get_auth_username();
            String want_pass = get_auth_password();
            if (want_user.length > 0 && want_pass.length > 0
                && sv_equal(authcid, want_user) && sv_equal(passwd, want_pass)) {
                authenticated = true;
                INFO("AUTH success: " SV_Fmt, SV_Arg(authcid));
                bufio_send_line(&bio, SV("235 2.7.0 Authentication successful"));
            } else {
                WARN("AUTH failed for user '" SV_Fmt "'", SV_Arg(authcid));
                bufio_send_line(&bio, SV("535 5.7.8 Authentication credentials invalid"));
            }

        } else if (sv_equal_ignore_case(cmd, SV("MAIL"))) {
            const StringPair pair = sv_split_delim(rest, '<');
            mail_from = sv_clone(sv_split_delim(pair.second, '>').first);
            state = SMTP_STATE_MAIL_FROM;
            bufio_send_line(&bio, SV("250 OK"));

        } else if (sv_equal_ignore_case(cmd, SV("RCPT"))) {
            const StringPair pair = sv_split_delim(rest, '<');
            String addr = sv_split_delim(pair.second, '>').first;
            String domain = sv_split_delim(addr, '@').second;
            bool is_local = sv_equal_ignore_case(domain, get_local_domain())
                         || sv_equal_ignore_case(domain, SV("localhost"));
            if (!is_local && !authenticated) {
                WARN("relay denied: " SV_Fmt " (authed=%d, tls=%d)",
                     SV_Arg(addr), authenticated, tls_active);
                bufio_send_line(&bio, SV("554 5.7.1 Relay access denied"));
                treset();
                continue;
            }
            safe_free(rcpt_to.data);
            rcpt_to = sv_clone(addr);
            state = SMTP_STATE_RCPT_TO;
            // Don't pre-create maildir here — only do it if delivery is local.
            bufio_send_line(&bio, SV("250 OK"));

        } else if (sv_equal_ignore_case(cmd, SV("DATA"))) {
            state = SMTP_STATE_DATA;
            bufio_send_line(&bio, SV("354 Start input, end with <CRLF>.<CRLF>"));

            err = bufio_read_until(&bio, CRLF "." CRLF);
            if (has_error(err)) break;

            String body = sv_clone(sb_to_sv(&bio.read_buf));
            body.length -= strlen(CRLF "." CRLF);
            atomic_fetch_add(&mail_received_total, 1);

            String rcpt_domain = sv_split_delim(rcpt_to, '@').second;
            bool is_local = sv_equal_ignore_case(rcpt_domain, get_local_domain())
                         || sv_equal_ignore_case(rcpt_domain, SV("localhost"));

            if (is_local) {
                String sender_host = ehlo_host.length > 0 ? ehlo_host : SV("localhost");
                String inbox = tprintf(SV_Fmt "/" SV_Fmt "/INBOX",
                                       SV_Arg(get_maildir()), SV_Arg(rcpt_to));
                if (!file_exists(inbox.data)) make_directory(inbox.data);
                int uid = next_uid(inbox.data);
                String filename = tprintf(SV_Fmt "/%ld.%s." SV_Fmt ";U=%d.eml",
                    SV_Arg(inbox),
                    (long)time(NULL),
                    sv_to_tmp_c(random_id(RANDOM_ID_LEN)),
                    SV_Arg(sender_host),
                    uid);
                Error werr = write_entire_file(filename.data, body);
                if (has_error(werr)) {
                    atomic_fetch_add(&mail_failures_maildir_write, 1);
                    ERROR("maildir write failed for " SV_Fmt ": " SV_Fmt,
                          SV_Arg(rcpt_to), SV_Arg(werr.message));
                    bufio_send_line(&bio, SV("451 local delivery failed; try later"));
                } else {
                    atomic_fetch_add(&mail_delivered_local_total, 1);
                    INFO("saved " SV_Fmt " (%zu bytes) for " SV_Fmt,
                         SV_Arg(filename), body.length, SV_Arg(rcpt_to));
                    bufio_send_line(&bio, SV("250 OK queued"));
                }
            } else {
                INFO("delivering to " SV_Fmt " from " SV_Fmt, SV_Arg(rcpt_to), SV_Arg(mail_from));
                Error rerr = smtp_deliver(mail_from, rcpt_to, body);
                if (has_error(rerr)) {
                    atomic_fetch_add(&mail_failures_relay, 1);
                    ERROR("delivery failed: " SV_Fmt, SV_Arg(rerr.message));
                    bufio_send_line(&bio, SV("451 delivery failed; try later"));
                } else {
                    atomic_fetch_add(&mail_sent_total, 1);
                    bufio_send_line(&bio, SV("250 OK delivered"));
                }
            }
            safe_free(body.data);

        } else if (sv_equal_ignore_case(cmd, SV("QUIT"))) {
            state = SMTP_STATE_QUIT;
            bufio_send_line(&bio, SV("221 Bye"));

        } else {
            bufio_send_line(&bio, SV("502 Command not implemented"));
        }

        treset();
    }

    if (ssl != NULL) {
        tls_session_close(ssl);
    } else {
        close(client_fd);
    }

    safe_free(mail_from.data);
    safe_free(rcpt_to.data);
    safe_free(ehlo_host.data);
    atomic_fetch_sub(&smtp_connections_active, 1);
    bufio_free(&bio);

    return NULL;
}

Error smtp_server_init(SmtpServer* server) {
    assert(server != NULL);

    server->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->sock_fd < 0) {
        return errorf("socket failed: %s", strerror(errno));
    }

    int socket_options = 1;
#ifdef SO_REUSEADDR
    if (setsockopt(server->sock_fd, SOL_SOCKET, SO_REUSEADDR, &socket_options,
                   sizeof(socket_options)) < 0) {
        return errorf("setsockopt failed: %s", strerror(errno));
    }
#endif
#ifdef SO_REUSEPORT
    if (setsockopt(server->sock_fd, SOL_SOCKET, SO_REUSEPORT, &socket_options,
                   sizeof(socket_options)) < 0) {
        return errorf("setsockopt failed: %s", strerror(errno));
    }
#endif

    int port = config_get_int(SV("server.smtp.port"), SMTP_DEFAULT_PORT);

    server->addr.sin_family = AF_INET;
    server->addr.sin_addr.s_addr = INADDR_ANY;
    server->addr.sin_port = htons(port);

    if (bind(server->sock_fd, (struct sockaddr*)&server->addr, sizeof(server->addr)) < 0) {
        return errorf("bind failed: %s", strerror(errno));
    }

    return ErrorNil;
}

Error smtp_server_listen(const SmtpServer* server) {
    assert(server != NULL);
    assert(server->sock_fd > 0);

    // Warm up the TLS context now so config errors surface at startup, not on
    // first STARTTLS upgrade.
    (void)tls_server_ctx();

    const int listen_port = config_get_int(SV("server.smtp.port"), SMTP_DEFAULT_PORT);

    if (listen(server->sock_fd, SMTP_SOCKET_BACKLOG) < 0) {
        return errorf("listen failed: %s\n", strerror(errno));
    }

    INFO("server started on port %d (plaintext + STARTTLS)", listen_port);
    while (true) {
        const int client_fd = accept(server->sock_fd, NULL, NULL);
        if (client_fd < 0) {
            ERROR("accept failed: %s\n", strerror(errno));
            continue;
        }

        SmtpClientArgs* args = malloc(sizeof(SmtpClientArgs));
        args->client_fd = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, args) != 0) {
            free(args);
            close(client_fd);
            ERROR("pthread_create failed: %s\n", strerror(errno));
            continue;
        }

        pthread_detach(tid);
    }

    assert(false && "unreachable");
}