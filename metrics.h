#ifndef METRICS_H
#define METRICS_H

#include <stdatomic.h>

#include "basic.h"

extern atomic_int mail_received_total;
extern atomic_int mail_delivered_local_total;
extern atomic_int mail_sent_total;
extern atomic_int mail_failures_relay;
extern atomic_int mail_failures_maildir_write;
extern atomic_int smtp_connections_total;
extern atomic_int smtp_connections_active;
extern atomic_int imap_connections_total;
extern atomic_int imap_connections_active;
extern atomic_int imap_logins_total;
extern atomic_int imap_fetch_total;
extern atomic_int imap_search_total;

// This blocks for every frequency_seconds this logs metrics to logdir (defined in config)
void metrics_logger(int frequency_seconds);

#endif
