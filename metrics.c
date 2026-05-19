#include "metrics.h"

// SMTP inbound
_Atomic(uint64_t) mail_received_total          = 0;
_Atomic(uint64_t) mail_delivered_local_total   = 0;
// SMTP outbound
_Atomic(uint64_t) mail_sent_total              = 0;
// Failures
_Atomic(uint64_t) mail_failures_relay          = 0;
_Atomic(uint64_t) mail_failures_maildir_write  = 0;
// SMTP connections
_Atomic(uint64_t) smtp_connections_total       = 0;
_Atomic(int)      smtp_connections_active       = 0;
// IMAP
_Atomic(uint64_t) imap_connections_total       = 0;
_Atomic(int)      imap_connections_active       = 0;
_Atomic(uint64_t) imap_logins_total            = 0;
_Atomic(uint64_t) imap_fetch_total             = 0;
_Atomic(uint64_t) imap_search_total            = 0;

static void emit_counter(StringBuilder *sb, const char *name,
                         const char *help, uint64_t v) {
    sb_push_str(sb, "# HELP "); sb_push_str(sb, name);
    sb_push_str(sb, " "); sb_push_str(sb, help); sb_push_str(sb, "\n");
    sb_push_str(sb, "# TYPE "); sb_push_str(sb, name); sb_push_str(sb, " counter\n");
    sb_push_str(sb, name); sb_push_str(sb, " ");
    sb_push_long(sb, v); sb_push_str(sb, "\n");
}

static void emit_gauge(StringBuilder *sb, const char *name,
                       const char *help, long v) {
    sb_push_str(sb, "# HELP "); sb_push_str(sb, name);
    sb_push_str(sb, " "); sb_push_str(sb, help); sb_push_str(sb, "\n");
    sb_push_str(sb, "# TYPE "); sb_push_str(sb, name); sb_push_str(sb, " gauge\n");
    sb_push_str(sb, name); sb_push_str(sb, " ");
    sb_push_long(sb, v); sb_push_str(sb, "\n");
}

void http_metrics_encode(StringBuilder *sb) {
    emit_counter(sb, "mail_received_total",
                 "Messages accepted via SMTP DATA", atomic_load(&mail_received_total));
    emit_counter(sb, "mail_delivered_local_total",
                 "Messages written to a local maildir", atomic_load(&mail_delivered_local_total));
    emit_counter(sb, "mail_sent_total",
                 "Messages relayed to a remote MX", atomic_load(&mail_sent_total));

    sb_push_str(sb, "# HELP mail_failures_total Failed message deliveries\n");
    sb_push_str(sb, "# TYPE mail_failures_total counter\n");
    sb_push_str(sb, "mail_failures_total{reason=\"relay\"} ");
    sb_push_long(sb, atomic_load(&mail_failures_relay));
    sb_push_str(sb, "\n");
    sb_push_str(sb, "mail_failures_total{reason=\"maildir_write\"} ");
    sb_push_long(sb, atomic_load(&mail_failures_maildir_write));
    sb_push_str(sb, "\n");

    emit_counter(sb, "smtp_connections_total",
                 "Total inbound SMTP sessions", atomic_load(&smtp_connections_total));
    emit_gauge(sb, "smtp_connections_active",
               "Active inbound SMTP sessions", atomic_load(&smtp_connections_active));

    emit_counter(sb, "imap_connections_total",
                 "Total IMAP sessions", atomic_load(&imap_connections_total));
    emit_gauge(sb, "imap_connections_active",
               "Active IMAP sessions", atomic_load(&imap_connections_active));
    emit_counter(sb, "imap_logins_total",
                 "Successful IMAP LOGIN commands", atomic_load(&imap_logins_total));
    emit_counter(sb, "imap_fetch_total",
                 "IMAP FETCH/UID FETCH commands", atomic_load(&imap_fetch_total));
    emit_counter(sb, "imap_search_total",
                 "IMAP SEARCH/UID SEARCH commands", atomic_load(&imap_search_total));
}

HttpResponse metrics_handler(const HttpRequest* request) {
    StringBuilder res = {0};
    http_metrics_encode(&res);
    return http_text_response(200, sb_to_sv(&res));
}
