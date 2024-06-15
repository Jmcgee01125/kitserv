/* Part of Kitserv, licensed under the GNU Affero GPL. */

#ifndef KITSERV_QUEUE_H
#define KITSERV_QUEUE_H

#include <stddef.h>

#ifdef __linux__
#include <sys/epoll.h>
typedef struct epoll_event queue_event;
#else
#endif

typedef enum {
    // must not bitwise overlap
    QUEUE_IN = 1,
    QUEUE_OUT = 2,
} queue_wait_cond;

/**
 * Initialize a queue and return the created file descriptor.
 * Returns fd on success, -1 on error.
 */
int kitserv_queue_init(void);

/**
 * Wait for up to n events on the given queue. Returns the number of actual events, written to out_events.
 * Returns nevents on success, -1 on error.
 */
ssize_t kitserv_queue_wait(int qfd, queue_event* out_events, int n);

/**
 * Add a new file descriptor and associated data to the queue with the given condition.
 * Returns 0 on success, -1 on error.
 */
int kitserv_queue_add(int qfd, int fd, void* data, queue_wait_cond, int shared);

/**
 * Re-arm a given file descriptor with the target queue type.
 * Returns 0 on success, -1 on error.
 */
int kitserv_queue_rearm(int qfd, int fd, void* data, queue_wait_cond, int shared);

/**
 * Remove a given file descriptor from the given queue.
 * Returns 0 on success, -1 on error.
 */
int kitserv_queue_remove(int qfd, int fd);

/**
 * Returns the data pointer associated with an event.
 */
static inline void* kitserv_queue_event_to_data(const queue_event* event)
{
#ifdef __linux__
    return event->data.ptr;
#else
#endif
}

#endif
