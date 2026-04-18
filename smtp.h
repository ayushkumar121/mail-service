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

// SMTP Server

typedef struct {
    int sock_fd;
    struct sockaddr_in addr;
} SmtpServer;

Error smtp_server_init(SmtpServer *server);
Error smtp_server_listen(const SmtpServer *server);

#endif