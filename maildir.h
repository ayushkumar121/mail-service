#ifndef MAILDIR_H
#define MAILDIR_H

#include "basic.h"

// Maildir filename layout (this server's convention):
//   <unique>;U=<uid>[,F=<flag-chars>].eml
// `;` (not `:`) is used as the section marker for cross-platform safety —
// `:` is forbidden on Windows/SMB. Compare to standard maildir which uses `:`.
// Examples:
//   1700000000.abcdef.host;U=42.eml          (no flags)
//   1700000000.abcdef.host;U=42,F=SR.eml     (Seen + Replied)
// Flag letters: D=Draft F=Flagged R=Replied S=Seen T=Trashed (\Deleted)

typedef struct {
    bool seen;
    bool deleted;
    bool flagged;
    bool replied;
    bool draft;
} MessageFlags;

// Filename parsing
int          parse_uid(String filename);
MessageFlags parse_flags(String filename);
String       flags_basename(String filename);   // unique-name portion, no ".eml"

// Filename construction (tprintf-allocated)
String maildir_filename(String base, int uid, MessageFlags f);
String set_flag(String filename, char flag_char);  // toggles one flag, preserves UID

// UID allocation — scans the directory for the current max U=N. O(n).
int next_uid(const char* dir_path);

// qsort comparator: ascending UID order
int cmp_by_uid(const void* a, const void* b);

#endif
