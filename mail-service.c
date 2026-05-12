#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <errno.h>

#define LOG_PREFIX "main: "

#include "basic.h"
#include "config.h"
#include "imap.h"
#include "smtp.h"

static SmtpServer smtp_server;
static ImapServer imap_server;

static void *smtp_thread(void* arg) {
    try(smtp_server_listen((SmtpServer*)arg));
    return NULL;
}

static void *imap_thread(void* arg) {
    try(imap_server_listen((ImapServer*)arg));
    return NULL;
}

static void crash_handler(int sig) {
    void* frames[64];
    int n = backtrace(frames, 64);
    CRITICAL("caught signal %d", sig);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void setup_signals() {
    // Don't crash when a peer closes the connection mid-write.
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGBUS, crash_handler);
}

int main(int argc, char** argv) {
    setup_signals();

    const char* config_path = argc > 1 ? argv[1] : "config.json";
    try(config_load(config_path));

    INFO("Setting up maildir: "SV_Fmt, SV_Arg(get_maildir()));

    try(smtp_server_init(&smtp_server));
    try(imap_server_init(&imap_server));

    pthread_t smtp_tid;
    pthread_create(&smtp_tid, NULL, smtp_thread, &smtp_server);

    pthread_t imap_tid;
    pthread_create(&imap_tid, NULL, imap_thread, &imap_server);

    pthread_join(smtp_tid, NULL);
    pthread_join(imap_tid, NULL);

    CRITICAL("unexpected shutdown");
}
