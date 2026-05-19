#ifndef METRICS_H
#define METRICS_H

#include "http.h"

#include <stdatomic.h>

// SMTP inbound
extern _Atomic(uint64_t) mail_received_total;
extern _Atomic(uint64_t) mail_delivered_local_total;
// SMTP outbound
extern _Atomic(uint64_t) mail_sent_total;
// Failures
extern _Atomic(uint64_t) mail_failures_relay;
extern _Atomic(uint64_t) mail_failures_maildir_write;
// SMTP connections
extern _Atomic(uint64_t) smtp_connections_total;
extern _Atomic(int)      smtp_connections_active;
// IMAP
extern _Atomic(uint64_t) imap_connections_total;
extern _Atomic(int)      imap_connections_active;
extern _Atomic(uint64_t) imap_logins_total;
extern _Atomic(uint64_t) imap_fetch_total;
extern _Atomic(uint64_t) imap_search_total;

HttpResponse metrics_handler(const HttpRequest* request);

#endif
