#ifndef SMTP_H
#define SMTP_H

#include <netinet/in.h>

#include "basic.h"

// SMTP Client
typedef struct {
    String from;
    String to;
    String subject;
    String body;
    String password;
} Email;

Error smtp_send(Email email);

// Forward an already-formed RFC822 message (headers + body) to a relay host.
// `raw_msg` is sent verbatim as DATA payload — caller is responsible for
// proper CRLF line endings and dot-stuffing (typical: forward exactly what
// we received via SMTP DATA).
Error smtp_relay(String host, int port, String from, String to, String raw_msg);

// SMTP Server

typedef struct {
    int sock_fd;
    struct sockaddr_in addr;
} SmtpServer;

Error smtp_server_init(SmtpServer *server);
Error smtp_server_listen(const SmtpServer *server);

#endif