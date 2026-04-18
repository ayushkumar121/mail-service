#include "smtp.h"

#include <errno.h>
#include <resolv.h>
#include <arpa/nameser.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include <unistd.h>

#include "config.h"

#define CRLF "\r\n"

#define SMTP_DEFAULT_PORT 25
#define SMTP_SOCKET_BACKLOG 1024
#define SMTP_DEFAULT_DIR SV("maildir")

String get_local_host() {
    static String host;
    if (host.length == 0) {
        host = config_get_string(SV("server.host"), SV("localhost"));
    }
    return host;
}

String get_mail_dir() {
    static String maildir;
    if (maildir.length == 0) {
        maildir = config_get_string(SV("server.smtp.dir"), SMTP_DEFAULT_DIR);
    }
    const char* maildir_cstr = sv_to_tmp_c(maildir);
    if (!file_exists(maildir_cstr)) {
        make_directory(maildir_cstr);
    }
    return maildir;
}

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

typedef struct {
    int sock_fd;
    StringBuilder read_buf;
    StringBuilder write_buf;
} SmtpConnection;

Error smtp_connect(SmtpConnection* conn, String host, int port) {
    assert(conn != NULL);

    char* host_cstr = sv_to_tmp_c(host);
    char* port_cstr = tprintf("%d", port).data;

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res;
    if (getaddrinfo(host_cstr, port_cstr, &hints, &res) != 0) {
        return errorf("getaddrinfo failed for " SV_Fmt ": %s", SV_Arg(host), strerror(errno));
    }

    conn->sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (conn->sock_fd < 0) {
        freeaddrinfo(res);
        return errorf("socket failed: %s", strerror(errno));
    }

    if (connect(conn->sock_fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        return errorf("connect failed to " SV_Fmt ": %d:%s", SV_Arg(host), port,
                      strerror(errno));
    }

    DEBUG("Connected to: "SV_Fmt" %d", SV_Arg(host), port);

    freeaddrinfo(res);
    return ErrorNil;
}

Error smtp_read(SmtpConnection* conn) {
    conn->read_buf.length = 0;
    char buf[512];

    bool finished = false;
    size_t scan_offset = 0;

    while (!finished) {
        const ssize_t n = read(conn->sock_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return errorf("smtp read failed: %s", strerror(errno));
        }
        if (n == 0) return errorf("smtp connection closed");
        sb_push_sv(&conn->read_buf, SV2(buf, n));

        // Finding the message end
        const String sv = SV2(conn->read_buf.data + scan_offset, conn->read_buf.length - scan_offset);
        StringPair pair = sv_split_str(sv, CRLF);
        while (pair.first.length > 0) {
            const String line = pair.first;
            if (line.length >= 4 && line.data[3] == ' ') {
                finished = true;
                break;
            };
            pair = sv_split_str(pair.second, CRLF);
        }
        scan_offset += n;
    }

    DEBUG("smtp recv: " SV_Fmt, SV_Arg(conn->read_buf));
    return ErrorNil;
}

Error smtp_write(SmtpConnection* conn, const String data) {
    conn->write_buf.length = 0;
    sb_push_sv(&conn->write_buf, data);
    sb_push_str(&conn->write_buf, CRLF);

    DEBUG("smtp send: " SV_Fmt, SV_Arg(conn->write_buf));

    size_t total_written = 0;
    while (total_written < conn->write_buf.length) {
        const ssize_t n = write(conn->sock_fd, conn->write_buf.data + total_written,
                          conn->write_buf.length - total_written);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK) continue;
            return errorf("smtp write failed: %s", strerror(errno));
        }
        if (n == 0) return errorf("smtp connection closed");
        total_written += n;
    }

    return ErrorNil;
}

Error smtp_expect_response(SmtpConnection* conn, int expected) {
    Error err = smtp_read(conn);
    if (has_error(err)) return err;

    const String response = sb_to_sv(&conn->read_buf);
    if (response.length < 3) return error("smtp response too short");

    char *endptr = NULL;
    const String code_sv = SV2(response.data, 3);
    int code = sv_to_int(code_sv, &endptr);

    if (code != expected) {
        return errorf(SV_Fmt, SV_Arg(conn->read_buf));
    }

    return ErrorNil;
}

Error smtp_send(const Email email) {
    Error err;
    const String domain = sv_split_delim(email.to, '@').second;

    String smtp_server;
    int smtp_port = 25;

    if (sv_equal(domain, SV("localhost"))) {
        smtp_server = domain;
    } else {
        StringBuilder builder = {0};
        err = smtp_lookup_server(domain, &builder);
        if (has_error(err)) { return err; }
        smtp_server = sb_to_sv(&builder);
    }

    // TODO: TLS

    // SMTP server communication
    SmtpConnection conn = {};
    smtp_connect(&conn, smtp_server, smtp_port);

    err = smtp_read(&conn);
    if (has_error(err)) { return err; }

    err = smtp_write(&conn, tprintf("EHLO "SV_Fmt, SV_Arg(get_local_host())));
    if (has_error(err)) { return err; }
    err = smtp_expect_response(&conn, 250);
    if (has_error(err)) { return err; }

    // 1. MAIL FROM
    err = smtp_write(&conn, tprintf("MAIL FROM:<"SV_Fmt">", SV_Arg(email.from)));
    if (has_error(err)) return err;
    err = smtp_expect_response(&conn, 250);
    if (has_error(err)) { return err; }

    // 2. RCPT TO
    err = smtp_write(&conn, tprintf("RCPT TO:<"SV_Fmt">", SV_Arg(email.to)));
    if (has_error(err)) return err;
    err = smtp_expect_response(&conn, 250);
    if (has_error(err)) { return err; }

    // 3. DATA
    err = smtp_write(&conn, SV("DATA"));
    if (has_error(err)) return err;
    err = smtp_expect_response(&conn, 354);
    if (has_error(err)) { return err; }

    // 4. Send headers + body
    err = smtp_write(&conn, tprintf("From: " SV_Fmt, SV_Arg(email.from)));
    if (has_error(err)) return err;
    err = smtp_write(&conn, tprintf("To: " SV_Fmt, SV_Arg(email.to)));
    if (has_error(err)) return err;
    err = smtp_write(&conn, tprintf("Subject: " SV_Fmt, SV_Arg(email.subject)));
    if (has_error(err)) return err;
    err = smtp_write(&conn, SV("")); // blank line separates headers from body
    if (has_error(err)) return err;
    err = smtp_write(&conn, email.body);
    if (has_error(err)) return err;

    // 5. End with a single dot on its own line
    err = smtp_write(&conn, SV("."));
    if (has_error(err)) return err;
    err = smtp_expect_response(&conn, 250);
    if (has_error(err)) { return err; }

    // 6. Quit
    err = smtp_write(&conn, SV("QUIT"));
    if (has_error(err)) return err;

    DEBUG("Message sent");

    return ErrorNil;
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

    if (bind(server->sock_fd, (struct sockaddr *) &server->addr,
             sizeof(server->addr)) < 0) {
        return errorf("bind failed: %s", strerror(errno));
    }

    INFO("Setting up maildir: "SV_Fmt, SV_Arg(get_mail_dir()));

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

Error smtp_recv(SmtpConnection* conn, const char* terminator) {
    conn->read_buf.length = 0;
    char buf[512];

    while (true) {
        const ssize_t n = read(conn->sock_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return errorf("smtp read failed: %s", strerror(errno));
        }
        if (n == 0) return errorf("smtp connection closed");
        sb_push_sv(&conn->read_buf, SV2(buf, n));

        if (sv_find(sb_to_sv(&conn->read_buf), terminator) != -1) break;
    }

    DEBUG("smtp recv: " SV_Fmt, SV_Arg(conn->read_buf));
    return ErrorNil;
}

static void* handle_client(void* p) {
    const int client_fd = (int)(intptr_t) p;
    SmtpConnection conn = {client_fd};

    SmtpSessionState state = SMTP_STATE_CONNECTED;

    String mail_from = StringNil;
    String rcpt_to = StringNil;

    // Greeting
    smtp_write(&conn, tprintf("220 "SV_Fmt" ESMTP ready", SV_Arg(get_local_host())));

    while (state != SMTP_STATE_QUIT) {
        Error err = smtp_recv(&conn, CRLF);
        if (has_error(err)) {
            ERROR("smtp_read failed: %s\n", strerror(errno));
            break;
        }

        const String line = sv_trim(sb_to_sv(&conn.read_buf));
        StringPair cmd_rest = sv_split_delim(line, ' ');
        const String cmd  = sv_trim(cmd_rest.first);
        const String rest = sv_trim(cmd_rest.second);

        if (sv_equal_ignore_case(cmd, SV("EHLO")) || sv_equal_ignore_case(cmd, SV("HELO"))) {
            state = SMTP_STATE_GREETED;
            smtp_write(&conn, tprintf("250 " SV_Fmt " greets " SV_Fmt, SV_Arg(get_local_host()), SV_Arg(rest)));

        } else if (sv_equal_ignore_case(cmd, SV("MAIL"))) {
            // MAIL FROM:<sender@example.com>
            // extract email between < >
            const StringPair pair = sv_split_delim(rest, '<');
            mail_from = sv_clone(sv_split_delim(pair.second, '>').first);
            state = SMTP_STATE_MAIL_FROM;
            smtp_write(&conn, SV("250 OK"));

        } else if (sv_equal_ignore_case(cmd, SV("RCPT"))) {
            // RCPT TO:<recipient@example.com>
            const StringPair pair = sv_split_delim(rest, '<');
            rcpt_to = sv_clone(sv_split_delim(pair.second, '>').first);
            state = SMTP_STATE_RCPT_TO;

            // TODO: reject emails if username is not found
            String path = tprintf(SV_Fmt "/" SV_Fmt "/inbox", SV_Arg(get_mail_dir()), SV_Arg(rcpt_to));
            if (!file_exists(path.data)) make_directory(path.data);

            smtp_write(&conn, SV("250 OK"));
        } else if (sv_equal_ignore_case(cmd, SV("DATA"))) {
            state = SMTP_STATE_DATA;
            smtp_write(&conn, SV("354 Start input, end with <CRLF>.<CRLF>"));

            // read body until "\r\n.\r\n"
            err = smtp_recv(&conn, CRLF "." CRLF);
            if (has_error(err)) break;

            String body = sv_clone(sb_to_sv(&conn.read_buf));
            body.length -= strlen(CRLF "." CRLF);

            String filename = tprintf(SV_Fmt "/" SV_Fmt "/inbox/%s.eml",
                SV_Arg(get_mail_dir()),
                SV_Arg(rcpt_to),
                sv_to_tmp_c(random_id()));
            write_entire_file(filename.data, body);

            smtp_write(&conn, SV("250 OK queued"));

        } else if (sv_equal_ignore_case(cmd, SV("QUIT"))) {
            state = SMTP_STATE_QUIT;
            smtp_write(&conn, SV("221 Bye"));

        } else {
            smtp_write(&conn, SV("502 Command not implemented"));
        }
    }

    safe_free(mail_from.data);
    safe_free(rcpt_to.data);

    close(client_fd);
    return NULL;
}

Error smtp_server_listen(const SmtpServer* server) {
    assert(server != NULL);
    assert(server->sock_fd > 0);

    if (listen(server->sock_fd, SMTP_SOCKET_BACKLOG) < 0) {
        return errorf("listen failed: %s\n", strerror(errno));
    }

    INFO("server started");
    while (true) {
        const int client_fd = accept(server->sock_fd, NULL, NULL);
        if (client_fd < 0) {
            ERROR("accept failed: %s\n", strerror(errno));
            continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, (void*)(intptr_t)client_fd) != 0) {
            close(client_fd);
            ERROR("pthread_create failed: %s\n", strerror(errno));
            continue;
        }

        pthread_detach(tid);
    }

    assert(false && "unreachable");
}
