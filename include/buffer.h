/* Part of FileKit, licensed under the GNU Affero GPL. */

#ifndef BUFFER_H
#define BUFFER_H

#include <stdarg.h>
#include <stdio.h>
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
int buffer_init(buffer_t* buffer, off_t initial_size);

/**
 * Free the given buffer.
 */
void buffer_free(buffer_t* buffer);

/**
 * Reset the given buffer to size.
 */
void buffer_reset(buffer_t* buffer, off_t size);

/**
 * Append n bytes from *elems to the buffer.
 * Returns 0 if successful. If failed, -1 is returned and the buffer is unchanged.
 */
int buffer_append(buffer_t* buffer, void* elems, off_t n);

/**
 * Append a format string to the buffer, expanding if necessary.
 * Returns 0 if successful. If failed, -1 is returned and the buffer is unchanged, but may be expanded.
 */
int buffer_appendf(buffer_t* buffer, char* fmt, ...);

#endif
