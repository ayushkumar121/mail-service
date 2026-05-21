#ifndef BUFIO_H
#define BUFIO_H

#include <stddef.h>
#include <sys/types.h>

#include "basic.h"

typedef ssize_t (*RawRead) (void* transport, char* buf, size_t n);
typedef ssize_t (*RawWrite)(void* transport, const char* buf, size_t n);

typedef struct BufIO {
    void*    transport;
    RawRead  raw_read;
    RawWrite raw_write;

    char*  read_buffer;
    size_t read_buffer_capacity;
    size_t consumed;
    size_t filled;

    char*  write_buffer;
    size_t write_buffer_capacity;
    size_t written;
} BufIO;

BufIO bufio_new(void* transport, RawRead raw_read, RawWrite raw_write);
void  bufio_free(BufIO* bio);

Error bufio_read_until(BufIO* bio, const char* terminator, String* out);
Error bufio_read_n    (BufIO* bio, size_t n, String* out);

Error bufio_read_until_into(BufIO* bio, const char* terminator, StringBuilder* out);
Error bufio_read_n_into    (BufIO* bio, size_t n, StringBuilder* out);

void  bufio_write     (BufIO* bio, String data);
void  bufio_write_line(BufIO* bio, String data);
Error bufio_flush     (BufIO* bio);
Error bufio_send_line (BufIO* bio, String data);

void  bufio_reset_read(BufIO* bio);

extern const Error err_eof;
extern const Error err_message_too_large;

bool bufio_is_eof(Error err);

#endif
