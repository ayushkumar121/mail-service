#include "maildir.h"

#include <dirent.h>
#include <string.h>

// Find ";KEY=" or ",KEY=" in filename and return the value substring.
// Stops at the next ',' or '.' or end-of-string.
static String find_kv(String filename, const char* key) {
    ssize_t colon = sv_find(filename, ";");
    if (colon < 0) return SV("");
    size_t klen = strlen(key);
    for (size_t i = (size_t)colon; i + klen + 1 < filename.length; i++) {
        if (filename.data[i] != ';' && filename.data[i] != ',') continue;
        if (memcmp(filename.data + i + 1, key, klen) != 0) continue;
        if (filename.data[i + 1 + klen] != '=') continue;
        size_t start = i + 2 + klen;
        size_t end = start;
        while (end < filename.length && filename.data[end] != ',' && filename.data[end] != '.') end++;
        return SV2(filename.data + start, end - start);
    }
    return SV("");
}

int parse_uid(String filename) {
    String v = find_kv(filename, "U");
    int uid = 0;
    for (size_t i = 0; i < v.length; i++) {
        char c = v.data[i];
        if (c < '0' || c > '9') break;
        uid = uid * 10 + (c - '0');
    }
    return uid;
}

MessageFlags parse_flags(String filename) {
    MessageFlags f = {0};
    String v = find_kv(filename, "F");
    for (size_t i = 0; i < v.length; i++) {
        switch (v.data[i]) {
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

String flags_basename(String filename) {
    ssize_t pos = sv_find(filename, ";");
    if (pos < 0) {
        if (filename.length >= 4 &&
            sv_equal(SV2(filename.data + filename.length - 4, 4), SV(".eml"))) {
            return tprintf("%.*s", (int)(filename.length - 4), filename.data);
        }
        return filename;
    }
    return tprintf("%.*s", (int)pos, filename.data);
}

String maildir_filename(String base, int uid, MessageFlags f) {
    char chars[8] = {0};
    int n = 0;
    if (f.draft)   chars[n++] = 'D';
    if (f.flagged) chars[n++] = 'F';
    if (f.replied) chars[n++] = 'R';
    if (f.seen)    chars[n++] = 'S';
    if (f.deleted) chars[n++] = 'T';
    if (n > 0) return tprintf(SV_Fmt ";U=%d,F=%s.eml", SV_Arg(base), uid, chars);
    return tprintf(SV_Fmt ";U=%d.eml", SV_Arg(base), uid);
}

String set_flag(String filename, char flag_char) {
    MessageFlags f = parse_flags(filename);
    switch (flag_char) {
        case 'D': f.draft   = true; break;
        case 'F': f.flagged = true; break;
        case 'R': f.replied = true; break;
        case 'S': f.seen    = true; break;
        case 'T': f.deleted = true; break;
        default: break;
    }
    return maildir_filename(flags_basename(filename), parse_uid(filename), f);
}

int next_uid(const char* dir_path) {
    DIR* d = opendir(dir_path);
    if (!d) return 1;
    int max = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        String name = SV2(e->d_name, strlen(e->d_name));
        int u = parse_uid(name);
        if (u > max) max = u;
    }
    closedir(d);
    return max + 1;
}

int cmp_by_uid(const void* a, const void* b) {
    int ua = parse_uid(*(const String*)a);
    int ub = parse_uid(*(const String*)b);
    return (ua > ub) - (ua < ub);
}

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
