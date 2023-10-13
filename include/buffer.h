/* Part of FileKit, licensed under the GNU Affero GPL. */

#ifndef BUFFER_H
#define BUFFER_H

#include <stdlib.h>
#include <string.h>

#define BUFFER_INCREMENT (256)

typedef struct {
    char* buf;
    off_t len;
    off_t max;
} buffer_t;

/**
 * Initialize the given buffer to the initial size.
 * Returns 0 if successful, -1 on error.
 */
static inline int buffer_init(buffer_t* buffer, off_t initial_size)
{
    buffer->buf = malloc(initial_size);
    if (buffer->buf == NULL) {
        return -1;
    }
    buffer->len = 0;
    buffer->max = initial_size;
    return 0;
}

/**
 * Free the given buffer.
 */
static inline void buffer_free(buffer_t* buffer)
{
    free(buffer->buf);
}

/**
 * Reset the given buffer to size.
 */
static inline void buffer_reset(buffer_t* buffer, off_t size)
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

/**
 * Expands the given buffer to include `n` new characters.
 * Returns a pointer to the end of the current buffer (with space), or NULL if no space could be made.
 * Note that this function does not modify `buffer->len` - if writing afterwards, manually increment.
 */
static inline char* buffer_ensure_space(buffer_t* buffer, off_t n)
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

/**
 * Append n bytes from *elems to the buffer.
 * Returns 0 if successful. If failed, -1 is returned and the buffer is unchanged.
 */
static inline int buffer_append(buffer_t* buffer, void* elems, off_t n)
{
    char* loc = buffer_ensure_space(buffer, n);
    if (!loc) {
        return -1;
    }
    memcpy(loc, elems, n);
    buffer->len += n;
    return 0;
}

#endif
