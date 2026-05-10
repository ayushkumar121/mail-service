#ifndef SMTP_H
#define SMTP_H

#include <netinet/in.h>

#include "basic.h"


// SMTP Server

typedef struct {
    int sock_fd;
    struct sockaddr_in addr;
} SmtpServer;

Error smtp_server_init(SmtpServer *server);
Error smtp_server_listen(const SmtpServer *server);

// Forward an already-formed RFC822 message to its recipient via direct MX
// delivery: looks up the MX record for the recipient's domain (falling back
// to A record) and connects on port 25. `raw_msg` is sent verbatim as DATA
// payload — caller is responsible for proper CRLF line endings and dot-stuffing.
Error smtp_deliver(String from, String to, String raw_msg);

#endif