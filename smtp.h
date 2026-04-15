#ifndef SMTP_H
#define SMTP_H

#include "basic.h"

typedef struct {
    String from;
    String to;
    String subject;
    String body;
    String password;
} Email;

Error smtp_send(Email email);

#endif