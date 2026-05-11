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

typedef void * (*ListenerFn)(void*);

typedef struct {
    const char* name;
    ListenerFn fn;
    void* arg;
    pthread_t tid;
} Listener;

static void *smtp_thread(void* arg) {
    try(smtp_server_listen((SmtpServer*)arg));
    return NULL;
}

static void *imap_thread(void* arg) {
    try(imap_server_listen((ImapServer*)arg));
    return NULL;
}

int main(int argc, char** argv) {
    setup_signals();

    const char* config_path = argc > 1 ? argv[1] : "config.json";
    try(config_load(config_path));

    INFO("Setting up maildir: "SV_Fmt, SV_Arg(get_maildir()));

    SmtpServer smtp_server = {0};
    try(smtp_server_init(&smtp_server));

    ImapServer imap_server = {0};
    try(imap_server_init(&imap_server));

    Listener listeners[] = {
        {.name = "smtp", .fn = smtp_thread, .arg = &smtp_server},
        {.name = "imap", .fn = imap_thread, .arg = &imap_server},
    };
    const size_t n = sizeof(listeners) / sizeof(listeners[0]);

    for (size_t i = 0; i < n; i++) {
        if (pthread_create(&listeners[i].tid, NULL, listeners[i].fn, listeners[i].arg) != 0) {
            CRITICAL("failed to start %s listener: %s", listeners[i].name, strerror(errno));
            return 1;
        }
        INFO("started %s listener", listeners[i].name);
    }

    // Main blocks here.
    for (size_t i = 0; i < n; i++) {
        pthread_join(listeners[i].tid, NULL);
        WARN("%s listener exited", listeners[i].name);
    }

    return 0;
}
