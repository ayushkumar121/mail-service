#define LOG_PREFIX "imap: "
#include "imap.h"

#include <pthread.h>
#include <unistd.h>

#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "config.h"
#include "maildir.h"

#define IMAP_SOCKET_BACKLOG 1024
#define IMAP_DEFAULT_PORT 143
#define CRLF "\r\n"
#define IMAP_CAPABILITY "IMAP4rev1 UIDPLUS"

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

// Imap <-> Maildir
String flags_to_imap(MessageFlags f) {
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

char imap_flag_to_maildir(String imap_flag) {
    if (sv_equal_ignore_case(imap_flag, SV("\\Seen")))     return 'S';
    if (sv_equal_ignore_case(imap_flag, SV("\\Deleted")))  return 'T';
    if (sv_equal_ignore_case(imap_flag, SV("\\Flagged")))  return 'F';
    if (sv_equal_ignore_case(imap_flag, SV("\\Answered"))) return 'R';
    if (sv_equal_ignore_case(imap_flag, SV("\\Draft")))    return 'D';
    return 0;
}

static Error handle_capability(BufIO* bio, String tag) {
    bufio_write_line(bio, SV("* CAPABILITY " IMAP_CAPABILITY));
    return bufio_send_line(bio, tprintf(SV_Fmt " OK CAPABILITY completed", SV_Arg(tag)));
}

static Error handle_noop(BufIO* bio, String tag) {
    return bufio_send_line(bio, tprintf(SV_Fmt " OK NOOP completed", SV_Arg(tag)));
}

// Strip surrounding double-quotes from a string token if present
static String strip_quotes(String s) {
    if (s.length >= 2 && s.data[0] == '"' && s.data[s.length - 1] == '"') {
        return SV2(s.data + 1, s.length - 2);
    }
    return s;
}

Error handle_login(BufIO* bio, ImapSession* session, String tag, String args) {
    // TODO: real password check
    StringPair pair = sv_split_delim(args, ' ');
    session->user = sv_clone(strip_quotes(sv_trim(pair.first)));
    session->state = IMAP_STATE_AUTHENTICATED;
    return bufio_send_line(bio, tprintf(SV_Fmt " OK LOGIN completed", SV_Arg(tag)));
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
        bufio_write_line(bio, tprintf("* %s (\\Noselect) \"/\" \"\"", cmd_name));
        return bufio_send_line(bio, tprintf(SV_Fmt " OK %s completed", SV_Arg(tag), cmd_name));
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
        return bufio_send_line(bio, tprintf(SV_Fmt " OK %s completed", SV_Arg(tag), cmd_name));
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_DIR) continue;
        if (entry->d_name[0] == '.') continue;  // skip . and ..

        String name = SV2(entry->d_name, strlen(entry->d_name));
        if (fnmatch(pat_cstr, sv_to_tmp_c(name), FNM_CASEFOLD) != 0) continue;

        bufio_write_line(bio,
            tprintf("* %s () \"/\" \"" SV_Fmt "\"", cmd_name, SV_Arg(name)));
    }
    closedir(dir);
    return bufio_send_line(bio, tprintf(SV_Fmt " OK %s completed", SV_Arg(tag), cmd_name));
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

    bufio_write_line(bio, SV("* FLAGS (\\Seen \\Answered \\Flagged \\Deleted \\Draft)"));
    bufio_write_line(bio, tprintf("* %d EXISTS", count));
    bufio_write_line(bio, tprintf("* %d RECENT", unseen));
    bufio_write_line(bio, SV("* OK [UIDVALIDITY 1] UIDs valid"));
    bufio_write_line(bio, tprintf("* OK [UIDNEXT %d] Predicted next UID", count + 1));
    bufio_write_line(bio, SV("* OK [PERMANENTFLAGS (\\Seen \\Answered \\Flagged \\Deleted \\Draft)] Limited"));
    return bufio_send_line(bio, tprintf(SV_Fmt " OK [READ-WRITE] SELECT completed", SV_Arg(tag)));
}

static void handle_subscribe(BufIO* bio, ImapSession* session, String tag, String args) {
    bufio_send_line(bio, tprintf(SV_Fmt " OK SUBSCRIBE completed", SV_Arg(tag)));
}

static void handle_unsubscribe(BufIO* bio, ImapSession* session, String tag, String args) {
    bufio_send_line(bio, tprintf(SV_Fmt " OK UNSUBSCRIBE completed", SV_Arg(tag)));
}
typedef struct {
    bool flags;
    bool uid;
    bool body;
    bool peek;
    bool size;
    bool internaldate;
    String section;  // raw spec inside BODY[...] — e.g. "" for BODY[], "HEADER.FIELDS (From To)" otherwise
} FetchItems;

// Returns the headers portion of an RFC 822 message including the trailing blank-line CRLF.
static String extract_headers(String body) {
    ssize_t end = sv_find(body, "\r\n\r\n");
    if (end >= 0) return SV2(body.data, (size_t)end + 4);
    end = sv_find(body, "\n\n");
    if (end >= 0) return SV2(body.data, (size_t)end + 2);
    return body;
}

static String extract_text(String body) {
    ssize_t end = sv_find(body, "\r\n\r\n");
    if (end >= 0) return SV2(body.data + end + 4, body.length - (size_t)end - 4);
    end = sv_find(body, "\n\n");
    if (end >= 0) return SV2(body.data + end + 2, body.length - (size_t)end - 2);
    return SV("");
}

static bool header_name_in_list(String name, String list) {
    size_t i = 0;
    while (i < list.length) {
        while (i < list.length && (list.data[i] == ' ' || list.data[i] == '\t')) i++;
        size_t s = i;
        while (i < list.length && list.data[i] != ' ' && list.data[i] != '\t') i++;
        String tok = SV2(list.data + s, i - s);
        if (tok.length > 0 && sv_equal_ignore_case(tok, name)) return true;
    }
    return false;
}

// Filter headers to those whose names appear in fields_list (which may be wrapped in parens).
// Returns a tprintf-allocated buffer.
static String filter_header_fields(String body, String fields_list) {
    fields_list = sv_trim(fields_list);
    if (fields_list.length >= 2 && fields_list.data[0] == '(' && fields_list.data[fields_list.length-1] == ')') {
        fields_list = sv_trim(SV2(fields_list.data + 1, fields_list.length - 2));
    }
    String headers = extract_headers(body);
    StringBuilder out = {0};
    size_t i = 0;
    bool keep_cont = false;
    while (i < headers.length) {
        size_t ls = i;
        while (i < headers.length && headers.data[i] != '\n') i++;
        if (i < headers.length) i++;
        String line = SV2(headers.data + ls, i - ls);
        if (line.length == 0) break;
        bool blank = (line.length == 2 && line.data[0] == '\r' && line.data[1] == '\n')
                  || (line.length == 1 && line.data[0] == '\n');
        if (blank) break;
        bool cont = (line.data[0] == ' ' || line.data[0] == '\t');
        if (cont) {
            if (keep_cont) sb_push_sv(&out, line);
            continue;
        }
        ssize_t colon = sv_find(line, ":");
        if (colon < 0) { keep_cont = false; continue; }
        String name = sv_trim(SV2(line.data, (size_t)colon));
        if (header_name_in_list(name, fields_list)) {
            sb_push_sv(&out, line);
            keep_cont = true;
        } else {
            keep_cont = false;
        }
    }
    sb_push_sv(&out, SV("\r\n"));
    String result = tprintf("%.*s", (int)out.length, out.data);
    sb_free(&out);
    return result;
}

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
            ssize_t rb = sv_find(tok, "]");
            fi.section = rb > 5 ? SV2(tok.data + 5, (size_t)rb - 5) : SV("");
        }
        else if (tok.length >= 10 && sv_equal_ignore_case(SV2(tok.data, 10), SV("BODY.PEEK["))) {
            fi.body = true; fi.peek = true;
            ssize_t rb = sv_find(tok, "]");
            fi.section = rb > 10 ? SV2(tok.data + 10, (size_t)rb - 10) : SV("");
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
        return bufio_send_line(bio, tprintf(SV_Fmt " NO mailbox not found", SV_Arg(tag)));
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
    qsort(names.data, names.length, sizeof(String), cmp_by_uid);

    int count = (int)names.length;
    int max_uid = count > 0 ? parse_uid(names.data[count - 1]) : 0;

    SeqRangeArray ranges = is_uid
        ? parse_seq_set(seq_set, max_uid)
        : parse_seq_set(seq_set, count);
    FetchItems fi = parse_fetch_items(items_str, is_uid);

    Error err = ErrorNil;
    for (int seq = 1; seq <= count; seq++) {
        int msg_uid = parse_uid(names.data[seq - 1]);
        bool match = is_uid ? seq_in_ranges(&ranges, msg_uid) : seq_in_ranges(&ranges, seq);
        if (!match) continue;

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
            sb_push_sv(&line, tprintf("UID %d", msg_uid));
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
                err = bufio_send_line(bio, tprintf(SV_Fmt " NO failed to read message", SV_Arg(tag)));
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
            // Compute the data slice corresponding to fi.section
            String full = sb_to_sv(&body_sb);
            String body_data = full;
            String sec = fi.section;
            if (sec.length == 0) {
                body_data = full;
            } else if (sv_equal_ignore_case(sec, SV("HEADER"))) {
                body_data = extract_headers(full);
            } else if (sv_equal_ignore_case(sec, SV("TEXT"))) {
                body_data = extract_text(full);
            } else if (sec.length >= 13 &&
                       sv_equal_ignore_case(SV2(sec.data, 13), SV("HEADER.FIELDS"))) {
                String fields = sv_trim(SV2(sec.data + 13, sec.length - 13));
                // HEADER.FIELDS.NOT not supported — fall back to filter as include list
                if (fields.length > 4 && sv_equal_ignore_case(SV2(fields.data, 4), SV(".NOT"))) {
                    fields = sv_trim(SV2(fields.data + 4, fields.length - 4));
                }
                body_data = filter_header_fields(full, fields);
            }

            if (!first) sb_push_char(&line, ' ');
            sb_push_sv(&line, tprintf("BODY[" SV_Fmt "] {%zu}",
                                      SV_Arg(sec), body_data.length));
            sb_push_sv(&line, SV("\r\n"));
            bufio_write(bio, sb_to_sv(&line));
            sb_free(&line);
            bufio_write(bio, body_data);
            sb_free(&body_sb);
            bufio_write_line(bio, SV(")"));
            continue;
        }

        if (has_body) sb_free(&body_sb);
        sb_push_char(&line, ')');
        bufio_write_line(bio, sb_to_sv(&line));
        sb_free(&line);
    }

    err = bufio_send_line(bio, tprintf(SV_Fmt " OK %s completed",
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

// CLOSE: silently expunge \Deleted messages and unselect the mailbox.
// Unlike EXPUNGE, no untagged * EXPUNGE responses are sent.
static Error handle_close(BufIO* bio, ImapSession* session, String tag) {
    if (session->selected_mailbox.length > 0) {
        String dir_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                                  SV_Arg(get_maildir()),
                                  SV_Arg(session->user),
                                  SV_Arg(session->selected_mailbox));
        DIR* dir = opendir(dir_path.data);
        if (dir != NULL) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != NULL) {
                String name = SV2(entry->d_name, strlen(entry->d_name));
                if (sv_find(name, ".eml") == -1) continue;
                if (parse_flags(name).deleted) {
                    String full = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(dir_path), SV_Arg(name));
                    INFO("CLOSE expunging " SV_Fmt, SV_Arg(full));
                    remove(full.data);
                }
            }
            closedir(dir);
        }
    }
    safe_free(session->selected_mailbox.data);
    session->selected_mailbox = StringNil;
    session->state = IMAP_STATE_AUTHENTICATED;
    return bufio_send_line(bio, tprintf(SV_Fmt " OK CLOSE completed", SV_Arg(tag)));
}

static Error handle_status(BufIO* bio, const ImapSession* session, String tag, String args) {
    // args: e.g. `"Test" (UIDNEXT MESSAGES UNSEEN RECENT)`
    args = sv_trim(args);
    String mailbox;
    String items;
    if (args.length > 0 && args.data[0] == '"') {
        ssize_t eq = -1;
        for (size_t i = 1; i < args.length; i++) {
            if (args.data[i] == '"') { eq = (ssize_t)i; break; }
        }
        if (eq < 0) return bufio_send_line(bio, tprintf(SV_Fmt " BAD unterminated mailbox", SV_Arg(tag)));
        mailbox = SV2(args.data + 1, (size_t)eq - 1);
        items = sv_trim(SV2(args.data + eq + 1, args.length - (size_t)eq - 1));
    } else {
        StringPair pair = sv_split_delim(args, ' ');
        mailbox = sv_trim(pair.first);
        items = sv_trim(pair.second);
    }
    if (items.length >= 2 && items.data[0] == '(' && items.data[items.length-1] == ')') {
        items = SV2(items.data + 1, items.length - 2);
    }

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

    StringBuilder out = {0};
    sb_push_sv(&out, tprintf("* STATUS \"" SV_Fmt "\" (", SV_Arg(mailbox)));
    bool first = true;
    size_t i = 0;
    while (i < items.length) {
        while (i < items.length && (items.data[i] == ' ' || items.data[i] == '\t')) i++;
        size_t s = i;
        while (i < items.length && items.data[i] != ' ' && items.data[i] != '\t') i++;
        String tok = SV2(items.data + s, i - s);
        if (tok.length == 0) continue;
        if (!first) sb_push_char(&out, ' ');
        if (sv_equal_ignore_case(tok, SV("MESSAGES")))         sb_push_sv(&out, tprintf("MESSAGES %d", total));
        else if (sv_equal_ignore_case(tok, SV("RECENT")))      sb_push_sv(&out, tprintf("RECENT %d", unseen));
        else if (sv_equal_ignore_case(tok, SV("UNSEEN")))      sb_push_sv(&out, tprintf("UNSEEN %d", unseen));
        else if (sv_equal_ignore_case(tok, SV("UIDNEXT")))     sb_push_sv(&out, tprintf("UIDNEXT %d", total + 1));
        else if (sv_equal_ignore_case(tok, SV("UIDVALIDITY"))) sb_push_sv(&out, SV("UIDVALIDITY 1"));
        else { sb_push_sv(&out, tok); sb_push_sv(&out, SV(" 0")); }
        first = false;
    }
    sb_push_char(&out, ')');

    bufio_write_line(bio, sb_to_sv(&out));
    sb_free(&out);
    return bufio_send_line(bio, tprintf(SV_Fmt " OK STATUS completed", SV_Arg(tag)));
}

static Error handle_store_ex(BufIO* bio, const ImapSession* session, String tag, String args, bool is_uid) {
    // args: "seq <op>FLAGS (\Seen \Deleted ...)" where op is +/-/empty
    StringPair seq_rest = sv_split_delim(sv_trim(args), ' ');
    char* endptr = NULL;
    int seq = sv_to_int(sv_trim(seq_rest.first), &endptr);

    StringPair op_flags = sv_split_delim(sv_trim(seq_rest.second), ' ');
    String op = sv_trim(op_flags.first);  // "+FLAGS", "-FLAGS", "FLAGS" (replace)
    String flags_str = sv_trim(op_flags.second);

    bool remove_op = (op.length > 0 && op.data[0] == '-');
    bool replace_op = (op.length > 0 && op.data[0] != '+' && op.data[0] != '-');

    if (flags_str.length >= 2 && flags_str.data[0] == '(' &&
        flags_str.data[flags_str.length - 1] == ')') {
        flags_str = SV2(flags_str.data + 1, flags_str.length - 2);
    }

    String dir_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()),
                              SV_Arg(session->user),
                              SV_Arg(session->selected_mailbox));

    DIR* dir = opendir(dir_path.data);
    if (dir == NULL) {
        return bufio_send_line(bio, tprintf(SV_Fmt " NO mailbox not found", SV_Arg(tag)));
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
    qsort(names.data, names.length, sizeof(String), cmp_by_uid);

    // For UID STORE, `seq` is actually a UID; locate the matching message.
    // For plain STORE, `seq` is a sequence number into the sorted list.
    String old_name = StringNil;
    int seq_pos = 0;
    for (size_t i = 0; i < names.length; i++) {
        bool match = is_uid ? (parse_uid(names.data[i]) == seq) : ((int)i + 1 == seq);
        if (match) {
            old_name = sv_clone(names.data[i]);
            seq_pos = (int)i + 1;
            break;
        }
    }
    for (size_t i = 0; i < names.length; i++) safe_free(names.data[i].data);
    array_free(&names);

    if (old_name.length == 0) {
        return bufio_send_line(bio, tprintf(SV_Fmt " NO message not found", SV_Arg(tag)));
    }

    // Build target flag set
    MessageFlags target = replace_op ? (MessageFlags){0} : parse_flags(old_name);
    while (flags_str.length > 0) {
        StringPair fp = sv_split_delim(sv_trim(flags_str), ' ');
        String flag = sv_trim(fp.first);
        if (flag.length == 0) break;
        char c = imap_flag_to_maildir(flag);
        bool on = !remove_op;
        switch (c) {
            case 'S': target.seen    = on; break;
            case 'T': target.deleted = on; break;
            case 'F': target.flagged = on; break;
            case 'R': target.replied = on; break;
            case 'D': target.draft   = on; break;
            default: break;
        }
        flags_str = fp.second;
    }

    String new_name = maildir_filename(flags_basename(old_name), parse_uid(old_name), target);

    String old_path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(dir_path), SV_Arg(old_name));
    String new_path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(dir_path), SV_Arg(new_name));

    if (!sv_equal(old_name, new_name)) {
        rename(old_path.data, new_path.data);
    }
    safe_free(old_name.data);

    MessageFlags f = parse_flags(new_name);
    String imap_flags = flags_to_imap(f);

    if (is_uid) {
        bufio_write_line(bio, tprintf("* %d FETCH (UID %d FLAGS %s)",
                                      seq_pos, parse_uid(new_name), imap_flags.data));
    } else {
        bufio_write_line(bio, tprintf("* %d FETCH (FLAGS %s)", seq_pos, imap_flags.data));
    }
    return bufio_send_line(bio, tprintf(SV_Fmt " OK STORE completed", SV_Arg(tag)));
}

static Error handle_store(BufIO* bio, const ImapSession* session, String tag, String args) {
    return handle_store_ex(bio, session, tag, args, false);
}

// uid_set is non-empty for UID EXPUNGE (RFC 4315): only expunge \Deleted msgs
// whose UID is in the set. NULL/empty means full EXPUNGE.
static Error expunge_impl(BufIO* bio, const ImapSession* session, String tag, String uid_set, bool is_uid_expunge) {
    String dir_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()),
                              SV_Arg(session->user),
                              SV_Arg(session->selected_mailbox));

    DIR* dir = opendir(dir_path.data);
    if (dir == NULL) {
        return bufio_send_line(bio, tprintf(SV_Fmt " NO mailbox not found", SV_Arg(tag)));
    }

    typedef ARRAY(String) StringArray;
    StringArray files = {0};
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        String name = SV2(entry->d_name, strlen(entry->d_name));
        if (sv_find(name, ".eml") == -1) continue;
        array_append(&files, sv_clone(name));
    }
    closedir(dir);
    qsort(files.data, files.length, sizeof(String), cmp_by_uid);

    int max_uid = files.length > 0 ? parse_uid(files.data[files.length - 1]) : 0;
    SeqRangeArray ranges = {0};
    if (is_uid_expunge) ranges = parse_seq_set(uid_set, max_uid);

    int seq = 1;
    for (size_t i = 0; i < files.length; i++) {
        MessageFlags f = parse_flags(files.data[i]);
        bool in_uid_set = is_uid_expunge
            ? seq_in_ranges(&ranges, parse_uid(files.data[i]))
            : true;
        if (f.deleted && in_uid_set) {
            String full_path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(dir_path), SV_Arg(files.data[i]));
            INFO("%s expunging seq=%d uid=%d " SV_Fmt,
                 is_uid_expunge ? "UID EXPUNGE" : "EXPUNGE",
                 seq, parse_uid(files.data[i]), SV_Arg(full_path));
            remove(full_path.data);
            bufio_write_line(bio, tprintf("* %d EXPUNGE", seq));
            // do not increment seq — sequence numbers shift down after each expunge
        } else {
            seq++;
        }
        safe_free(files.data[i].data);
    }
    array_free(&files);
    array_free(&ranges);

    return bufio_send_line(bio, tprintf(SV_Fmt " OK %s completed",
                                        SV_Arg(tag),
                                        is_uid_expunge ? "UID EXPUNGE" : "EXPUNGE"));
}

static Error handle_expunge(BufIO* bio, const ImapSession* session, String tag) {
    return expunge_impl(bio, session, tag, SV(""), false);
}

static Error handle_uid_expunge(BufIO* bio, const ImapSession* session, String tag, String args) {
    return expunge_impl(bio, session, tag, sv_trim(args), true);
}

static Error handle_append(BufIO* bio, const ImapSession* session, String tag_in, String args) {
    // tag_in and args point into bio->read_buf, which bufio_read_n will clobber.
    // Snapshot everything we need from the request line before reading the literal.
    String tag = tprintf(SV_Fmt, SV_Arg(tag_in));
    args = tprintf(SV_Fmt, SV_Arg(sv_trim(args)));

    ssize_t lbrace = sv_find(args, "{");
    ssize_t rbrace = sv_find(args, "}");
    if (lbrace < 0 || rbrace < 0 || rbrace <= lbrace) {
        return bufio_send_line(bio, tprintf(SV_Fmt " BAD APPEND requires literal {N}", SV_Arg(tag)));
    }

    String size_str = SV2(args.data + lbrace + 1, (size_t)(rbrace - lbrace - 1));
    char* endptr = NULL;
    int size = sv_to_int(sv_trim(size_str), &endptr);
    if (size < 0) {
        return bufio_send_line(bio, tprintf(SV_Fmt " BAD invalid literal size", SV_Arg(tag)));
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
            return bufio_send_line(bio, tprintf(SV_Fmt " BAD unterminated quoted name", SV_Arg(tag)));
        }
        mailbox = SV2(head.data + 1, (size_t)(end_quote - 1));
        rest = sv_trim(SV2(head.data + end_quote + 1, head.length - (size_t)end_quote - 1));
    } else {
        StringPair p = sv_split_delim(head, ' ');
        mailbox = sv_trim(p.first);
        rest = sv_trim(p.second);
    }

    if (mailbox.length == 0) {
        return bufio_send_line(bio, tprintf(SV_Fmt " BAD mailbox name required", SV_Arg(tag)));
    }

    // Optional flag list "(\Seen \Draft ...)" — anything else (date-time) is ignored.
    MessageFlags flags = {0};
    if (rest.length > 0 && rest.data[0] == '(') {
        ssize_t end_paren = sv_find(rest, ")");
        if (end_paren < 0) {
            return bufio_send_line(bio, tprintf(SV_Fmt " BAD unterminated flag list", SV_Arg(tag)));
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
        // Auto-create — Thunderbird does not always retry on [TRYCREATE]
        String user_dir = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(get_maildir()), SV_Arg(session->user));
        mkdir(user_dir.data, 0700);
        if (mkdir(dir_path.data, 0700) != 0 && errno != EEXIST) {
            return bufio_send_line(bio, tprintf(SV_Fmt " NO failed to create mailbox: %s",
                                              SV_Arg(tag), strerror(errno)));
        }
    }

    Error err = bufio_send_line(bio, SV("+ Ready for literal data"));
    if (has_error(err)) return err;

    err = bufio_read_n(bio, (size_t)size);
    if (has_error(err)) return err;
    String body = sv_clone(sb_to_sv(&bio->read_buf));
    err = bufio_read_until(bio, CRLF);
    if (has_error(err)) { safe_free(body.data); return err; }

    String host = get_hostname();
    int uid = next_uid(dir_path.data);
    String base = tprintf(SV_Fmt "/%ld." SV_Fmt "." SV_Fmt,
                          SV_Arg(dir_path), (long)time(NULL),
                          SV_Arg(random_id(RANDOM_ID_LEN)), SV_Arg(host));
    String filename = maildir_filename(base, uid, flags);

    err = write_entire_file(filename.data, body);
    if (has_error(err)) {
        safe_free(body.data);
        return bufio_send_line(bio, tprintf(SV_Fmt " NO failed to write message", SV_Arg(tag)));
    }
    INFO("APPEND saved " SV_Fmt " (%zu bytes) uid=%d",
         SV_Arg(filename), body.length, uid);
    safe_free(body.data);
    return bufio_send_line(bio, tprintf(SV_Fmt " OK [APPENDUID 1 %d] APPEND completed",
                                        SV_Arg(tag), uid));
}

static Error handle_create(BufIO* bio, const ImapSession* session, String tag, String args) {
    String name = strip_quotes(sv_trim(args));
    if (name.length == 0) {
        return bufio_send_line(bio, tprintf(SV_Fmt " BAD mailbox name required", SV_Arg(tag)));
    }
    if (sv_equal_ignore_case(name, SV("INBOX"))) {
        return bufio_send_line(bio, tprintf(SV_Fmt " NO cannot create INBOX", SV_Arg(tag)));
    }

    String user_dir = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(get_maildir()), SV_Arg(session->user));
    mkdir(user_dir.data, 0700);

    String path = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(user_dir), SV_Arg(name));
    if (mkdir(path.data, 0700) != 0) {
        if (errno == EEXIST) {
            return bufio_send_line(bio, tprintf(SV_Fmt " NO mailbox already exists", SV_Arg(tag)));
        }
        return bufio_send_line(bio, tprintf(SV_Fmt " NO %s", SV_Arg(tag), strerror(errno)));
    }
    return bufio_send_line(bio, tprintf(SV_Fmt " OK CREATE completed", SV_Arg(tag)));
}

static Error handle_delete(BufIO* bio, ImapSession* session, String tag, String args) {
    String name = strip_quotes(sv_trim(args));
    if (name.length == 0) {
        return bufio_send_line(bio, tprintf(SV_Fmt " BAD mailbox name required", SV_Arg(tag)));
    }
    if (sv_equal_ignore_case(name, SV("INBOX"))) {
        return bufio_send_line(bio, tprintf(SV_Fmt " NO cannot delete INBOX", SV_Arg(tag)));
    }

    String path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                          SV_Arg(get_maildir()), SV_Arg(session->user), SV_Arg(name));

    DIR* dir = opendir(path.data);
    if (dir == NULL) {
        return bufio_send_line(bio, tprintf(SV_Fmt " NO mailbox does not exist", SV_Arg(tag)));
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
        return bufio_send_line(bio, tprintf(SV_Fmt " NO %s", SV_Arg(tag), strerror(errno)));
    }

    if (sv_equal(session->selected_mailbox, name)) {
        safe_free(session->selected_mailbox.data);
        session->selected_mailbox = StringNil;
        session->state = IMAP_STATE_AUTHENTICATED;
    }
    return bufio_send_line(bio, tprintf(SV_Fmt " OK DELETE completed", SV_Arg(tag)));
}

static Error handle_rename(BufIO* bio, ImapSession* session, String tag, String args) {
    StringPair pair = sv_split_delim(sv_trim(args), ' ');
    String old_name = strip_quotes(sv_trim(pair.first));
    String new_name = strip_quotes(sv_trim(pair.second));

    if (old_name.length == 0 || new_name.length == 0) {
        return bufio_send_line(bio, tprintf(SV_Fmt " BAD RENAME requires two names", SV_Arg(tag)));
    }
    // Renaming from INBOX has special semantics (move messages, keep INBOX) — not supported.
    if (sv_equal_ignore_case(old_name, SV("INBOX")) || sv_equal_ignore_case(new_name, SV("INBOX"))) {
        return bufio_send_line(bio, tprintf(SV_Fmt " NO INBOX rename not supported", SV_Arg(tag)));
    }

    String old_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()), SV_Arg(session->user), SV_Arg(old_name));
    String new_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()), SV_Arg(session->user), SV_Arg(new_name));

    if (rename(old_path.data, new_path.data) != 0) {
        return bufio_send_line(bio, tprintf(SV_Fmt " NO %s", SV_Arg(tag), strerror(errno)));
    }

    if (sv_equal(session->selected_mailbox, old_name)) {
        safe_free(session->selected_mailbox.data);
        session->selected_mailbox = sv_clone(new_name);
    }
    return bufio_send_line(bio, tprintf(SV_Fmt " OK RENAME completed", SV_Arg(tag)));
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

    bufio_write_line(bio, tprintf("* SEARCH %.*s", (int)sb.length, sb.data));
    sb_free(&sb);
    return bufio_send_line(bio, tprintf(SV_Fmt " OK SEARCH completed", SV_Arg(tag)));
}

static Error handle_copy_ex(BufIO* bio, const ImapSession* session, String tag, String args, bool is_uid) {
    StringPair pair = sv_split_delim(sv_trim(args), ' ');
    String seq_set = sv_trim(pair.first);
    String dest = strip_quotes(sv_trim(pair.second));

    if (seq_set.length == 0 || dest.length == 0) {
        return bufio_send_line(bio, tprintf(SV_Fmt " BAD COPY requires seq-set and mailbox", SV_Arg(tag)));
    }
    if (session->selected_mailbox.length == 0) {
        return bufio_send_line(bio, tprintf(SV_Fmt " BAD no mailbox selected", SV_Arg(tag)));
    }

    String src_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()), SV_Arg(session->user),
                              SV_Arg(session->selected_mailbox));
    String dst_path = tprintf(SV_Fmt "/" SV_Fmt "/" SV_Fmt,
                              SV_Arg(get_maildir()), SV_Arg(session->user), SV_Arg(dest));

    // Auto-create destination if missing (matches APPEND behavior).
    if (!file_exists(dst_path.data)) {
        if (mkdir(dst_path.data, 0700) != 0 && errno != EEXIST) {
            return bufio_send_line(bio, tprintf(SV_Fmt " NO [TRYCREATE] %s",
                                                SV_Arg(tag), strerror(errno)));
        }
    }

    DIR* dir = opendir(src_path.data);
    if (dir == NULL) {
        return bufio_send_line(bio, tprintf(SV_Fmt " NO source mailbox not found", SV_Arg(tag)));
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
    qsort(names.data, names.length, sizeof(String), cmp_by_uid);

    int count = (int)names.length;
    int max_uid = count > 0 ? parse_uid(names.data[count - 1]) : 0;
    SeqRangeArray ranges = is_uid
        ? parse_seq_set(seq_set, max_uid)
        : parse_seq_set(seq_set, count);
    String host = get_hostname();

    StringBuilder src_set = {0};
    StringBuilder dst_set = {0};

    Error err = ErrorNil;
    for (int seq = 1; seq <= count; seq++) {
        int src_uid = parse_uid(names.data[seq - 1]);
        bool match = is_uid ? seq_in_ranges(&ranges, src_uid) : seq_in_ranges(&ranges, seq);
        if (!match) continue;

        String src_name = names.data[seq - 1];
        String src_full = tprintf(SV_Fmt "/" SV_Fmt, SV_Arg(src_path), SV_Arg(src_name));

        StringBuilder body = {0};
        err = read_entire_file(src_full.data, &body);
        if (has_error(err)) break;

        // Preserve flags from source; allocate fresh UID in destination.
        MessageFlags f = parse_flags(src_name);
        int dst_uid = next_uid(dst_path.data);
        String base = tprintf(SV_Fmt "/%ld." SV_Fmt "." SV_Fmt,
                              SV_Arg(dst_path), (long)time(NULL),
                              SV_Arg(random_id(RANDOM_ID_LEN)), SV_Arg(host));
        String dst_full = maildir_filename(base, dst_uid, f);

        err = write_entire_file(dst_full.data, sb_to_sv(&body));
        if (has_error(err)) { sb_free(&body); break; }
        INFO("COPY saved " SV_Fmt " (%zu bytes) src_uid=%d dst_uid=%d",
             SV_Arg(dst_full), body.length, src_uid, dst_uid);
        sb_free(&body);

        if (src_set.length > 0) sb_push_char(&src_set, ',');
        sb_push_sv(&src_set, tprintf("%d", src_uid));
        if (dst_set.length > 0) sb_push_char(&dst_set, ',');
        sb_push_sv(&dst_set, tprintf("%d", dst_uid));
    }

    for (size_t i = 0; i < names.length; i++) safe_free(names.data[i].data);
    array_free(&names);
    array_free(&ranges);

    if (has_error(err)) {
        sb_free(&src_set); sb_free(&dst_set);
        return bufio_send_line(bio, tprintf(SV_Fmt " NO copy failed: " SV_Fmt,
                                            SV_Arg(tag), SV_Arg(err.message)));
    }

    Error res = src_set.length > 0
        ? bufio_send_line(bio, tprintf(SV_Fmt " OK [COPYUID 1 %.*s %.*s] %s completed",
                                       SV_Arg(tag),
                                       (int)src_set.length, src_set.data,
                                       (int)dst_set.length, dst_set.data,
                                       is_uid ? "UID COPY" : "COPY"))
        : bufio_send_line(bio, tprintf(SV_Fmt " OK %s completed",
                                       SV_Arg(tag), is_uid ? "UID COPY" : "COPY"));
    sb_free(&src_set);
    sb_free(&dst_set);
    return res;
}

static Error handle_copy(BufIO* bio, const ImapSession* session, String tag, String args) {
    return handle_copy_ex(bio, session, tag, args, false);
}

static void* handle_client(void* p) {
    int client_fd = (int)(intptr_t)p;
    BufIO bio = {.fd = client_fd};
    ImapSession session = {.state = IMAP_STATE_NOT_AUTHENTICATED};

    bufio_send_line(&bio, SV("* OK [CAPABILITY " IMAP_CAPABILITY "] IMAP server ready"));

    while (session.state != IMAP_STATE_LOGOUT) {
        Error err = bufio_read_until(&bio, CRLF);
        if (has_error(err)) {
            ERROR("read failed: " SV_Fmt, SV_Arg(err.message));
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
        } else if (sv_equal_ignore_case(cmd, SV("CLOSE"))) {
            handle_close(&bio, &session, tag);
        } else if (sv_equal_ignore_case(cmd, SV("COPY"))) {
            handle_copy(&bio, &session, tag, args);
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
                handle_store_ex(&bio, &session, tag, uid_args, true);
            } else if (sv_equal_ignore_case(uid_cmd, SV("COPY"))) {
                handle_copy_ex(&bio, &session, tag, uid_args, true);
            } else if (sv_equal_ignore_case(uid_cmd, SV("EXPUNGE"))) {
                handle_uid_expunge(&bio, &session, tag, uid_args);
            } else {
                bufio_send_line(&bio, tprintf(SV_Fmt " BAD UID %s not supported", SV_Arg(tag), uid_cmd.data));
            }
        } else if (sv_equal_ignore_case(cmd, SV("LOGOUT"))) {
            bufio_write_line(&bio, SV("* BYE logging out"));
            bufio_send_line(&bio, tprintf(SV_Fmt " OK LOGOUT completed", SV_Arg(tag)));
            session.state = IMAP_STATE_LOGOUT;
        } else {
            bufio_send_line(&bio, tprintf(SV_Fmt " BAD command not recognized", SV_Arg(tag)));
        }

        treset();
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

    INFO("server started");
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