#include "./basic.h"
#include "./config.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

// Globals
_Thread_local size_t temp_allocated = 0;
_Thread_local uint8_t temp_buffer[TEMP_BUFFER_CAP];

size_t align(size_t size) {
    if (size % 8 == 0)
        return size;
    return size + (8 - size % 8);
}

void *talloc(size_t n) {
    size_t size = align(n);
    assert(size <= TEMP_BUFFER_CAP);

    if (temp_allocated + size >= TEMP_BUFFER_CAP) {
        temp_allocated = 0;
    }

    void* ptr = &temp_buffer[temp_allocated];
    temp_allocated += size;
    return ptr;
}

void treset() { temp_allocated = 0; }

// Error Handling

Error error(char* message) { return error_sv(SV(message)); }
Error error_sv(String message) { return (Error){.message = message}; }

Error errorf(const char* format, ...) {
    va_list args;

    va_start(args, format);
    String str = tvprintf(format, args);
    va_end(args);

    return error_sv(str);
}

bool has_error(Error err) { return err.message.length > 0; }

void try_(Error err, char* file, int line) {
    if (has_error(err)) {
        ERROR("%s:%d: thread panicked: " SV_Fmt "\n", file, line,
              SV_Arg(err.message));
        exit(1);
    }
}

/* String Builder */

void sb_resize(StringBuilder* sb, size_t new_capacity) {
    sb->capacity = new_capacity;
    void* ptr = realloc(sb->data, sb->capacity + 1);
    assert(ptr != NULL);
    sb->data = ptr;
}

void sb_free(const StringBuilder* sb) { array_free(sb); }

String sb_to_sv(const StringBuilder* sb) {
    String sv = {0};
    sv.data = sb->data;
    sv.length = sb->length;
    return sv;
}

void sb_push_str(StringBuilder* sb, const char* str) {
    size_t item_len = strlen(str);
    if (sb->capacity < (sb->length + item_len + 1)) {
        sb_resize(sb, sb->capacity + item_len);
    }

    memcpy(sb->data + sb->length, str, item_len);
    sb->length += item_len;
    sb->data[sb->length] = 0;
}

void sb_push_sv(StringBuilder* sb, String sv) {
    if (sb->capacity < (sb->length + sv.length + 1)) {
        sb_resize(sb, sb->capacity + sv.length);
    }

    memcpy(sb->data + sb->length, sv.data, sv.length);
    sb->length += sv.length;
    sb->data[sb->length] = 0;
}

void sb_push_char(StringBuilder* sb, char ch) {
    if (sb->capacity < (sb->length + 2)) {
        sb_resize(sb, sb->capacity + 1);
    }

    sb->data[sb->length] = ch;
    sb->length += 1;
    sb->data[sb->length] = 0;
}

void sb_push_long(StringBuilder* sb, long l) {
    if (l == 0) {
        sb_push_char(sb, '0');
        return;
    }

    const bool neg = l < 0;
    if (neg)
        l *= -1;

    const long k = (long) ((neg) ? log10((double) l) + 2 : log10((double) l) + 1);
    sb_resize(sb, sb->capacity + k);

    long j = k;
    while (l != 0) {
        sb->data[sb->length + j - 1] = (char) ((l % 10) + '0');
        l = l / 10;
        j -= 1;
    }

    if (neg)
        sb->data[sb->length] = '-';
    sb->length += k;
    sb->data[sb->length] = 0;
}

void sb_push_double(StringBuilder* sb, double d) {
    // Handling integral part
    const long l = (long) ((d < 0) ? ceil(d) : floor(d));
    sb_push_long(sb, l);

    // Handling fractional part
    long f = ((long) d - l) * 1000000;
    if (f < 0)
        f *= -1;
    if (f > 0) {
        sb_push_char(sb, '.');
        sb_push_long(sb, f);
    }
}

void sb_push_float(StringBuilder* sb, const float f) {
    sb_push_double(sb, f);
}

StringBuilder sb_clone(const StringBuilder* sb) {
    StringBuilder clone = {0};
    sb_push_str(&clone, sb->data);
    return clone;
}

/* String View */

bool sv_equal(String s1, String s2) {
    if (s1.length != s2.length)
        return false;

    for (size_t i = 0; i < s1.length; i++) {
        if (s1.data[i] != s2.data[i])
            return false;
    }

    return true;
}

bool sv_equal_ignore_case(String s1, String s2) {
    if (s1.length != s2.length)
        return false;

    for (size_t i = 0; i < s1.length; i++) {
        if (tolower(s1.data[i]) != tolower(s2.data[i]))
            return false;
    }

    return true;
}

String sv_trim_left(String sv) {
    String result = sv;
    while (result.length > 0 && isspace(*result.data)) {
        result.data++;
        result.length--;
    }
    return result;
}

String sv_trim_right(String sv) {
    String result = sv;
    while (result.length > 0 && isspace(result.data[result.length - 1]))
        result.length--;
    return result;
}

String sv_trim(String sv) { return sv_trim_left(sv_trim_right(sv)); }

StringPair sv_split_delim(String sv, char delim) {
    StringPair result = {0};

    size_t i = 0;
    while (i < sv.length && sv.data[i] != delim)
        i++;

    // No delimiter found
    if (i == sv.length) {
        result.first = sv;
        result.second = StringNil;
        return result;
    }

    result.first = SV2(sv.data, i);
    result.second = SV2(sv.data + i + 1, sv.length - i - 1);

    return result;
}

ssize_t sv_find(const String sv, const char* str) {
    ssize_t i = 0;
    const size_t n = strlen(str);

    for (i = 0; i < sv.length; i++) {
        if (sv.data[i] == str[0] && i + n <= sv.length) {
            if (memcmp(str, &sv.data[i], n) == 0) {
                return i;
            }
        }
    }

    return -1;
}

ssize_t sv_rev_find(const String sv, const char* str) {
    ssize_t i = 0;
    const size_t n = strlen(str);

    for (i = sv.length - 1; i >= 0; i--) {
        if (sv.data[i] == str[0] && i + n <= sv.length) {
            if (memcmp(str, &sv.data[i], n) == 0) {
                return i;
            }
        }
    }

    return -1;
}

StringPair sv_split_str(String sv, const char* str) {
    StringPair result = {0};

    size_t n = strlen(str);
    if (n == 0 || sv.length < n) {
        result.first = sv;
        result.second = StringNil;
        return result;
    }

    size_t i = 0;
    bool found = false;
    for (i = 0; i + n <= sv.length; i++) {
        if (memcmp(str, &sv.data[i], n) == 0) {
            found = true;
            break;
        }
    }

    // No match found
    if (!found) {
        result.first = sv;
        result.second = StringNil;
        return result;
    }

    result.first = SV2(sv.data, i);
    result.second = SV2(sv.data + i + n, sv.length - i - n);

    return result;
}

String sv_clone(String sv) {
    char* str_copy = malloc(sv.length + 1);
    memcpy(str_copy, sv.data, sv.length);
    str_copy[sv.length] = 0;
    return SV2(str_copy, sv.length);
}

char *sv_to_c(String sv) {
    return sv_clone(sv).data;
}

char *sv_to_tmp_c(String sv) {
    char* str_copy = talloc(sv.length + 1);
    memcpy(str_copy, sv.data, sv.length);
    str_copy[sv.length] = 0;
    return str_copy;
}

String tprintf(const char* format, ...) {
    va_list args, args2;

    va_start(args, format);
    va_copy(args2, args);

    size_t n = vsnprintf(NULL, 0, format, args);
    assert(n > 0);

    char* str = talloc(n + 1);
    vsnprintf(str, n + 1, format, args2);

    va_end(args2);
    va_end(args);

    return SV2(str, n);
}

String tvprintf(const char* format, va_list args) {
    va_list args2;
    va_copy(args2, args);

    size_t n = vsnprintf(NULL, 0, format, args);
    assert(n > 0);

    char* str = talloc(n + 1);
    vsnprintf(str, n + 1, format, args2);

    va_end(args2);

    return SV2(str, n);
}

long sv_to_long(String sv, char** endptr) {
    assert(endptr != NULL);
    if (sv.length == 0 || sv.data == NULL)
        return 0;
    long l = 0;
    bool neg = false;

    if (sv.data[0] == '-')
        neg = true;
    else if (sv.data[0] == '+')
        neg = false;

    size_t i;
    if (sv.data[0] == '-' || sv.data[0] == '+')
        i = 1;
    else
        i = 0;

    if (i == sv.length) {
        *endptr = sv.data;
        return 0;
    }

    for (; i < sv.length; i++) {
        const char ch = sv.data[i];
        if (ch < '0' || ch > '9') {
            *endptr = &sv.data[i];
            return 0;
        }
        long d = ch - '0';
        if (l > (LONG_MAX - d) / 10) {
            *endptr = &sv.data[i];
            return neg ? LONG_MIN : LONG_MAX;
        }
        l = l * 10 + d;
    }
    *endptr = sv.data + sv.length;
    return (neg) ? -l : l;
}

int sv_to_int(String sv, char** endptr) { return (int) sv_to_long(sv, endptr); }

String sv_escape(String sv) {
    StringBuilder sb = {0};
    for (size_t i = 0; i < sv.length; i++) {
        unsigned char ch = (unsigned char) sv.data[i];
        switch (ch) {
            case '\r':
                sb_push_str(&sb, "\\r");
                break;
            case '\n':
                sb_push_str(&sb, "\\n");
                break;
            case '\t':
                sb_push_str(&sb, "\\t");
                break;
            case '\"':
                sb_push_str(&sb, "\\\"");
                break;
            case '\\':
                sb_push_str(&sb, "\\\\");
                break;
            default:
                if (ch <= 0x1F) {
                    sb_push_sv(&sb, tprintf("\\u%04x", ch));
                } else {
                    sb_push_char(&sb, sv.data[i]);
                }
        }
    }
    return sb_to_sv(&sb);
}

/* Hash Table */

HashTable hash_table_init(size_t capacity, KeyEqFunc key_eq,
                          KeyHashFunc key_hash) {
    assert(key_eq != NULL && "key_eq is required");
    assert(key_hash != NULL && "key_hash is required");

    HashTable v = {0};

    size_t sz = capacity * sizeof(HashTableEntry);
    v.entries = (HashTableEntry *) malloc(sz);
    assert(v.entries != NULL);
    v.capacity = capacity;
    v.key_eq = key_eq;
    v.key_hash = key_hash;

    memset(v.entries, 0, sz);
    return v;
}

bool hash_table_set(HashTable* v, void* key, void* val) {
    assert(v != NULL && "map is null");
    assert(key != NULL && "key is null");
    assert(val != NULL && "value is null");
    assert(v->entries != NULL && "uninitialized map");

    size_t index = v->key_hash(v->capacity, key);
    bool index_found = false;

    for (size_t i = 0; i < v->capacity; ++i) {
        size_t try_index = (i + index) % v->capacity;
        HashTableEntry entry = v->entries[try_index];

        if (entry.key == NULL || v->key_eq(entry.key, key)) {
            index = try_index;
            index_found = true;
            break;
        }
    }
    if (!index_found)
        return false;

    v->entries[index].key = key;
    v->entries[index].value = val;
    v->length++;

    return true;
}

bool hash_table_get(const HashTable* v, void* key, void** out) {
    assert(v != NULL && "map is null");
    assert(key != NULL && "key is null");
    assert(v->entries != NULL && "uninitialized map");

    size_t index = v->key_hash(v->capacity, key);
    bool index_found = false;

    for (size_t i = 0; i < v->capacity; ++i) {
        size_t try_index = (i + index) % v->capacity;
        HashTableEntry entry = v->entries[try_index];

        if (entry.key != NULL && v->key_eq(entry.key, key)) {
            index = try_index;
            index_found = true;
            break;
        }
    }
    if (!index_found)
        return false;
    if (out != NULL) {
        *out = v->entries[index].value;
    }

    return true;
}

bool hash_table_remove(HashTable* v, void* key, void** out) {
    assert(v != NULL && "map is null");
    assert(key != NULL && "key is null");
    assert(v->entries != NULL && "uninitialized map");

    size_t index = v->key_hash(v->capacity, key);
    bool index_found = false;

    for (size_t i = 0; i < v->capacity; ++i) {
        size_t try_index = (i + index) % v->capacity;
        HashTableEntry entry = v->entries[try_index];

        if (entry.key != NULL && v->key_eq(entry.key, key)) {
            index = try_index;
            index_found = true;
            break;
        }
    }
    if (!index_found)
        return false;

    if (out != NULL) {
        *out = v->entries[index].value;
    }

    v->entries[index].key = NULL;
    v->entries[index].value = NULL;
    v->length--;

    return true;
}

void hash_table_free(HashTable* v) {
    assert(v != NULL && "map is null");

    if (v->entries) {
        free(v->entries);
        v->entries = NULL;
        v->capacity = 0;
    }
}

// Json Encoding & Decoding

JsonValue *json_new_null(void) {
    JsonValue* value = malloc(sizeof(JsonValue));
    value->type = JSON_NULL;
    return value;
}

JsonValue *json_new_bool(bool b) {
    JsonValue* value = malloc(sizeof(JsonValue));
    value->type = JSON_BOOL;
    value->as.boolean = b;
    return value;
}

JsonValue *json_new_number(double n) {
    JsonValue* value = malloc(sizeof(JsonValue));
    value->type = JSON_NUMBER;
    value->as.number = n;
    return value;
}

JsonValue *json_new_string(const String s) {
    JsonValue* value = malloc(sizeof(JsonValue));
    value->type = JSON_STRING;
    value->as.string = sv_escape(s);
    return value;
}

JsonValue *json_new_cstr(char* s) {
    JsonValue* value = malloc(sizeof(JsonValue));
    value->type = JSON_STRING;
    value->as.string = sv_clone(SV(s));
    return value;
}

JsonValue *json_new_array(void) {
    JsonValue* value = malloc(sizeof(JsonValue));
    value->type = JSON_ARRAY;
    value->as.array = (JsonArray){0};
    return value;
}

JsonValue *json_new_object(void) {
    JsonValue* value = malloc(sizeof(JsonValue));
    value->type = JSON_OBJECT;
    value->as.object = (JsonObject){0};
    return value;
}

Error json_error_(Error cause, String s, const char* file, int line) {
    return errorf(SV_Fmt ": \"" SV_Fmt "\" at %s:%d", SV_Arg(cause.message),
                  SV_Arg(s), file, line);
}

#define json_error(cause, s) json_error_(cause, s, __FILE__, __LINE__)

bool json_is_end_char(char ch) { return ch == ',' || ch == ']' || ch == '}'; }

bool json_consume_char(String* sv) {
    if (sv->length == 0)
        return false;
    sv->data++;
    sv->length--;
    return true;
}

bool json_consume_literal(String* sv, const char* str, const size_t n) {
    if (sv->length < n)
        return false;

    for (size_t i = 0; i < n; i++) {
        if (*sv->data != str[i])
            return false;

        if (!json_consume_char(sv))
            return false;
    }

    return true;
}

Error json_decode_value(String* sv, JsonValue** out);

Error json_decode_null(String* sv, JsonValue** out) {
    if (!json_consume_literal(sv, "null", 4))
        return json_error(JsonErrorUnexpectedToken, *sv);

    *out = json_new_null();
    return ErrorNil;
}

Error json_decode_true(String* sv, JsonValue** out) {
    if (!json_consume_literal(sv, "true", 4))
        return json_error(JsonErrorUnexpectedToken, *sv);

    *out = json_new_bool(true);
    return ErrorNil;
}

Error json_decode_false(String* sv, JsonValue** out) {
    if (!json_consume_literal(sv, "false", 5))
        return json_error(JsonErrorUnexpectedToken, *sv);

    *out = json_new_bool(false);
    return ErrorNil;
}

Error json_decode_number(String* sv, JsonValue** out) {
    char* endptr;
    double number = strtod(sv->data, &endptr);
    if (endptr == sv->data)
        return JsonErrorUnexpectedToken;

    sv->length -= (endptr - sv->data);
    sv->data = endptr;

    *out = json_new_number(number);
    return ErrorNil;
}

Error json_decode_string(String* sv, JsonValue** out) {
    if (*sv->data != '\"')
        return json_error(JsonErrorUnexpectedToken, *sv);
    if (!json_consume_char(sv))
        return json_error(JsonErrorEOF, *sv);

    String str = {0};
    str.data = sv->data;

    while (sv->length > 0 && *sv->data != '\"') {
        if (!json_consume_char(sv))
            return json_error(JsonErrorEOF, *sv);
        str.length++;
    }

    if (*sv->data != '\"')
        return json_error(JsonErrorUnexpectedToken, *sv);
    if (!json_consume_char(sv))
        return json_error(JsonErrorEOF, *sv);

    *out = json_new_string(str);
    return ErrorNil;
}

Error json_decode_array(String* sv, JsonValue** out) {
    if (*sv->data != '[')
        return json_error(JsonErrorUnexpectedToken, *sv);
    if (!json_consume_char(sv))
        return json_error(JsonErrorEOF, *sv);

    JsonArray values = {0};

    while (sv->length > 0) {
        *sv = sv_trim_left(*sv);
        if (*sv->data == ']') {
            if (!json_consume_char(sv))
                return json_error(JsonErrorEOF, *sv);
            break;
        }

        JsonValue* value = NULL;
        Error err = json_decode_value(sv, &value);
        if (has_error(err))
            return err;

        array_append(&values, value);

        *sv = sv_trim_left(*sv);
        if (*sv->data == ',') {
            if (!json_consume_char(sv))
                return json_error(JsonErrorEOF, *sv);
        }
    }

    *out = json_new_array();
    (*out)->as.array = values;
    return ErrorNil;
}

Error json_decode_object(String* sv, JsonValue** out) {
    if (*sv->data != '{')
        return json_error(JsonErrorUnexpectedToken, *sv);
    if (!json_consume_char(sv))
        return json_error(JsonErrorEOF, *sv);

    JsonObject object = {0};

    while (sv->length > 0) {
        *sv = sv_trim_left(*sv);
        if (*sv->data == '}') {
            if (!json_consume_char(sv))
                return json_error(JsonErrorEOF, *sv);
            break;
        }

        JsonObjectEntry entry = {0};

        // Extracting key
        if (*sv->data != '\"')
            return json_error(JsonErrorUnexpectedToken, *sv);
        if (!json_consume_char(sv))
            return json_error(JsonErrorEOF, *sv);

        String key = {0};
        key.data = sv->data;

        while (sv->length > 0 && *sv->data != '\"') {
            if (!json_consume_char(sv))
                return json_error(JsonErrorEOF, *sv);
            key.length++;
        }

        if (*sv->data != '\"')
            return json_error(JsonErrorUnexpectedToken, *sv);
        if (!json_consume_char(sv))
            return json_error(JsonErrorEOF, *sv);

        entry.key = sv_clone(key);

        *sv = sv_trim_left(*sv);
        if (*sv->data != ':')
            return json_error(JsonErrorUnexpectedToken, *sv);
        if (!json_consume_char(sv))
            return json_error(JsonErrorEOF, *sv);

        // Extracting value
        JsonValue* value = NULL;
        const Error err = json_decode_value(sv, &value);
        if (has_error(err))
            return err;
        entry.value = value;

        *sv = sv_trim_left(*sv);
        if (*sv->data == ',') {
            if (!json_consume_char(sv))
                return json_error(JsonErrorEOF, *sv);
        }

        array_append(&object, entry);
    }

    *out = json_new_object();
    (*out)->as.object = object;
    return ErrorNil;
}

Error json_decode_value(String* sv, JsonValue** out) {
    *sv = sv_trim_left(*sv);
    if (sv->length == 0)
        return json_error(JsonErrorEOF, *sv);

    switch (*sv->data) {
        case 'n':
            return json_decode_null(sv, out);
        case 't':
            return json_decode_true(sv, out);
        case 'f':
            return json_decode_false(sv, out);
        case '\"':
            return json_decode_string(sv, out);
        case '[':
            return json_decode_array(sv, out);
        case '{':
            return json_decode_object(sv, out);
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
        case '-': {
            return json_decode_number(sv, out);
        }
        default:
            return json_error(JsonErrorUnexpectedToken, *sv);
    }
}

Error json_decode(String sv, JsonValue** out) {
    String sv2 = sv_trim(sv);
    Error err = json_decode_value(&sv2, out);
    if (has_error(err))
        return err;
    if (sv2.length > 0)
        return json_error(JsonErrorUnexpectedToken, sv2);
    return ErrorNil;
}

JsonNumber json_get_number(const JsonValue* json) {
    assert(json != NULL);
    assert(json->type == JSON_NUMBER);
    return json->as.number;
}

JsonString json_get_string(const JsonValue* json) {
    assert(json != NULL);
    assert(json->type == JSON_STRING);
    return json->as.string;
}

JsonValue *json_object_get(const JsonValue* json, String key) {
    assert(json != NULL);
    assert(json->type == JSON_OBJECT);
    for (size_t i = 0; i < json->as.object.length; i++) {
        const JsonObjectEntry entry = json->as.object.data[i];
        if (sv_equal(entry.key, key)) {
            return entry.value;
        }
    }
    return NULL;
}

JsonValue *json_get(const JsonValue* json, String key) {
    assert(json != NULL);

    StringPair p = sv_split_delim(key, '.');
    JsonValue* value = (JsonValue *) json;
    while (p.first.length != 0) {
        if (value->type == JSON_OBJECT) {
            value = json_object_get(value, p.first);
        } else if (value->type == JSON_ARRAY) {
            char* endptr;
            const int index = sv_to_int(p.first, &endptr);
            if (endptr == p.first.data + p.first.length) {
                value = json_array_get(value, index);
            } else {
                return NULL;
            }
        } else {
            return NULL;
        }
        if (value == NULL)
            return NULL;
        p = sv_split_delim(p.second, '.');
    }
    return value;
}

void json_object_set(JsonValue* json, const String key, JsonValue* val) {
    assert(json != NULL);
    assert(json->type == JSON_OBJECT);
    for (size_t i = 0; i < json->as.object.length; i++) {
        const JsonObjectEntry entry = json->as.object.data[i];
        if (sv_equal(entry.key, key)) {
            json->as.object.data[i].value = val;
            return;
        }
    }

    array_append(&json->as.object,
                 ((JsonObjectEntry){sv_clone(key), val}));
}

bool json_object_remove(JsonValue* json, const String key) {
    assert(json != NULL);
    assert(json->type == JSON_OBJECT);
    size_t index = -1;
    for (size_t i = 0; i < json->as.object.length; i++) {
        const JsonObjectEntry entry = json->as.object.data[i];
        if (sv_equal(entry.key, key)) {
            index = i;
            break;
        }
    }

    if (index != -1) {
        array_remove(&json->as.object, index);
        return true;
    }

    return false;
}

JsonValue *json_array_get(const JsonValue* json, int i) {
    assert(json != NULL);
    assert(json->type == JSON_ARRAY);
    assert(i < json->as.array.length);
    return json->as.array.data[i];
}

void json_array_append(JsonValue* json, JsonValue* val) {
    assert(json != NULL);
    assert(json->type == JSON_ARRAY);
    array_append(&json->as.array, val);
}

void json_array_remove(JsonValue* json, size_t index) {
    assert(json != NULL);
    assert(json->type == JSON_ARRAY);
    assert(index < json->as.array.length);
    array_remove(&json->as.array, index);
}

void sb_push_whitespace(StringBuilder* sb, int indent) {
    for (int i = 0; i < indent; i++) {
        sb_push_char(sb, ' ');
    }
}

void json_encode_(JsonValue json, StringBuilder* sb, int pp, int indent) {
    switch (json.type) {
        case JSON_NULL:
            sb_push_str(sb, "null");
            break;
        case JSON_BOOL:
            sb_push_str(sb, json.as.boolean ? "true" : "false");
            break;
        case JSON_NUMBER:
            sb_push_double(sb, json.as.number);
            break;
        case JSON_STRING:
            sb_push_char(sb, '\"');
            sb_push_sv(sb, json.as.string);
            sb_push_char(sb, '\"');
            break;

        case JSON_ARRAY: {
            sb_push_char(sb, '[');
            if (json.as.array.length > 0 && pp > 0)
                sb_push_char(sb, '\n');

            for (int i = 0; i < json.as.array.length; i++) {
                if (pp > 0)
                    sb_push_whitespace(sb, indent);
                json_encode_(*json.as.array.data[i], sb, pp, indent + pp);
                if (i < json.as.array.length - 1)
                    sb_push_char(sb, ',');
                if (pp > 0)
                    sb_push_char(sb, '\n');
            }
            if (json.as.array.length > 0 && pp > 0)
                sb_push_whitespace(sb, indent - pp);
            sb_push_char(sb, ']');
            break;
        }

        case JSON_OBJECT: {
            sb_push_char(sb, '{');
            if (json.as.object.length > 0 && pp > 0)
                sb_push_char(sb, '\n');

            for (int i = 0; i < json.as.object.length; i++) {
                if (pp > 0)
                    sb_push_whitespace(sb, indent);
                sb_push_char(sb, '\"');
                sb_push_sv(sb, json.as.object.data[i].key);
                sb_push_char(sb, '\"');
                sb_push_char(sb, ':');
                json_encode_(*json.as.object.data[i].value, sb, pp, indent + pp);
                if (i < json.as.array.length - 1)
                    sb_push_char(sb, ',');
                if (pp > 0)
                    sb_push_char(sb, '\n');
            }
            if (json.as.object.length > 0 && pp > 0)
                sb_push_whitespace(sb, indent - pp);
            sb_push_char(sb, '}');
            break;
        }
    }
}

void json_encode(const JsonValue json, StringBuilder* sb, int pp) {
    return json_encode_(json, sb, pp, pp);
}

void json_print(FILE* f, JsonValue json, int pp) {
    StringBuilder sb = {0};
    json_encode(json, &sb, pp);
    fprintf(f, SV_Fmt "\n", SV_Arg(sb));
    sb_free(&sb);
}

void json_free(JsonValue* json) {
    assert(json != NULL);

    switch (json->type) {
        case JSON_NULL:
        case JSON_BOOL:
        case JSON_NUMBER:
            free(json);
            break;
        case JSON_STRING:
            free(json->as.string.data);
            free(json);
            break;

        case JSON_ARRAY: {
            if (json->as.array.data) {
                for (int i = 0; i < json->as.array.length; i++) {
                    json_free(json->as.array.data[i]);
                }
                free(json->as.array.data);
            }
            free(json);
            break;
        }

        case JSON_OBJECT: {
            if (json->as.object.data) {
                for (int i = 0; i < json->as.object.length; i++) {
                    if (json->as.object.data[i].key.data != NULL)
                        free(json->as.object.data[i].key.data);
                    json_free(json->as.object.data[i].value);
                }
                free(json->as.object.data);
            }
            free(json);
            break;
        }
    }
}

// File I/O

size_t file_size(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

bool file_exists(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return true;
    }
    return false;
}

Error read_entire_file(const char* path, StringBuilder* sb) {
    size_t size = file_size(path);
    if (size == 0) {
        return ErrorFileEmpty;
    }
    sb_resize(sb, size);
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return ErrorReadFile;
    }
    size_t n = fread(sb->data, 1, size, file);
    if (n != size) {
        return errorf("failed to read file %s: %s", path, strerror(errno));
    }
    sb->length = n;
    sb->data[n] = 0;
    fclose(file);
    return ErrorNil;
}

Error write_entire_file(const char* path, String sv) {
    FILE* file = fopen(path, "w");
    if (file == NULL) {
        return ErrorWriteFile;
    }
    size_t n = fwrite(sv.data, 1, sv.length, file);
    if (n != sv.length) {
        return errorf("failed to write file %s: %s", path, strerror(errno));
    }
    fclose(file);
    return ErrorNil;
}

Error make_directory(const char* path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return errorf("mkdir failed for %s: %s", tmp, strerror(errno));
            }
            *p = '/';
        }
    }

    // create the final directory
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return errorf("mkdir failed for %s: %s", tmp, strerror(errno));
    }

    return ErrorNil;
}

#define HEX_CHARSET_LEN 16
const char hex_chars[] = "0123456789abcdef";

Error random_bytes(char* buf, size_t n) {
    assert(buf != NULL);
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd == -1) {
        return errorf("failed to open /dev/urandom: %s", strerror(errno));
    }

    if (read(fd, buf, n) != n) {
        close(fd);
        return errorf("failed to read from /dev/urandom: %s", strerror(errno));
    }
    close(fd);

    return ErrorNil;
}

String random_id(size_t n) {
    unsigned char* raw = talloc(n);
    try(random_bytes((char*)raw, n));

    char* id = talloc(n + 1);
    for (size_t i = 0; i < n; ++i) {
        id[i] = hex_chars[raw[i] % HEX_CHARSET_LEN];
    }
    id[n] = 0;
    return SV2(id, n);
}

// ---- TLS / BufIO adapters --------------------------------------------------

ssize_t ssl_raw_read(void* ssl, char* buf, size_t n) {
    int r = SSL_read((SSL*)ssl, buf, (int)n);
    if (r <= 0) {
        int err = SSL_get_error((SSL*)ssl, r);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;       // peer closed cleanly
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            errno = EAGAIN;
            return -1;
        }
        errno = EIO;
        return -1;
    }
    return r;
}

ssize_t ssl_raw_write(void* ssl, const char* buf, size_t n) {
    int r = SSL_write((SSL*)ssl, buf, (int)n);
    if (r <= 0) {
        int err = SSL_get_error((SSL*)ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            errno = EAGAIN;
            return -1;
        }
        errno = EIO;
        return -1;
    }
    return r;
}

ssize_t fd_raw_read(void* fd_as_ptr, char* buf, size_t n) {
    return read((int)(intptr_t)fd_as_ptr, buf, n);
}

ssize_t fd_raw_write(void* fd_as_ptr, const char* buf, size_t n) {
    return write((int)(intptr_t)fd_as_ptr, buf, n);
}

const char* tls_last_error(void) {
    unsigned long err = ERR_get_error();
    if (err == 0) return strerror(errno);
    // Drain the rest of the queue so the next call gets a clean slate.
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    while (ERR_get_error()) {}
    return tprintf("%s", buf).data;
}

void tls_session_close(SSL* ssl) {
    if (ssl == NULL) return;
    int fd = SSL_get_fd(ssl);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    if (fd >= 0) close(fd);
}

static SSL_CTX* g_tls_ctx = NULL;

SSL_CTX* tls_server_ctx(void) {
    if (g_tls_ctx != NULL) return g_tls_ctx;

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        CRITICAL("SSL_CTX_new failed");
        abort();
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    String cert = config_get_string(SV("server.tls.cert"), SV("cert.pem"));
    String key  = config_get_string(SV("server.tls.key"),  SV("key.pem"));

    // config strings aren't NUL-terminated guarantees; copy to a C string.
    char cert_path[1024], key_path[1024];
    snprintf(cert_path, sizeof(cert_path), "%.*s", (int)cert.length, cert.data);
    snprintf(key_path,  sizeof(key_path),  "%.*s", (int)key.length,  key.data);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) <= 0) {
        ERR_print_errors_fp(stderr);
        CRITICAL("failed to load TLS cert %s", cert_path);
        abort();
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        CRITICAL("failed to load TLS key %s", key_path);
        abort();
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        CRITICAL("TLS cert and key do not match");
        abort();
    }

    g_tls_ctx = ctx;
    return ctx;
}

// ---- Base64 ----------------------------------------------------------------

static const int8_t b64_table[256] = {
    ['A']= 0,['B']= 1,['C']= 2,['D']= 3,['E']= 4,['F']= 5,['G']= 6,['H']= 7,
    ['I']= 8,['J']= 9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
    ['Y']=24,['Z']=25,
    ['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,['g']=32,['h']=33,
    ['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,['o']=40,['p']=41,
    ['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,['w']=48,['x']=49,
    ['y']=50,['z']=51,
    ['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,['5']=57,['6']=58,['7']=59,
    ['8']=60,['9']=61,['+']=62,['/']=63,
};

String base64_decode(String src) {
    // Strip whitespace length; we'll skip on the fly.
    size_t max_out = (src.length / 4) * 3 + 3;
    char* out = talloc(max_out);
    size_t out_len = 0;

    uint32_t accum = 0;
    int bits = 0;
    for (size_t i = 0; i < src.length; i++) {
        unsigned char c = (unsigned char)src.data[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64_table[c];
        if (v == 0 && c != 'A') {
            // unknown char (b64_table is zero-initialized for non-mapped slots)
            // 'A' maps to 0 legitimately, everything else with value 0 is invalid.
            continue;
        }
        accum = (accum << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[out_len++] = (char)((accum >> bits) & 0xFF);
        }
    }
    return (String){ .data = out, .length = out_len };
}
