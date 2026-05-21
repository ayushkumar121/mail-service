#ifndef BASIC_H
#define BASIC_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define safe_free(ptr) if (ptr) {free(ptr); (ptr) = NULL;}

// Modules can set LOG_PREFIX (a string literal) before any log call to scope
#ifndef LOG_PREFIX
#define LOG_PREFIX ""
#endif

// Leading "<N>" is a syslog priority prefix (RFC 5424);
#define DEBUG(format, ...) fprintf(stderr, "<7>DEBUG: " LOG_PREFIX format "\n", ##__VA_ARGS__)
#define INFO(format, ...)  fprintf(stderr, "<6>INFO: "  LOG_PREFIX format "\n", ##__VA_ARGS__)
#define WARN(format, ...)  fprintf(stderr, "<4>WARN: "  LOG_PREFIX format "\n", ##__VA_ARGS__)
#define ERROR(format, ...) fprintf(stderr, "<3>ERROR: " LOG_PREFIX format "\n", ##__VA_ARGS__)
#define CRITICAL(format, ...) fprintf(stderr, "<2>CRITICAL: " LOG_PREFIX format "\n", ##__VA_ARGS__)

#define PAIR(T1, T2)                                                           \
  struct {                                                                     \
    T1 first;                                                                  \
    T2 second;                                                                 \
  }

#define ARRAY_INIT_CAP 2
#define ARRAY(T)                                                               \
  struct {                                                                     \
    size_t length;                                                             \
    size_t capacity;                                                           \
    T *data;                                                                  \
  }

#define array_append(array, item)                                              \
  do {                                                                         \
    if ((array)->capacity < (array)->length + 1) {                             \
      (array)->capacity =                                                      \
          ((array)->capacity == 0) ? ARRAY_INIT_CAP : (array)->capacity * 1.5; \
      void* ptr = realloc(                                                     \
          (array)->data, (array)->capacity * sizeof(*(array)->data));        \
      assert(ptr != NULL);                                                     \
      (array)->data = ptr;                                                    \
    }                                                                          \
    (array)->data[(array)->length++] = (item);                                \
  } while (0)

#define array_remove(array, i)                                                 \
  do {                                                                         \
    size_t j = (i);                                                            \
    assert(j < (array)->length);                                               \
    (array)->data[j] = (array)->data[--(array)->length];                     \
  } while (0)

#define array_free(array)                                                      \
  do {                                                                         \
    if ((array)->capacity > 0)                                                 \
      free((array)->data);                                                \
  } while (0)

// Temp Allocator
#define TEMP_BUFFER_CAP (4 * 1024)

void *talloc(size_t bytes);
void treset();

// String
typedef struct {
    size_t length;
    size_t capacity;
    char* data;
} StringBuilder;

typedef struct {
    size_t length;
    char* data;
} String;

#define StringNil (String){0}

void sb_resize(StringBuilder* sb, size_t new_capacity);
void sb_free(const StringBuilder* sb);
void sb_push_str(StringBuilder* sb, const char* str);
void sb_push_sv(StringBuilder* sb, String sv);
void sb_push_char(StringBuilder* sb, char ch);
void sb_push_long(StringBuilder* sb, long l);
void sb_push_double(StringBuilder* sb, double d);
String sb_to_sv(const StringBuilder* sb);
StringBuilder sb_clone(const StringBuilder* sb);

#define SV_Fmt "%.*s"
#define SV_Arg(sv) (int)(sv).length, (sv).data

#define SV2(s, len) (String){len, s}
#define SV(s) (String){sizeof(s)-1, s}

bool sv_equal(String s1, String s2);
bool sv_equal_ignore_case(String s1, String s2);
ssize_t sv_find(String sv, const char* str);
ssize_t sv_rev_find(String sv, const char* str);
String sv_trim_left(String sv);
String sv_trim_right(String sv);
String sv_trim(String sv);

typedef PAIR(String, String) StringPair;

StringPair sv_split_delim(String sv, char delim);
StringPair sv_split_str(String sv, const char* str);
String sv_clone(String sv); // Clones the string on the heap
char* sv_to_c(String sv);
char* sv_to_tmp_c(String sv);

String tprintf(const char* format, ...) __attribute__((format(printf, 1, 2)));
String tvprintf(const char* format, va_list);

long sv_to_long(String sv, char** endptr);
int sv_to_int(String sv, char** endptr);
String sv_escape(String sv); // This heap allocates memory

// Hash Table

typedef struct {
    void* key;
    void* value;
} HashTableEntry;

typedef bool (*KeyEqFunc)(void* a, void* b);
typedef size_t (*KeyHashFunc)(size_t capacity, void* a);

typedef struct {
    HashTableEntry* entries;
    size_t length;
    size_t capacity;

    KeyEqFunc key_eq;
    KeyHashFunc key_hash;
} HashTable;

HashTable hash_table_init(size_t capacity, KeyEqFunc key_eq,
                          KeyHashFunc key_hash);
bool hash_table_set(HashTable* v, void* key, void* val);
bool hash_table_get(const HashTable* v, void* key, void** out);
bool hash_table_remove(HashTable* v, void* key, void** out);
void hash_table_free(HashTable* v);

// Error Handling
typedef struct {
    String message;
} Error;

#define MAX_ERROR_LENGTH 500
#define ErrorNil (Error){0}

Error error(char* message);

Error error_sv(String message);

Error errorf(const char* format, ...) __attribute__((format(printf, 1, 2)));

bool has_error(Error err);

void try_(Error err, char* file, int line);

#define try(err) try_(err, __FILE__, __LINE__)

// JSON Encoding & Decoding

#define JsonErrorEOF                                                           \
  (Error) { SV("json eof") }
#define JsonErrorUnexpectedToken                                               \
  (Error) { SV("json unexpected token") }

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_OBJECT,
    JSON_ARRAY
} JsonType;

typedef struct JsonObjectEntry JsonObjectEntry;
typedef struct JsonValue JsonValue;
typedef double JsonNumber;
typedef bool JsonBoolean;
typedef String JsonString;

typedef ARRAY(JsonValue *) JsonArray;

typedef ARRAY(JsonObjectEntry) JsonObject;

typedef struct JsonValue {
    JsonType type;

    union {
        JsonBoolean boolean;
        JsonNumber number;
        JsonString string;
        JsonArray array;
        JsonObject object;
    } as;
} JsonValue;

typedef struct JsonObjectEntry {
    String key;
    JsonValue* value;
} JsonObjectEntry;

JsonValue *json_new_null(void);
JsonValue *json_new_bool(bool b);
JsonValue *json_new_number(double n);
JsonValue *json_new_string(String s);
JsonValue *json_new_cstr(char* s);
JsonValue *json_new_array(void);
JsonValue *json_new_object(void);
Error json_decode(String sv, JsonValue** out);
JsonNumber json_get_number(const JsonValue* json);
JsonString json_get_string(const JsonValue* json);
JsonValue *json_object_get(const JsonValue* json, String key);
JsonValue *json_get(const JsonValue* json, String key);
void json_object_set(JsonValue* json, String key, JsonValue* val);
bool json_object_remove(JsonValue* json, String key);
JsonValue *json_array_get(const JsonValue* json, int index);
void json_array_append(JsonValue* json, JsonValue* val);
void json_array_remove(JsonValue* json, size_t index);
void json_encode(JsonValue json, StringBuilder* sb, int pp);
void json_print(FILE* file, JsonValue json, int pp);
void json_free(JsonValue* json);

// File I/O

#define ErrorReadFile                                                          \
  (Error) { SV("failed to read file") }
#define ErrorWriteFile                                                         \
  (Error) { SV("failed to write file") }
#define ErrorFileEmpty                                                         \
  (Error) { SV("file is empty") }

size_t file_size(const char* path);

bool file_exists(const char* path);

Error read_entire_file(const char* path, StringBuilder* sb);

Error write_entire_file(const char* path, String sv);

Error make_directory(const char* path);

// UUID
#define RANDOM_ID_LEN 12

Error random_bytes(char* buf, size_t n);

// Allocated in temp buffer should be cloned for long-lived objects
String random_id(size_t n);

// BUF IO
typedef struct BufIO BufIO;

typedef ssize_t (*RawRead) (void*, char* buf, size_t n);
typedef ssize_t (*RawWrite) (void*, const char*, size_t n);

typedef struct BufIO {
    void* file;
    StringBuilder read_buf;
    StringBuilder write_buf;
    StringBuilder overflow;

    RawRead raw_read;
    RawWrite raw_write;
} BufIO;

BufIO bufio_new(void* file, RawRead raw_read, RawWrite raw_write);
Error bufio_read_until(BufIO* bio, const char* terminator);
Error bufio_read_n(BufIO* bio, size_t n);
Error bufio_read_line(BufIO* bio);
Error bufio_read_all(BufIO* bio);
// Append to the internal write buffer without flushing. Must be paired with bufio_flush.
void  bufio_write(BufIO* bio, String data);
void  bufio_write_line(BufIO* bio, String data);
Error bufio_flush(BufIO* bio);

// Append + flush in one call. Use for single-shot replies; for batched output,
// call bufio_write_line repeatedly and finish with bufio_flush (or bufio_send_line).
Error bufio_send(BufIO* bio, String data);
Error bufio_send_line(BufIO* bio, String data);

void  bufio_free(BufIO* bio);

// TLS helpers (OpenSSL). Implementations live in basic.c.
// Forward-declare OpenSSL types so we don't pull <openssl/ssl.h> into every TU.
typedef struct ssl_st     SSL;
typedef struct ssl_ctx_st SSL_CTX;

// Process-wide TLS server context. Lazily built on first call; loads cert/key
// from config keys "server.tls.cert" / "server.tls.key" (defaults: cert.pem / key.pem).
// Aborts on load failure - the server can't run without certs.
SSL_CTX* tls_server_ctx(void);

// Adapters for BufIO over an SSL* handle. Pass these to bufio_new.
ssize_t ssl_raw_read (void* ssl, char* buf, size_t n);
ssize_t ssl_raw_write(void* ssl, const char* buf, size_t n);

// Adapters for BufIO over a raw fd cast to (void*)(intptr_t)fd.
ssize_t fd_raw_read (void* fd_as_ptr, char* buf, size_t n);
ssize_t fd_raw_write(void* fd_as_ptr, const char* buf, size_t n);

// Shutdown + free the SSL object and close the underlying fd in the correct order.
void tls_session_close(SSL* ssl);

// Base64 (used by SMTP AUTH PLAIN, etc.). Allocated in temp buffer.
String base64_decode(String src);

// Priority Queue
#define PQUEUE(T) \
  struct { \
    ARRAY(T) arr; \
    int (*cmp)(const T*, const T*); \
  }

#define pqueue_free(pq) array_free(&(pq)->arr)

#define pqueue_empty(pq) ((pq)->arr.length == 0)

#define pqueue_swap(pq, i, j) \
  do { \
    typeof((pq)->arr.items[0]) temp = (pq)->arr.items[i]; \
    (pq)->arr.items[i] = (pq)->arr.items[j]; \
    (pq)->arr.items[j] = temp; \
  } while (0)

#define pqueue_heapify_up(pq, idx) \
  do { \
    size_t _i = (idx); \
    while (_i > 0) { \
      size_t _parent = (_i - 1) / 2; \
      if ((pq)->cmp(&(pq)->arr.items[_i], &(pq)->arr.items[_parent]) <= 0) \
        break; \
      pqueue_swap(pq, _i, _parent); \
      _i = _parent; \
    } \
  } while (0)

#define pqueue_heapify_down(pq, idx) \
  do { \
    size_t _i = (idx); \
    while (1) { \
      size_t _left = 2 * _i + 1; \
      size_t _right = 2 * _i + 2; \
      size_t _largest = _i; \
      \
      if (_left < (pq)->arr.length && \
          (pq)->cmp(&(pq)->arr.items[_left], &(pq)->arr.items[_largest]) > 0) \
        _largest = _left; \
      if (_right < (pq)->arr.length && \
          (pq)->cmp(&(pq)->arr.items[_right], &(pq)->arr.items[_largest]) > 0) \
        _largest = _right; \
      \
      if (_largest == _i) break; \
      \
      pqueue_swap(pq, _i, _largest); \
      _i = _largest; \
    } \
  } while (0)

#define pqueue_push(pq, item) \
  do { \
    array_append(&(pq)->arr, item); \
    pqueue_heapify_up(pq, (pq)->arr.length - 1); \
  } while (0)

#define pqueue_pop(pq, out) \
  do { \
    assert((pq)->arr.length > 0); \
    if (out) *(out) = (pq)->arr.items[0]; \
    (pq)->arr.items[0] = (pq)->arr.items[(pq)->arr.length - 1]; \
    (pq)->arr.length--; \
    if ((pq)->arr.length > 0) \
      pqueue_heapify_down(pq, 0); \
  } while (0)

#define pqueue_peek(pq, out) \
  do { \
    assert((pq)->arr.length > 0); \
    if (out) *(out) = (pq)->arr.items[0]; \
  } while (0)

#endif // BASIC_H
