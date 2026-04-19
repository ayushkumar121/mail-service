#include "imap.h"

#include <pthread.h>
#include <unistd.h>

#include <errno.h>
#include <dirent.h>
#include <sys/socket.h>

#include "config.h"

#define IMAP_SOCKET_BACKLOG 1024
#define IMAP_DEFAULT_PORT 143
#define CRLF "\r\n"

typedef enum {
    IMAP_STATE_NOT_AUTHENTICATED,
    IMAP_STATE_AUTHENTICATED,
    IMAP_STATE_SELECTED,
    IMAP_STATE_LOGOUT,
} ImapState;

typedef struct {
    ImapState state;
    String user;
    String selected_mailbox;
} ImapSession;

Error handle_login(BufIO* bio, ImapSession* session, String tag, String args) {
    // TODO: real password check
    StringPair pair = sv_split_delim(args, ' ');
    session->user = sv_clone(sv_trim(pair.first));
    session->state = IMAP_STATE_AUTHENTICATED;
    return bufio_writeln(bio, tprintf(SV_Fmt " OK LOGIN completed", SV_Arg(tag)));
}

Error handle_list(BufIO* bio, String tag) {
    Error err = bufio_writeln(bio, SV("* LIST () \"/\" \"INBOX\""));
    if (has_error(err)) return err;
    return bufio_writeln(bio, tprintf(SV_Fmt " OK LIST completed", SV_Arg(tag)));
}

Error handle_select(BufIO* bio, ImapSession* session, String tag, String mailbox) {
    mailbox = sv_trim(mailbox);
    session->selected_mailbox = sv_clone(mailbox);
    session->state = IMAP_STATE_SELECTED;

    String path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                          SV_Arg(get_maildir()),
                          SV_Arg(session->user),
                          SV_Arg(mailbox));

    int count = 0;
    DIR* dir = opendir(path.data);
    if (dir != NULL) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            String name = SV2(entry->d_name, strlen(entry->d_name));
            if (sv_find(name, ".eml") != -1) count++;
        }
        closedir(dir);
    }

    Error err = bufio_writeln(bio, tprintf("* %d EXISTS", count));
    if (has_error(err)) return err;
    err = bufio_writeln(bio, SV("* 0 RECENT"));
    if (has_error(err)) return err;
    return bufio_writeln(bio, tprintf(SV_Fmt " OK SELECT completed", SV_Arg(tag)));
}

Error handle_fetch(BufIO* bio, const ImapSession* session, String tag, String args) {
    char* endptr = NULL;
    StringPair pair = sv_split_delim(args, ' ');
    int seq = sv_to_int(sv_trim(pair.first), &endptr);

    String path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                          SV_Arg(get_maildir()),
                          SV_Arg(session->user),
                          SV_Arg(session->selected_mailbox));

    DIR* dir = opendir(path.data);
    if (dir == NULL) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO mailbox not found", SV_Arg(tag)));
    }

    int count = 0;
    struct dirent* entry;
    String filename = StringNil;
    while ((entry = readdir(dir)) != NULL) {
        String name = SV2(entry->d_name, strlen(entry->d_name));
        if (sv_find(name, ".eml") != -1) {
            count++;
            if (count == seq) {
                filename = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(path), SV_Arg(name));
                break;
            }
        }
    }
    closedir(dir);

    if (filename.length == 0) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO message not found", SV_Arg(tag)));
    }

    StringBuilder body = {0};
    Error err = read_entire_file(filename.data, &body);
    if (has_error(err)) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO failed to read message", SV_Arg(tag)));
    }

    err = bufio_writeln(bio, tprintf("* %d FETCH (BODY[] {%zu}", seq, body.length));
    if (has_error(err)) return err;
    err = bufio_write(bio, sb_to_sv(&body));
    if (has_error(err)) return err;
    err = bufio_writeln(bio, SV(")"));
    if (has_error(err)) return err;
    return bufio_writeln(bio, tprintf(SV_Fmt " OK FETCH completed", SV_Arg(tag)));
}

static void* handle_client(void* p) {
    int client_fd = (int)(intptr_t)p;
    BufIO bio = {.fd = client_fd};
    ImapSession session = {.state = IMAP_STATE_NOT_AUTHENTICATED};

    bufio_writeln(&bio, SV("* OK IMAP server ready"));

    while (session.state != IMAP_STATE_LOGOUT) {
        Error err = bufio_read_until(&bio, CRLF);
        if (has_error(err)) {
            ERROR("imap read failed: " SV_Fmt, SV_Arg(err.message));
            break;
        }

        const String line = sv_trim(sb_to_sv(&bio.read_buf));
        StringPair tag_rest = sv_split_delim(line, ' ');
        const String tag  = sv_trim(tag_rest.first);
        StringPair cmd_args = sv_split_delim(sv_trim(tag_rest.second), ' ');
        const String cmd  = sv_trim(cmd_args.first);
        const String args = sv_trim(cmd_args.second);

        if (sv_equal_ignore_case(cmd, SV("LOGIN"))) {
            handle_login(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("LIST"))) {
            handle_list(&bio, tag);
        } else if (sv_equal_ignore_case(cmd, SV("SELECT"))) {
            handle_select(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("FETCH"))) {
            handle_fetch(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("LOGOUT"))) {
            bufio_writeln(&bio, SV("* BYE logging out"));
            bufio_writeln(&bio, tprintf(SV_Fmt " OK LOGOUT completed", SV_Arg(tag)));
            session.state = IMAP_STATE_LOGOUT;
        } else {
            bufio_writeln(&bio, tprintf(SV_Fmt " BAD command not recognized", SV_Arg(tag)));
        }
    }

    safe_free(session.user.data);
    safe_free(session.selected_mailbox.data);
    bufio_close(&bio);
    return NULL;
}

Error imap_server_init(ImapServer* server) {
    assert(server != NULL);

    server->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->sock_fd < 0) {
        return errorf("socket failed: %s", strerror(errno));
    }

    int socket_options = 1;
#ifdef SO_REUSEADDR
    if (setsockopt(server->sock_fd, SOL_SOCKET, SO_REUSEADDR, &socket_options,
                   sizeof(socket_options)) < 0) {
        return errorf("setsockopt failed: %s", strerror(errno));
    }
#endif
#ifdef SO_REUSEPORT
    if (setsockopt(server->sock_fd, SOL_SOCKET, SO_REUSEPORT, &socket_options,
                   sizeof(socket_options)) < 0) {
        return errorf("setsockopt failed: %s", strerror(errno));
    }
#endif

    int port = config_get_int(SV("server.imap.port"), IMAP_DEFAULT_PORT);

    server->addr.sin_family = AF_INET;
    server->addr.sin_addr.s_addr = INADDR_ANY;
    server->addr.sin_port = htons(port);

    if (bind(server->sock_fd, (struct sockaddr*)&server->addr, sizeof(server->addr)) < 0) {
        return errorf("bind failed: %s", strerror(errno));
    }

    return ErrorNil;
}

Error imap_server_listen(const ImapServer* server) {
    assert(server != NULL);
    assert(server->sock_fd > 0);

    if (listen(server->sock_fd, IMAP_SOCKET_BACKLOG) < 0) {
        return errorf("listen failed: %s\n", strerror(errno));
    }

    INFO("IMAP server started");
    while (true) {
        const int client_fd = accept(server->sock_fd, NULL, NULL);
        if (client_fd < 0) {
            ERROR("accept failed: %s\n", strerror(errno));
            continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, (void*)(intptr_t)client_fd) != 0) {
            close(client_fd);
            ERROR("pthread_create failed: %s\n", strerror(errno));
            continue;
        }

        pthread_detach(tid);
    }

    assert(false && "unreachable");
}