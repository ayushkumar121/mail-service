#ifndef IMAP_H
#define IMAP_H
#include <netinet/in.h>

#include "basic.h"

typedef struct {
    int sock_fd;
    struct sockaddr_in addr;
} ImapServer;

Error imap_server_init(ImapServer *server);
Error imap_server_listen(const ImapServer *server);

#endif