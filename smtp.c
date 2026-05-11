#define LOG_PREFIX "smtp: "
#include "smtp.h"
#include "maildir.h"

#include <errno.h>
#include <resolv.h>
#include <arpa/nameser.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

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
    assert(bio != NULL);

    char* host_cstr = sv_to_tmp_c(host);
    char* port_cstr = tprintf("%d", port).data;

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res;
    if (getaddrinfo(host_cstr, port_cstr, &hints, &res) != 0) {
        return errorf("getaddrinfo failed for " SV_Fmt ": %s", SV_Arg(host), strerror(errno));
    }

    bio->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (bio->fd < 0) {
        freeaddrinfo(res);
        return errorf("socket failed: %s", strerror(errno));
    }

    if (connect(bio->fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        return errorf("connect failed to " SV_Fmt ": %d:%s", SV_Arg(host), port, strerror(errno));
    }

    DEBUG("Connected to: " SV_Fmt " %d", SV_Arg(host), port);

    freeaddrinfo(res);
    return ErrorNil;
}

Error smtp_read_from_server(BufIO* bio) {
    bio->read_buf.length = 0;
    char buf[512];

    bool finished = false;
    size_t scan_offset = 0;

    while (!finished) {
        const ssize_t n = read(bio->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return errorf("smtp read failed: %s", strerror(errno));
        }
        if (n == 0) return errorf("smtp connection closed");
        sb_push_sv(&bio->read_buf, SV2(buf, n));

        const String sv = SV2(bio->read_buf.data + scan_offset, bio->read_buf.length - scan_offset);
        StringPair pair = sv_split_str(sv, CRLF);
        while (pair.first.length > 0) {
            const String line = pair.first;
            if (line.length >= 4 && line.data[3] == ' ') {
                finished = true;
                break;
            }
            pair = sv_split_str(pair.second, CRLF);
        }
        scan_offset += n;
    }

    DEBUG("recv: " SV_Fmt, SV_Arg(bio->read_buf));
    return ErrorNil;
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
    bufio_close(&bio);
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

static void* handle_client(void* p) {
    const int client_fd = (int)(intptr_t)p;
    BufIO bio = {.fd = client_fd};

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

        if (sv_equal_ignore_case(cmd, SV("EHLO")) || sv_equal_ignore_case(cmd, SV("HELO"))) {
            state = SMTP_STATE_GREETED;
            safe_free(ehlo_host.data);
            ehlo_host = sv_clone(rest);
            bufio_send_line(&bio, tprintf("250 " SV_Fmt " greets " SV_Fmt, SV_Arg(get_hostname()), SV_Arg(rest)));

        } else if (sv_equal_ignore_case(cmd, SV("MAIL"))) {
            const StringPair pair = sv_split_delim(rest, '<');
            mail_from = sv_clone(sv_split_delim(pair.second, '>').first);
            state = SMTP_STATE_MAIL_FROM;
            bufio_send_line(&bio, SV("250 OK"));

        } else if (sv_equal_ignore_case(cmd, SV("RCPT"))) {
            const StringPair pair = sv_split_delim(rest, '<');
            rcpt_to = sv_clone(sv_split_delim(pair.second, '>').first);
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
                write_entire_file(filename.data, body);
                INFO("saved " SV_Fmt " (%zu bytes) for " SV_Fmt,
                     SV_Arg(filename), body.length, SV_Arg(rcpt_to));
                bufio_send_line(&bio, SV("250 OK queued"));
            } else {
                INFO("delivering to " SV_Fmt " from " SV_Fmt, SV_Arg(rcpt_to), SV_Arg(mail_from));
                Error rerr = smtp_deliver(mail_from, rcpt_to, body);
                if (has_error(rerr)) {
                    ERROR("delivery failed: " SV_Fmt, SV_Arg(rerr.message));
                    bufio_send_line(&bio, SV("451 delivery failed; try later"));
                } else {
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

    safe_free(mail_from.data);
    safe_free(rcpt_to.data);
    safe_free(ehlo_host.data);
    bufio_close(&bio);
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