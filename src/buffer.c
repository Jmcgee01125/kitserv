/* Part of Kitserv, licensed under the GNU Affero GPL. */

#include "buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_INCREMENT (256)

int kitserv_buffer_init(buffer_t* buffer, off_t initial_size)
{
    buffer->buf = malloc(initial_size);
    if (buffer->buf == NULL) {
        return -1;
    }
    buffer->len = 0;
    buffer->max = initial_size;
    return 0;
}

void kitserv_buffer_free(buffer_t* buffer)
{
    free(buffer->buf);
}

void kitserv_buffer_reset(buffer_t* buffer, off_t size)
{
    buffer->len = 0;
    if (buffer->max > size) {
        char* new_buf = realloc(buffer->buf, size);
        if (new_buf) {
            buffer->buf = new_buf;
            buffer->max = size;
        }
    }
}

static inline char* kitserv_buffer_ensure_space(buffer_t* buffer, off_t n)
{
    if (buffer->len + n >= buffer->max) {
        off_t new_max = (n / BUFFER_INCREMENT + 1) * BUFFER_INCREMENT;  // min increments to fit
        char* new_buf = realloc(buffer->buf, n);
        if (!new_buf) {
            return NULL;
        }
        buffer->buf = new_buf;
        buffer->max = new_max;
    }
    return &buffer->buf[buffer->len];
}

int kitserv_buffer_append(buffer_t* buffer, const void* elems, off_t n)
{
    char* loc = kitserv_buffer_ensure_space(buffer, n);
    if (!loc) {
        return -1;
    }
    memcpy(loc, elems, n);
    buffer->len += n;
    return 0;
}

int kitserv_buffer_appendva(buffer_t* buffer, const char* fmt, va_list* ap)
{
    int rc;

    rc = vsnprintf(&buffer->buf[buffer->len], buffer->max - buffer->len, fmt, *ap);
    if (rc > buffer->max - buffer->len) {
        // failed - would have overrun the buffer (so, expand and retry)
        kitserv_buffer_ensure_space(buffer, rc);
        rc = vsnprintf(&buffer->buf[buffer->len], buffer->max - buffer->len, fmt, *ap);
        if (rc > buffer->max - buffer->len) {
            return -1;
        }
    }
    buffer->len += rc;
    return 0;
}

int kitserv_buffer_appendf(buffer_t* buffer, const char* fmt, ...)
{
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = kitserv_buffer_appendva(buffer, fmt, &ap);
    va_end(ap);
    return rc;
}
