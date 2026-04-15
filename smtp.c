#include "smtp.h"

#include <errno.h>
#include <resolv.h>
#include <arpa/nameser.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#define CRLF "\r\n"
#define SMTP_SOCKET_READ_TIMEOUT 5
#define SMTP_RESPONSE_CODE_SUCCESS 250

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
    String host;
    int port;
    StringBuilder read_buf;
    StringBuilder write_buf;
} SmtpConnection;

SmtpConnection smtp_connection_init(String host, int port) {
    return (SmtpConnection){
        .host = host,
        .port = port,
    };
}

Error smtp_connect(SmtpConnection* conn) {
    assert(conn != NULL);

    char* host_cstr = sv_to_tmp_c(conn->host);
    char* port_cstr = tprintf("%d", conn->port).data;

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res;
    if (getaddrinfo(host_cstr, port_cstr, &hints, &res) != 0) {
        return errorf("getaddrinfo failed for " SV_Fmt ": %s", SV_Arg(conn->host), strerror(errno));
    }

    conn->sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (conn->sock_fd < 0) {
        freeaddrinfo(res);
        return errorf("socket failed: %s", strerror(errno));
    }

    if (connect(conn->sock_fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        return errorf("connect failed to " SV_Fmt ":%d : %s", SV_Arg(conn->host), conn->port,
                      strerror(errno));
    }

    struct timeval tv = { .tv_sec = SMTP_SOCKET_READ_TIMEOUT, .tv_usec = 0 };
    if (setsockopt(conn->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        return errorf("setsockopt failed: %s", strerror(errno));
    }

    INFO("Connected to: "SV_Fmt" %d", SV_Arg(conn->host), conn->port);

    freeaddrinfo(res);
    return ErrorNil;
}

Error smtp_read(SmtpConnection* conn) {
    char buf[512];
    while (true) {
        ssize_t n = read(conn->sock_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return errorf("smtp read failed: %s", strerror(errno));
        }
        if (n == 0) return errorf("smtp connection closed");
        sb_push_sv(&conn->read_buf, SV2(buf, n));

        // check if last line
        const String sv = sb_to_sv(&conn->read_buf);
        if (sv.length >= 4 && sv.data[3] == ' ') break;
    }

    DEBUG("smtp recv: " SV_Fmt, SV_Arg(sb_to_sv(&conn->read_buf)));
    return ErrorNil;
}

Error smtp_write(SmtpConnection* conn, const String data) {
    conn->write_buf.length = 0; // reset
    sb_push_sv(&conn->write_buf, data);
    sb_push_str(&conn->write_buf, CRLF);

    DEBUG("smtp send: " SV_Fmt, SV_Arg(sb_to_sv(&conn->write_buf)));

    size_t total_written = 0;
    while (total_written < conn->write_buf.length) {
        ssize_t n = write(conn->sock_fd, conn->write_buf.data + total_written,
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

bool smtp_response_ok(const SmtpConnection* conn, const int expected_code) {
    String response = sb_to_sv(&conn->read_buf);
    if (response.length < 3) return false;

    char *endptr = NULL;
    const String code_sv = SV2(response.data, 3);
    int code = sv_to_int(code_sv, &endptr);
    return code == expected_code;
}

Error smtp_send(const Email email) {
    Error err;
    const String domain = sv_split_delim(email.from, '@').second;

    StringBuilder smtp_server = {0};
    err = smtp_lookup_server(domain, &smtp_server);
    if (has_error(err)) { return err; }

    // SMTP server stuff
    SmtpConnection conn = smtp_connection_init(sb_to_sv(&smtp_server), 25);
    smtp_connect(&conn);

    err = smtp_read(&conn);
    if (has_error(err)) { return err; }

    err = smtp_write(&conn, SV("EHLO ayush-kumar.com")); // TODO: my host
    if (has_error(err)) { return err; }

    err = smtp_read(&conn);
    if (has_error(err)) { return err; }

    if (!smtp_response_ok(&conn, SMTP_RESPONSE_CODE_SUCCESS)) {
        return errorf("EHLO failed");
    }

    return ErrorNil;
}
