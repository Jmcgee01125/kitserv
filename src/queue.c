/* Part of Kitserv, licensed under the GNU Affero GPL. */

#include "queue.h"

#include <stddef.h>
#include <stdio.h>

#ifdef __linux__
#include <sys/epoll.h>
#else
#error No non-Linux setup has been created!
// TODO: make a kqueue wrapper as well
#endif

int kitserv_queue_init()
{
    int qfd;
#ifdef __linux__
    if ((qfd = epoll_create1(0)) < 0) {
        return -1;
    }
#else
#endif
    return qfd;
}

ssize_t kitserv_queue_wait(int qfd, queue_event* out_events, int n)
{
    ssize_t nready;
#ifdef __linux__
    if ((nready = epoll_wait(qfd, out_events, n, -1)) < 0) {
        return -1;
    }
#else
#endif
    return nready;
}

int kitserv_queue_add(int qfd, int fd, void* data, queue_wait_cond type, int shared)
{
#ifdef __linux__
    struct epoll_event e;
    if (shared) {
        e.events = EPOLLEXCLUSIVE;
    } else {
        e.events = EPOLLET;
    }
    if (type & QUEUE_IN) {
        e.events |= EPOLLIN;
    }
    if (type & QUEUE_OUT) {
        e.events |= EPOLLOUT;
    }
    e.data.ptr = data;
    if (epoll_ctl(qfd, EPOLL_CTL_ADD, fd, &e) < 0) {
        return -1;
    }
#else
#endif
    return 0;
}

int kitserv_queue_rearm(int qfd, int fd, void* data, queue_wait_cond type, int shared)
{
#ifdef __linux__
    struct epoll_event e;
    if (shared) {
        e.events = EPOLLEXCLUSIVE;
    } else {
        e.events = EPOLLET;
    }
    if (type & QUEUE_IN) {
        e.events |= EPOLLIN;
    }
    if (type & QUEUE_OUT) {
        e.events |= EPOLLOUT;
    }
    e.data.ptr = data;
    if (epoll_ctl(qfd, EPOLL_CTL_MOD, fd, &e) < 0) {
        return -1;
    }
#else
#endif
    return 0;
}

int kitserv_queue_remove(int qfd, int fd)
{
#ifdef __linux__
    struct epoll_event e;
    if (epoll_ctl(qfd, EPOLL_CTL_DEL, fd, &e) < 0) {
        return -1;
    }
#else
#endif
    return 0;
}
