#include "bufio.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFIO_READ_CAPACITY  4096
#define BUFIO_WRITE_CAPACITY 4096

const Error err_eof               = { .message = SV("bufio: EOF") };
const Error err_message_too_large = { .message = SV("bufio: message too large") };

bool bufio_is_eof(Error err) {
    return err.message.data == err_eof.message.data;
}

static bool is_too_large(Error err) {
    return err.message.data == err_message_too_large.message.data;
}

BufIO bufio_new(void* transport, RawRead raw_read, RawWrite raw_write) {
    BufIO b = {
        .transport = transport,
        .raw_read  = raw_read,
        .raw_write = raw_write,
        .read_buffer  = malloc(BUFIO_READ_CAPACITY),
        .read_buffer_capacity = BUFIO_READ_CAPACITY,
        .write_buffer = malloc(BUFIO_WRITE_CAPACITY),
        .write_buffer_capacity = BUFIO_WRITE_CAPACITY,
    };
    return b;
}

void bufio_free(BufIO* bio) {
    if (bio == NULL) return;
    free(bio->read_buffer);
    free(bio->write_buffer);
    bio->read_buffer = NULL;
    bio->write_buffer = NULL;
}

void bufio_reset_read(BufIO* bio) {
    bio->consumed = 0;
    bio->filled = 0;
}

static Error fill(BufIO* b) {
    if (b->consumed == b->filled) {
        b->consumed = b->filled = 0;
    } else if (b->consumed > b->read_buffer_capacity / 2) {
        memmove(b->read_buffer, b->read_buffer + b->consumed, b->filled - b->consumed);
        b->filled  -= b->consumed;
        b->consumed = 0;
    }
    if (b->filled == b->read_buffer_capacity) return err_message_too_large;

    for (;;) {
        ssize_t n = b->raw_read(b->transport, b->read_buffer + b->filled,
                                b->read_buffer_capacity - b->filled);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return errorf("bufio read failed: %s", strerror(errno));
        }
        if (n == 0) return err_eof;
        b->filled += n;
        return ErrorNil;
    }
}

Error bufio_read_until(BufIO* bio, const char* terminator, String* out) {
    const size_t tlen = strlen(terminator);
    for (;;) {
        if (bio->filled > bio->consumed) {
            String window = SV2(bio->read_buffer + bio->consumed, bio->filled - bio->consumed);
            ssize_t idx = sv_find(window, terminator);
            if (idx >= 0) {
                size_t end = bio->consumed + idx + tlen;
                *out = SV2(bio->read_buffer + bio->consumed, end - bio->consumed);
                bio->consumed = end;
                return ErrorNil;
            }
        }
        Error err = fill(bio);
        if (has_error(err)) return err;
    }
}

Error bufio_read_n(BufIO* bio, size_t n, String* out) {
    while (bio->filled - bio->consumed < n) {
        Error err = fill(bio);
        if (has_error(err)) return err;
    }
    *out = SV2(bio->read_buffer + bio->consumed, n);
    bio->consumed += n;
    return ErrorNil;
}

Error bufio_read_until_into(BufIO* bio, const char* terminator, StringBuilder* out) {
    String view;
    Error err = bufio_read_until(bio, terminator, &view);
    if (!has_error(err)) {
        sb_push_sv(out, view);
        return ErrorNil;
    }
    // If the terminator didn't fit in one buffer-full, drain partial bytes into
    // `out` and keep reading. Anything else (real EOF, I/O error) propagates.
    if (!is_too_large(err)) return err;

    const size_t tlen = strlen(terminator);
    for (;;) {
        // Keep tlen-1 trailing bytes in the bufio buffer so a terminator
        // straddling the flush boundary still gets matched.
        size_t available = bio->filled - bio->consumed;
        if (available > tlen - 1) {
            size_t emit = available - (tlen - 1);
            sb_push_sv(out, SV2(bio->read_buffer + bio->consumed, emit));
            bio->consumed += emit;
        }

        err = bufio_read_until(bio, terminator, &view);
        if (!has_error(err)) {
            sb_push_sv(out, view);
            return ErrorNil;
        }
        if (!is_too_large(err)) return err;
    }
}

Error bufio_read_n_into(BufIO* bio, size_t n, StringBuilder* out) {
    while (n > 0) {
        if (bio->filled == bio->consumed) {
            Error err = fill(bio);
            if (has_error(err)) return err;
        }
        size_t available = bio->filled - bio->consumed;
        size_t take = available < n ? available : n;
        sb_push_sv(out, SV2(bio->read_buffer + bio->consumed, take));
        bio->consumed += take;
        n -= take;
    }
    return ErrorNil;
}

static void wb_append(BufIO* bio, const char* data, size_t len) {
    while (len > 0) {
        size_t free_space = bio->write_buffer_capacity - bio->written;
        if (free_space == 0) {
            Error err = bufio_flush(bio);
            if (has_error(err)) return;
            free_space = bio->write_buffer_capacity;
        }
        size_t take = len < free_space ? len : free_space;
        memcpy(bio->write_buffer + bio->written, data, take);
        bio->written += take;
        data += take;
        len  -= take;
    }
}

void bufio_write(BufIO* bio, String data) {
    wb_append(bio, data.data, data.length);
}

void bufio_write_line(BufIO* bio, String data) {
    wb_append(bio, data.data, data.length);
    wb_append(bio, "\r\n", 2);
}

Error bufio_flush(BufIO* bio) {
    size_t off = 0;
    while (off < bio->written) {
        ssize_t n = bio->raw_write(bio->transport, bio->write_buffer + off, bio->written - off);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            // Leave buffer intact so a higher-level retry could re-flush.
            return errorf("bufio write failed: %s", strerror(errno));
        }
        if (n == 0) return errorf("bufio write returned 0");
        off += n;
    }
    bio->written = 0;
    return ErrorNil;
}

Error bufio_send_line(BufIO* bio, String data) {
    bufio_write_line(bio, data);
    return bufio_flush(bio);
}
