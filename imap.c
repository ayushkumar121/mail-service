#include "imap.h"

#include <pthread.h>
#include <unistd.h>

#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/socket.h>
#include <sys/stat.h>

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
            default: break;
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
        default: break;
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

// Strip surrounding double-quotes from a string token if present
static String strip_quotes(String s) {
    if (s.length >= 2 && s.data[0] == '"' && s.data[s.length - 1] == '"') {
        return SV2(s.data + 1, s.length - 2);
    }
    return s;
}

// Replace '%' with '*' so POSIX fnmatch handles flat-maildir LIST patterns
static String normalize_pattern(String pat) {
    StringBuilder sb = {0};
    for (size_t i = 0; i < pat.length; i++) {
        sb_push_char(&sb, pat.data[i] == '%' ? '*' : pat.data[i]);
    }
    String result = tprintf("%.*s", (int)sb.length, sb.data);
    sb_free(&sb);
    return result;
}

static Error handle_list_like(BufIO* bio, const ImapSession* session, String tag, String args, const char* cmd_name) {
    StringPair ref_pat = sv_split_delim(sv_trim(args), ' ');
    String reference   = strip_quotes(sv_trim(ref_pat.first));
    String pattern     = strip_quotes(sv_trim(ref_pat.second));

    // Hierarchy-delimiter probe: LIST "" ""
    if (pattern.length == 0) {
        Error err = bufio_writeln(bio, tprintf("* %s (\\Noselect) \"/\" \"\"", cmd_name));
        if (has_error(err)) return err;
        return bufio_writeln(bio, tprintf(SV_Fmt " OK %s completed", SV_Arg(tag), cmd_name));
    }

    // RFC 3501: reference and pattern are concatenated directly (no separator)
    // e.g. LIST "INBOX" "*" -> effective pattern "INBOX*" which matches "INBOX"
    String full_pattern = reference.length > 0
        ? tprintf(SV_Fmt SV_Fmt, SV_Arg(reference), SV_Arg(pattern))
        : pattern;
    String norm = normalize_pattern(full_pattern);
    const char* pat_cstr = sv_to_tmp_c(norm);

    String user_dir = tprintf(SV_Fmt "/" SV_Fmt,
                               SV_Arg(get_maildir()),
                               SV_Arg(session->user));
    DIR* dir = opendir(user_dir.data);
    if (dir == NULL) {
        // User maildir doesn't exist yet — return empty list
        return bufio_writeln(bio, tprintf(SV_Fmt " OK %s completed", SV_Arg(tag), cmd_name));
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_DIR) continue;
        if (entry->d_name[0] == '.') continue;  // skip . and ..

        String name = SV2(entry->d_name, strlen(entry->d_name));
        if (fnmatch(pat_cstr, sv_to_tmp_c(name), FNM_CASEFOLD) != 0) continue;

        Error err = bufio_writeln(bio,
            tprintf("* %s () \"/\" \"" SV_Fmt "\"", cmd_name, SV_Arg(name)));
        if (has_error(err)) { closedir(dir); return err; }
    }
    closedir(dir);
    return bufio_writeln(bio, tprintf(SV_Fmt " OK %s completed", SV_Arg(tag), cmd_name));
}

static Error handle_list(BufIO* bio, const ImapSession* session, String tag, String args) {
    return handle_list_like(bio, session, tag, args, "LIST");
}

static Error handle_lsub(BufIO* bio, const ImapSession* session, String tag, String args) {
    return handle_list_like(bio, session, tag, args, "LSUB");
}

static Error handle_select(BufIO* bio, ImapSession* session, String tag, String mailbox) {
    mailbox = strip_quotes(sv_trim(mailbox));
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

static void handle_subscribe(BufIO* bio, ImapSession* session, String tag, String args) {
    bufio_writeln(bio, tprintf(SV_Fmt " OK SUBSCRIBE completed", SV_Arg(tag)));
}

static void handle_unsubscribe(BufIO* bio, ImapSession* session, String tag, String args) {
    bufio_writeln(bio, tprintf(SV_Fmt " OK UNSUBSCRIBE completed", SV_Arg(tag)));
}
typedef struct {
    bool flags;
    bool uid;
    bool body;
    bool peek;
    bool size;
    bool internaldate;
} FetchItems;

static FetchItems parse_fetch_items(String s, bool force_uid) {
    FetchItems fi = {0};
    s = sv_trim(s);
    if (s.length >= 2 && s.data[0] == '(' && s.data[s.length-1] == ')') {
        s = SV2(s.data + 1, s.length - 2);
    }
    // Macros
    if (sv_equal_ignore_case(s, SV("ALL"))) {
        fi.flags = fi.internaldate = fi.size = true; // skip ENVELOPE
    } else if (sv_equal_ignore_case(s, SV("FAST"))) {
        fi.flags = fi.internaldate = fi.size = true;
    } else if (sv_equal_ignore_case(s, SV("FULL"))) {
        fi.flags = fi.internaldate = fi.size = fi.body = true;
    }
    // Token scan
    size_t i = 0;
    while (i < s.length) {
        while (i < s.length && (s.data[i] == ' ' || s.data[i] == '\t')) i++;
        size_t start = i;
        int depth = 0;
        while (i < s.length) {
            char c = s.data[i];
            if (c == '[' || c == '(') depth++;
            else if (c == ']' || c == ')') depth--;
            else if ((c == ' ' || c == '\t') && depth == 0) break;
            i++;
        }
        String tok = SV2(s.data + start, i - start);
        if (tok.length == 0) continue;
        if (sv_equal_ignore_case(tok, SV("FLAGS"))) fi.flags = true;
        else if (sv_equal_ignore_case(tok, SV("UID"))) fi.uid = true;
        else if (sv_equal_ignore_case(tok, SV("RFC822.SIZE"))) fi.size = true;
        else if (sv_equal_ignore_case(tok, SV("INTERNALDATE"))) fi.internaldate = true;
        else if (sv_equal_ignore_case(tok, SV("RFC822"))) { fi.body = true; }
        else if (tok.length >= 5 && sv_equal_ignore_case(SV2(tok.data, 5), SV("BODY["))) {
            fi.body = true;
        }
        else if (tok.length >= 10 && sv_equal_ignore_case(SV2(tok.data, 10), SV("BODY.PEEK["))) {
            fi.body = true; fi.peek = true;
        }
    }
    if (force_uid) fi.uid = true;
    return fi;
}

typedef struct { int start; int end; } SeqRange;
typedef ARRAY(SeqRange) SeqRangeArray;

static SeqRangeArray parse_seq_set(String s, int max) {
    SeqRangeArray ranges = {0};
    size_t i = 0;
    while (i < s.length) {
        size_t start = i;
        while (i < s.length && s.data[i] != ',') i++;
        String tok = sv_trim(SV2(s.data + start, i - start));
        if (i < s.length) i++; // skip comma
        if (tok.length == 0) continue;

        ssize_t colon = sv_find(tok, ":");
        SeqRange r = {0};
        if (colon < 0) {
            char* end = NULL;
            int n = (tok.length == 1 && tok.data[0] == '*')
                ? max
                : sv_to_int(tok, &end);
            r.start = r.end = n;
        } else {
            String a = sv_trim(SV2(tok.data, (size_t)colon));
            String b = sv_trim(SV2(tok.data + colon + 1, tok.length - (size_t)colon - 1));
            char* end = NULL;
            r.start = (a.length == 1 && a.data[0] == '*') ? max : sv_to_int(a, &end);
            r.end   = (b.length == 1 && b.data[0] == '*') ? max : sv_to_int(b, &end);
            if (r.end < r.start) { int t = r.start; r.start = r.end; r.end = t; }
        }
        if (r.start < 1) r.start = 1;
        if (r.end > max) r.end = max;
        if (r.start <= r.end) array_append(&ranges, r);
    }
    return ranges;
}

static bool seq_in_ranges(const SeqRangeArray* ranges, int seq) {
    for (size_t i = 0; i < ranges->length; i++) {
        if (seq >= ranges->data[i].start && seq <= ranges->data[i].end) return true;
    }
    return false;
}

static Error handle_fetch_ex(BufIO* bio, const ImapSession* session, String tag, String args, bool is_uid) {
    StringPair pair = sv_split_delim(sv_trim(args), ' ');
    String seq_set = sv_trim(pair.first);
    String items_str = sv_trim(pair.second);

    String path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                          SV_Arg(get_maildir()),
                          SV_Arg(session->user),
                          SV_Arg(session->selected_mailbox));

    DIR* dir = opendir(path.data);
    if (dir == NULL) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO mailbox not found", SV_Arg(tag)));
    }

    typedef ARRAY(String) StringArray;
    StringArray names = {0};
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        String name = SV2(entry->d_name, strlen(entry->d_name));
        if (sv_find(name, ".eml") == -1) continue;
        array_append(&names, sv_clone(name));
    }
    closedir(dir);

    int count = (int)names.length;
    SeqRangeArray ranges = parse_seq_set(seq_set, count);
    FetchItems fi = parse_fetch_items(items_str, is_uid);

    Error err = ErrorNil;
    for (int seq = 1; seq <= count; seq++) {
        if (!seq_in_ranges(&ranges, seq)) continue;

        String name = names.data[seq - 1];
        MessageFlags flags = parse_flags(name);

        // BODY[] (not BODY.PEEK[]) implicitly sets \Seen
        if (fi.body && !fi.peek && !flags.seen) {
            String new_entry = set_flag(name, 'S');
            String old_path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(path), SV_Arg(name));
            String new_path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(path), SV_Arg(new_entry));
            rename(old_path.data, new_path.data);
            safe_free(names.data[seq - 1].data);
            names.data[seq - 1] = sv_clone(new_entry);
            name = names.data[seq - 1];
            flags.seen = true;
        }

        StringBuilder line = {0};
        sb_push_sv(&line, tprintf("* %d FETCH (", seq));
        bool first = true;
        if (fi.uid) {
            if (!first) sb_push_char(&line, ' ');
            sb_push_sv(&line, tprintf("UID %d", seq));
            first = false;
        }
        if (fi.flags) {
            if (!first) sb_push_char(&line, ' ');
            sb_push_sv(&line, SV("FLAGS "));
            sb_push_sv(&line, flags_to_imap(flags));
            first = false;
        }
        if (fi.internaldate) {
            if (!first) sb_push_char(&line, ' ');
            sb_push_sv(&line, SV("INTERNALDATE \"01-Jan-1970 00:00:00 +0000\""));
            first = false;
        }

        StringBuilder body_sb = {0};
        bool has_body = false;
        if (fi.body || fi.size) {
            String full_path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(path), SV_Arg(name));
            err = read_entire_file(full_path.data, &body_sb);
            if (has_error(err)) {
                sb_free(&line); sb_free(&body_sb);
                err = bufio_writeln(bio, tprintf(SV_Fmt " NO failed to read message", SV_Arg(tag)));
                goto cleanup;
            }
            has_body = true;
        }
        if (fi.size) {
            if (!first) sb_push_char(&line, ' ');
            sb_push_sv(&line, tprintf("RFC822.SIZE %zu", body_sb.length));
            first = false;
        }
        if (fi.body) {
            if (!first) sb_push_char(&line, ' ');
            sb_push_sv(&line, tprintf("BODY[] {%zu}", body_sb.length));
            // emit prefix line, body literal, then closing paren
            sb_push_sv(&line, SV("\r\n"));
            err = bufio_write(bio, sb_to_sv(&line));
            sb_free(&line);
            if (has_error(err)) { sb_free(&body_sb); goto cleanup; }
            err = bufio_write(bio, sb_to_sv(&body_sb));
            sb_free(&body_sb);
            if (has_error(err)) goto cleanup;
            err = bufio_writeln(bio, SV(")"));
            if (has_error(err)) goto cleanup;
            continue;
        }

        if (has_body) sb_free(&body_sb);
        sb_push_char(&line, ')');
        err = bufio_writeln(bio, sb_to_sv(&line));
        sb_free(&line);
        if (has_error(err)) goto cleanup;
    }

    err = bufio_writeln(bio, tprintf(SV_Fmt " OK %s completed",
                                     SV_Arg(tag), is_uid ? "UID FETCH" : "FETCH"));
cleanup:
    for (size_t i = 0; i < names.length; i++) safe_free(names.data[i].data);
    array_free(&names);
    array_free(&ranges);
    return err;
}

static Error handle_fetch(BufIO* bio, const ImapSession* session, String tag, String args) {
    return handle_fetch_ex(bio, session, tag, args, false);
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

static Error handle_append(BufIO* bio, const ImapSession* session, String tag, String args) {
    args = sv_trim(args);

    ssize_t lbrace = sv_find(args, "{");
    ssize_t rbrace = sv_find(args, "}");
    if (lbrace < 0 || rbrace < 0 || rbrace <= lbrace) {
        return bufio_writeln(bio, tprintf(SV_Fmt " BAD APPEND requires literal {N}", SV_Arg(tag)));
    }

    String size_str = SV2(args.data + lbrace + 1, (size_t)(rbrace - lbrace - 1));
    char* endptr = NULL;
    int size = sv_to_int(sv_trim(size_str), &endptr);
    if (size < 0) {
        return bufio_writeln(bio, tprintf(SV_Fmt " BAD invalid literal size", SV_Arg(tag)));
    }

    String head = sv_trim(SV2(args.data, (size_t)lbrace));

    // Mailbox name: first token, possibly quoted (may contain spaces if quoted)
    String mailbox = StringNil;
    String rest = StringNil;
    if (head.length > 0 && head.data[0] == '"') {
        ssize_t end_quote = -1;
        for (size_t i = 1; i < head.length; i++) {
            if (head.data[i] == '"') { end_quote = (ssize_t)i; break; }
        }
        if (end_quote < 0) {
            return bufio_writeln(bio, tprintf(SV_Fmt " BAD unterminated quoted name", SV_Arg(tag)));
        }
        mailbox = SV2(head.data + 1, (size_t)(end_quote - 1));
        rest = sv_trim(SV2(head.data + end_quote + 1, head.length - (size_t)end_quote - 1));
    } else {
        StringPair p = sv_split_delim(head, ' ');
        mailbox = sv_trim(p.first);
        rest = sv_trim(p.second);
    }

    if (mailbox.length == 0) {
        return bufio_writeln(bio, tprintf(SV_Fmt " BAD mailbox name required", SV_Arg(tag)));
    }

    // Optional flag list "(\Seen \Draft ...)" — anything else (date-time) is ignored.
    MessageFlags flags = {0};
    if (rest.length > 0 && rest.data[0] == '(') {
        ssize_t end_paren = sv_find(rest, ")");
        if (end_paren < 0) {
            return bufio_writeln(bio, tprintf(SV_Fmt " BAD unterminated flag list", SV_Arg(tag)));
        }
        String flag_list = SV2(rest.data + 1, (size_t)(end_paren - 1));
        while (flag_list.length > 0) {
            StringPair fp = sv_split_delim(sv_trim(flag_list), ' ');
            String flag = sv_trim(fp.first);
            if (flag.length == 0) break;
            char c = imap_flag_to_maildir(flag);
            switch (c) {
                case 'S': flags.seen    = true; break;
                case 'T': flags.deleted = true; break;
                case 'F': flags.flagged = true; break;
                case 'R': flags.replied = true; break;
                case 'D': flags.draft   = true; break;
                default: break;
            }
            flag_list = fp.second;
        }
    }

    String dir_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()), SV_Arg(session->user), SV_Arg(mailbox));
    if (!file_exists(dir_path.data)) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO [TRYCREATE] mailbox does not exist", SV_Arg(tag)));
    }

    Error err = bufio_writeln(bio, SV("+ Ready for literal data"));
    if (has_error(err)) return err;

    // bufio_read_n discards overflow, so drain pipelined bytes manually first.
    StringBuilder body_sb = {0};
    if (bio->overflow.length > 0) {
        size_t take = bio->overflow.length < (size_t)size ? bio->overflow.length : (size_t)size;
        sb_push_sv(&body_sb, SV2(bio->overflow.data, take));
        if (take < bio->overflow.length) {
            size_t leftover = bio->overflow.length - take;
            memmove(bio->overflow.data, bio->overflow.data + take, leftover);
            bio->overflow.length = leftover;
        } else {
            bio->overflow.length = 0;
        }
    }
    if (body_sb.length < (size_t)size) {
        err = bufio_read_n(bio, (size_t)size - body_sb.length);
        if (has_error(err)) { sb_free(&body_sb); return err; }
        sb_push_sv(&body_sb, sb_to_sv(&bio->read_buf));
    }
    String body = sv_clone(sb_to_sv(&body_sb));
    sb_free(&body_sb);
    err = bufio_read_until(bio, CRLF);
    if (has_error(err)) { safe_free(body.data); return err; }

    char flag_chars[8] = {0};
    int n = 0;
    if (flags.draft)   flag_chars[n++] = 'D';
    if (flags.flagged) flag_chars[n++] = 'F';
    if (flags.replied) flag_chars[n++] = 'R';
    if (flags.seen)    flag_chars[n++] = 'S';
    if (flags.deleted) flag_chars[n++] = 'T';

    String filename = n > 0
        ? tprintf(SV_Fmt "/%ld." SV_Fmt ".eml:2,%s",
                  SV_Arg(dir_path), (long)time(NULL),
                  SV_Arg(random_id(RANDOM_ID_LEN)), flag_chars)
        : tprintf(SV_Fmt "/%ld." SV_Fmt ".eml",
                  SV_Arg(dir_path), (long)time(NULL),
                  SV_Arg(random_id(RANDOM_ID_LEN)));

    err = write_entire_file(filename.data, body);
    safe_free(body.data);
    if (has_error(err)) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO failed to write message", SV_Arg(tag)));
    }
    return bufio_writeln(bio, tprintf(SV_Fmt " OK APPEND completed", SV_Arg(tag)));
}

static Error handle_create(BufIO* bio, const ImapSession* session, String tag, String args) {
    String name = strip_quotes(sv_trim(args));
    if (name.length == 0) {
        return bufio_writeln(bio, tprintf(SV_Fmt " BAD mailbox name required", SV_Arg(tag)));
    }
    if (sv_equal_ignore_case(name, SV("INBOX"))) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO cannot create INBOX", SV_Arg(tag)));
    }

    String user_dir = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(get_maildir()), SV_Arg(session->user));
    mkdir(user_dir.data, 0700);

    String path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(user_dir), SV_Arg(name));
    if (mkdir(path.data, 0700) != 0) {
        if (errno == EEXIST) {
            return bufio_writeln(bio, tprintf(SV_Fmt " NO mailbox already exists", SV_Arg(tag)));
        }
        return bufio_writeln(bio, tprintf(SV_Fmt " NO %s", SV_Arg(tag), strerror(errno)));
    }
    return bufio_writeln(bio, tprintf(SV_Fmt " OK CREATE completed", SV_Arg(tag)));
}

static Error handle_delete(BufIO* bio, ImapSession* session, String tag, String args) {
    String name = strip_quotes(sv_trim(args));
    if (name.length == 0) {
        return bufio_writeln(bio, tprintf(SV_Fmt " BAD mailbox name required", SV_Arg(tag)));
    }
    if (sv_equal_ignore_case(name, SV("INBOX"))) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO cannot delete INBOX", SV_Arg(tag)));
    }

    String path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                          SV_Arg(get_maildir()), SV_Arg(session->user), SV_Arg(name));

    DIR* dir = opendir(path.data);
    if (dir == NULL) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO mailbox does not exist", SV_Arg(tag)));
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == 0 || (entry->d_name[1] == '.' && entry->d_name[2] == 0))) continue;
        String file_path = tprintf(SV_Fmt "/%s", SV_Arg(path), entry->d_name);
        unlink(file_path.data);
    }
    closedir(dir);

    if (rmdir(path.data) != 0) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO %s", SV_Arg(tag), strerror(errno)));
    }

    if (sv_equal(session->selected_mailbox, name)) {
        safe_free(session->selected_mailbox.data);
        session->selected_mailbox = StringNil;
        session->state = IMAP_STATE_AUTHENTICATED;
    }
    return bufio_writeln(bio, tprintf(SV_Fmt " OK DELETE completed", SV_Arg(tag)));
}

static Error handle_rename(BufIO* bio, ImapSession* session, String tag, String args) {
    StringPair pair = sv_split_delim(sv_trim(args), ' ');
    String old_name = strip_quotes(sv_trim(pair.first));
    String new_name = strip_quotes(sv_trim(pair.second));

    if (old_name.length == 0 || new_name.length == 0) {
        return bufio_writeln(bio, tprintf(SV_Fmt " BAD RENAME requires two names", SV_Arg(tag)));
    }
    // Renaming from INBOX has special semantics (move messages, keep INBOX) — not supported.
    if (sv_equal_ignore_case(old_name, SV("INBOX")) || sv_equal_ignore_case(new_name, SV("INBOX"))) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO INBOX rename not supported", SV_Arg(tag)));
    }

    String old_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()), SV_Arg(session->user), SV_Arg(old_name));
    String new_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()), SV_Arg(session->user), SV_Arg(new_name));

    if (rename(old_path.data, new_path.data) != 0) {
        return bufio_writeln(bio, tprintf(SV_Fmt " NO %s", SV_Arg(tag), strerror(errno)));
    }

    if (sv_equal(session->selected_mailbox, old_name)) {
        safe_free(session->selected_mailbox.data);
        session->selected_mailbox = sv_clone(new_name);
    }
    return bufio_writeln(bio, tprintf(SV_Fmt " OK RENAME completed", SV_Arg(tag)));
}

static Error handle_search(BufIO* bio, const ImapSession* session, String tag) {
    String path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                          SV_Arg(get_maildir()),
                          SV_Arg(session->user),
                          SV_Arg(session->selected_mailbox));

    // Count messages so we can build "* SEARCH 1 2 3 ..."
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

    // Build the sequence number list into a StringBuilder
    StringBuilder sb = {0};
    for (int i = 1; i <= count; i++) {
        if (i > 1) sb_push_char(&sb, ' ');
        // tprintf uses the temp allocator; push the number directly
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d", i);
        for (int j = 0; j < len; j++) sb_push_char(&sb, buf[j]);
    }

    Error err = bufio_writeln(bio, tprintf("* SEARCH %.*s", (int)sb.length, sb.data));
    sb_free(&sb);
    if (has_error(err)) return err;
    return bufio_writeln(bio, tprintf(SV_Fmt " OK SEARCH completed", SV_Arg(tag)));
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

        INFO("Command received: " SV_Fmt, SV_Arg(line));

        if (sv_equal_ignore_case(cmd, SV("CAPABILITY"))) {
            handle_capability(&bio, tag);
        } else if (sv_equal_ignore_case(cmd, SV("NOOP"))) {
            handle_noop(&bio, tag);
        } else if (sv_equal_ignore_case(cmd, SV("LOGIN"))) {
            handle_login(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("LIST"))) {
            handle_list(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("LSUB"))) {
            handle_lsub(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("LSUB"))) {
            handle_lsub(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("SUBSCRIBE"))) {
            handle_subscribe(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("UNSUBSCRIBE"))) {
            handle_unsubscribe(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("CREATE"))) {
            handle_create(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("DELETE"))) {
            handle_delete(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("RENAME"))) {
            handle_rename(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("APPEND"))) {
            handle_append(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("SELECT"))) {
            handle_select(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("EXAMINE"))) {
            handle_select(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("STATUS"))) {
            handle_status(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("SEARCH"))) {
            handle_search(&bio, &session, tag);
        } else if (sv_equal_ignore_case(cmd, SV("FETCH"))) {
            handle_fetch(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("STORE"))) {
            handle_store(&bio, &session, tag, args);
        } else if (sv_equal_ignore_case(cmd, SV("EXPUNGE"))) {
            handle_expunge(&bio, &session, tag);
        } else if (sv_equal_ignore_case(cmd, SV("UID"))) {
            // UID sub-commands: treat UID as sequence number (no UID store yet)
            StringPair uid_cmd_args = sv_split_delim(sv_trim(args), ' ');
            String uid_cmd  = sv_trim(uid_cmd_args.first);
            String uid_args = sv_trim(uid_cmd_args.second);
            if (sv_equal_ignore_case(uid_cmd, SV("FETCH"))) {
                handle_fetch_ex(&bio, &session, tag, uid_args, true);
            } else if (sv_equal_ignore_case(uid_cmd, SV("SEARCH"))) {
                handle_search(&bio, &session, tag);
            } else if (sv_equal_ignore_case(uid_cmd, SV("STORE"))) {
                handle_store(&bio, &session, tag, uid_args);
            } else {
                bufio_writeln(&bio, tprintf(SV_Fmt " BAD UID %s not supported", SV_Arg(tag), uid_cmd.data));
            }
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