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
#define IMAP_CAPABILITY "IMAP4rev1"

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

// ---------------------------------------------------------------------------
// Maildir flag helpers
// Maildir encodes flags as a filename suffix: "basename.eml:2,DFrst"
// Letters: D=Draft F=Flagged R=Replied S=Seen T=Trashed(\Deleted)
// ---------------------------------------------------------------------------

typedef struct {
    bool seen;
    bool deleted;
    bool flagged;
    bool replied;
    bool draft;
} MessageFlags;

static MessageFlags parse_flags(String filename) {
    MessageFlags f = {0};
    ssize_t pos = sv_find(filename, ":2,");
    if (pos < 0) return f;
    for (size_t i = (size_t)pos + 3; i < filename.length; i++) {
        switch (filename.data[i]) {
            case 'S': f.seen    = true; break;
            case 'T': f.deleted = true; break;
            case 'F': f.flagged = true; break;
            case 'R': f.replied = true; break;
            case 'D': f.draft   = true; break;
        }
    }
    return f;
}

// Returns the base name without ":2,..." suffix (tprintf-allocated)
static String flags_basename(String filename) {
    ssize_t pos = sv_find(filename, ":2,");
    if (pos < 0) return filename;
    return tprintf("%.*s", (int)pos, filename.data);
}

// Produces a new filename with flag_char set (keeps flags sorted, tprintf-allocated)
static String set_flag(String filename, char flag_char) {
    MessageFlags f = parse_flags(filename);
    String base = flags_basename(filename);
    switch (flag_char) {
        case 'D': f.draft   = true; break;
        case 'F': f.flagged = true; break;
        case 'R': f.replied = true; break;
        case 'S': f.seen    = true; break;
        case 'T': f.deleted = true; break;
    }
    char flags[8] = {0};
    int n = 0;
    if (f.draft)   flags[n++] = 'D';
    if (f.flagged) flags[n++] = 'F';
    if (f.replied) flags[n++] = 'R';
    if (f.seen)    flags[n++] = 'S';
    if (f.deleted) flags[n++] = 'T';
    return tprintf(SV_Fmt ":2,%s", SV_Arg(base), flags);
}

// Produces IMAP FLAGS string e.g. "(\Seen \Deleted)" (tprintf-allocated)
static String flags_to_imap(MessageFlags f) {
    StringBuilder sb = {0};
    sb_push_char(&sb, '(');
    bool first = true;
    if (f.seen)    { if (!first) sb_push_char(&sb, ' '); sb_push_str(&sb, "\\Seen");    first = false; }
    if (f.deleted) { if (!first) sb_push_char(&sb, ' '); sb_push_str(&sb, "\\Deleted"); first = false; }
    if (f.flagged) { if (!first) sb_push_char(&sb, ' '); sb_push_str(&sb, "\\Flagged"); first = false; }
    if (f.replied) { if (!first) sb_push_char(&sb, ' '); sb_push_str(&sb, "\\Answered"); first = false; }
    if (f.draft)   { if (!first) sb_push_char(&sb, ' '); sb_push_str(&sb, "\\Draft");   first = false; }
    sb_push_char(&sb, ')');
    String result = tprintf("%.*s", (int)sb.length, sb.data);
    sb_free(&sb);
    return result;
}

// Map IMAP flag name (e.g. "\\Seen") to Maildir letter; returns 0 if unknown
static char imap_flag_to_maildir(String imap_flag) {
    if (sv_equal_ignore_case(imap_flag, SV("\\Seen")))    return 'S';
    if (sv_equal_ignore_case(imap_flag, SV("\\Deleted"))) return 'T';
    if (sv_equal_ignore_case(imap_flag, SV("\\Flagged"))) return 'F';
    if (sv_equal_ignore_case(imap_flag, SV("\\Answered"))) return 'R';
    if (sv_equal_ignore_case(imap_flag, SV("\\Draft")))   return 'D';
    return 0;
}

static Error handle_capability(BufIO* bio, String tag) {
    Error err = bufio_writeln(bio, SV("* CAPABILITY " IMAP_CAPABILITY));
    if (has_error(err)) return err;
    return bufio_writeln(bio, tprintf(SV_Fmt " OK CAPABILITY completed", SV_Arg(tag)));
}

static Error handle_noop(BufIO* bio, String tag) {
    return bufio_writeln(bio, tprintf(SV_Fmt " OK NOOP completed", SV_Arg(tag)));
}

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

    int count = 0, unseen = 0;
    DIR* dir = opendir(path.data);
    if (dir != NULL) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            String name = SV2(entry->d_name, strlen(entry->d_name));
            if (sv_find(name, ".eml") == -1) continue;
            count++;
            MessageFlags f = parse_flags(name);
            if (!f.seen) unseen++;
        }
        closedir(dir);
    }

    Error err = bufio_writeln(bio, tprintf("* %d EXISTS", count));
    if (has_error(err)) return err;
    err = bufio_writeln(bio, tprintf("* %d RECENT", unseen));
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

static Error handle_status(BufIO* bio, const ImapSession* session, String tag, String args) {
    // args: "INBOX (MESSAGES UNSEEN RECENT)" — extract mailbox name (first token)
    StringPair pair = sv_split_delim(sv_trim(args), ' ');
    String mailbox = sv_trim(pair.first);

    String path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                          SV_Arg(get_maildir()),
                          SV_Arg(session->user),
                          SV_Arg(mailbox));

    int total = 0, unseen = 0;
    DIR* dir = opendir(path.data);
    if (dir != NULL) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            String name = SV2(entry->d_name, strlen(entry->d_name));
            if (sv_find(name, ".eml") == -1) continue;
            total++;
            MessageFlags f = parse_flags(name);
            if (!f.seen) unseen++;
        }
        closedir(dir);
    }

    Error err = bufio_writeln(bio, tprintf("* STATUS " SV_Fmt " (MESSAGES %d UNSEEN %d RECENT 0)",
                                           SV_Arg(mailbox), total, unseen));
    if (has_error(err)) return err;
    return bufio_writeln(bio, tprintf(SV_Fmt " OK STATUS completed", SV_Arg(tag)));
}

static Error handle_store(BufIO* bio, const ImapSession* session, String tag, String args) {
    // args: "seq +FLAGS (\Seen)"
    StringPair seq_rest = sv_split_delim(sv_trim(args), ' ');
    char* endptr = NULL;
    int seq = sv_to_int(sv_trim(seq_rest.first), &endptr);

    // skip "+FLAGS" token, get the flags list
    StringPair op_flags = sv_split_delim(sv_trim(seq_rest.second), ' ');
    String flags_str = sv_trim(op_flags.second); // e.g. "(\Seen)" or "(\Deleted)"

    // strip surrounding parens
    if (flags_str.length >= 2 && flags_str.data[0] == '(') {
        flags_str.data++;
        flags_str.length -= 2;
    }
    flags_str = sv_trim(flags_str);
    INFO("flags_str: "SV_Fmt, SV_Arg(flags_str));

    char maildir_flag = imap_flag_to_maildir(flags_str);
    if (maildir_flag == 0) {
        return bufio_writeln(bio, tprintf(SV_Fmt " BAD unknown flag", SV_Arg(tag)));
    }

    String dir_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()),
                              SV_Arg(session->user),
                              SV_Arg(session->selected_mailbox));

    DIR* dir = opendir(dir_path.data);
    if (dir == NULL) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO mailbox not found", SV_Arg(tag)));
    }

    int count = 0;
    struct dirent* entry;
    String old_name = StringNil;
    while ((entry = readdir(dir)) != NULL) {
        String name = SV2(entry->d_name, strlen(entry->d_name));
        if (sv_find(name, ".eml") == -1) continue;
        count++;
        if (count == seq) {
            old_name = sv_clone(name);
            break;
        }
    }
    closedir(dir);

    if (old_name.length == 0) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO message not found", SV_Arg(tag)));
    }

    String new_name = set_flag(old_name, maildir_flag);
    String old_path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(dir_path), SV_Arg(old_name));
    String new_path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(dir_path), SV_Arg(new_name));

    if (!sv_equal(old_name, new_name)) {
        rename(old_path.data, new_path.data);
    }
    safe_free(old_name.data);

    MessageFlags f = parse_flags(new_name);
    String imap_flags = flags_to_imap(f);

    Error err = bufio_writeln(bio, tprintf("* %d FETCH (FLAGS %s)", seq, imap_flags.data));
    if (has_error(err)) return err;
    return bufio_writeln(bio, tprintf(SV_Fmt " OK STORE completed", SV_Arg(tag)));
}

static Error handle_expunge(BufIO* bio, const ImapSession* session, String tag) {
    String dir_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()),
                              SV_Arg(session->user),
                              SV_Arg(session->selected_mailbox));

    DIR* dir = opendir(dir_path.data);
    if (dir == NULL) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO mailbox not found", SV_Arg(tag)));
    }

    // Collect all .eml filenames
    typedef ARRAY(String) StringArray;
    StringArray files = {0};
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        String name = SV2(entry->d_name, strlen(entry->d_name));
        if (sv_find(name, ".eml") == -1) continue;
        array_append(&files, sv_clone(name));
    }
    closedir(dir);

    // Walk in order; track current sequence number (shrinks as we delete)
    int seq = 1;
    Error err = ErrorNil;
    for (size_t i = 0; i < files.length; i++) {
        MessageFlags f = parse_flags(files.data[i]);
        if (f.deleted) {
            String full_path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(dir_path), SV_Arg(files.data[i]));
            remove(full_path.data);
            err = bufio_writeln(bio, tprintf("* %d EXPUNGE", seq));
            if (has_error(err)) break;
            // do not increment seq — sequence numbers shift down after each expunge
        } else {
            seq++;
        }
        safe_free(files.data[i].data);
    }
    array_free(&files);

    if (has_error(err)) return err;
    return bufio_writeln(bio, tprintf(SV_Fmt " OK EXPUNGE completed", SV_Arg(tag)));
}

static void* handle_client(void* p) {
    int client_fd = (int)(intptr_t)p;
    BufIO bio = {.fd = client_fd};
    ImapSession session = {.state = IMAP_STATE_NOT_AUTHENTICATED};

    bufio_writeln(&bio, SV("* OK [CAPABILITY " IMAP_CAPABILITY "] IMAP server ready"));

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

        if (sv_equal_ignore_case(cmd, SV("CAPABILITY"))) {
            handle_capability(&bio, tag);
        } else if (sv_equal_ignore_case(cmd, SV("NOOP"))) {
            handle_noop(&bio, tag);
        } else if (sv_equal_ignore_case(cmd, SV("LOGIN"))) {
            handle_login(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("LIST"))) {
            handle_list(&bio, tag);
        } else if (sv_equal_ignore_case(cmd, SV("SELECT"))) {
            handle_select(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("EXAMINE"))) {
            handle_select(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("STATUS"))) {
            handle_status(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("FETCH"))) {
            handle_fetch(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("STORE"))) {
            handle_store(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("EXPUNGE"))) {
            handle_expunge(&bio, &session, tag);
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