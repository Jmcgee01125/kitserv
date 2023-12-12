/* Part of Kitserv, licensed under the GNU Affero GPL. */

#ifndef KITSERV_BUFFER_H
#define KITSERV_BUFFER_H

#include <stdarg.h>
#include <stdlib.h>

typedef struct {
    char* buf;
    off_t len;
    off_t max;
} buffer_t;

/**
 * Initialize the given buffer to the initial size.
 * Returns 0 if successful, -1 on error.
 */
int kitserv_buffer_init(buffer_t* buffer, off_t initial_size);

/**
 * Free the given buffer.
 */
void kitserv_buffer_free(buffer_t* buffer);

/**
 * Reset the given buffer to size.
 */
void kitserv_buffer_reset(buffer_t* buffer, off_t size);

/**
 * Append n bytes from *elems to the buffer.
 * Returns 0 if successful. If failed, -1 is returned and the buffer is unchanged.
 */
int kitserv_buffer_append(buffer_t* buffer, const void* elems, off_t n);

/**
 * Append va_list according to fmt to the buffer.
 * Returns 0 if successful. If failed, -1 is returned and the buffer is unchanged, but may be expanded.
 */
int kitserv_buffer_appendva(buffer_t* buffer, const char* fmt, va_list* ap);

/**
 * Append a format string to the buffer, expanding if necessary.
 * Returns 0 if successful. If failed, -1 is returned and the buffer is unchanged, but may be expanded.
 */
int kitserv_buffer_appendf(buffer_t* buffer, const char* fmt, ...);

#endif
