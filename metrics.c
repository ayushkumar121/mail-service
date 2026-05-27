#include "metrics.h"

#include <time.h>
#include <unistd.h>
#include "config.h"

atomic_int mail_received_total;
atomic_int mail_delivered_local_total;
atomic_int mail_sent_total;
atomic_int mail_failures_relay;
atomic_int mail_failures_maildir_write;
atomic_int smtp_connections_total;
atomic_int smtp_connections_active;
atomic_int imap_connections_total;
atomic_int imap_connections_active;
atomic_int imap_logins_total;
atomic_int imap_fetch_total;
atomic_int imap_search_total;

static StringBuilder sb = {0};

static void emit_mertic(const char* label, long value) {
    sb_push_str(&sb, label);
    sb_push_char(&sb, '=');
    sb_push_long(&sb, value);
    sb_push_char(&sb, '\n');
}

static void log_metrics(long timestamp) {
    INFO("Logging metrics");

    sb.length = 0;
    sb_push_str(&sb, "# METRICS_LOG "); sb_push_long(&sb, timestamp); sb_push_char(&sb, '\n');
    emit_mertic("mail_received_total", atomic_load(&mail_received_total));
    emit_mertic("mail_delivered_local_total", atomic_load(&mail_delivered_local_total));
    emit_mertic("mail_sent_total", atomic_load(&mail_sent_total));
    emit_mertic("mail_failures_total{reason=\"relay\"}", atomic_load(&mail_failures_relay));
    emit_mertic("mail_failures_total{reason=\"maildir_write\"}", atomic_load(&mail_failures_maildir_write));
    emit_mertic("smtp_connections_total", atomic_load(&smtp_connections_total));
    emit_mertic("smtp_connections_active", atomic_load(&smtp_connections_active));
    emit_mertic("imap_connections_total", atomic_load(&imap_connections_total));
    emit_mertic("imap_connections_active", atomic_load(&imap_connections_active));
    emit_mertic("imap_logins_total", atomic_load(&imap_logins_total));
    emit_mertic("imap_fetch_total", atomic_load(&imap_fetch_total));
    emit_mertic("imap_search_total", atomic_load(&imap_search_total));

    String log_path = tprintf(SV_Fmt"/metrics.log", SV_Arg(get_logdir()));
    FILE* f = fopen(log_path.data, "a");
    fwrite(sb.data, sb.length, 1, f);
    fclose(f);
}

void metrics_logger(int frequency_seconds) {
    while (1) {
        log_metrics(time(NULL));
        sleep(frequency_seconds);
    }
}
