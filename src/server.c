/* Part of FileKit, licensed under the GNU Affero GPL. */

#include "server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>

#include "connection.h"
#include "http.h"
#include "queue.h"
#include "socket.h"

#define CONN_FLAG_MAX ((struct connection*)0x10)
#define CONN_ACCEPT_4 ((struct connection*)0x4)
#define CONN_ACCEPT_6 ((struct connection*)0x6)

static int slots;

struct worker {
    pthread_t tid;
    struct connection_container conn_container;
    int queuefd;
    int acceptfd_ipv4;
    int acceptfd_ipv6;
};

static void* client_worker(void* data)
{
    struct worker* self = (struct worker*)data;
    struct connection* conn;
    queue_event events[16];
    int nevents, i;

    connection_init(&self->conn_container, slots);
    self->queuefd = queue_init();
    if (self->queuefd < 0) {
        perror("queue_init");
        abort();
    }
    // we hijack a couple of invalid addresses in the zero page to map to acceptfds
    if (self->acceptfd_ipv4) {
        queue_add(self->queuefd, self->acceptfd_ipv4, CONN_ACCEPT_4, QUEUE_IN, true);
    }
    if (self->acceptfd_ipv6) {
        queue_add(self->queuefd, self->acceptfd_ipv6, CONN_ACCEPT_6, QUEUE_IN, true);
    }

    while (1) {
        nevents = queue_wait(self->queuefd, events, 16);
        if (nevents < 0) {
            perror("queue_wait");
            continue;
        }
        for (i = 0; i < nevents; i++) {
            conn = queue_event_to_data(&events[i]);
            if (conn < CONN_FLAG_MAX) {
                if (conn == CONN_ACCEPT_4) {
                    conn = connection_accept(&self->conn_container, self->acceptfd_ipv4);
                } else if (conn == CONN_ACCEPT_6) {
                    conn = connection_accept(&self->conn_container, self->acceptfd_ipv6);
                } else {
                    fprintf(stderr, "Unknown queue event: %p\n", (void*)conn);
                    continue;
                }
                if (!conn) {
                    continue;
                }
                queue_add(self->queuefd, conn->client.sockfd, conn, QUEUE_IN | QUEUE_OUT, false);
            }

            if (connection_serve(conn)) {
                // that transaction was the last one on this connection, so drop it
                queue_remove(self->queuefd, conn->client.sockfd);
                connection_close(&self->conn_container, conn);
            }
        }
    }

    return NULL;
}

void start_server(char* port_string, bool bind_ipv4, bool bind_ipv6, int num_workers, int num_slots)
{
    int v4sock = 0;
    int v6sock = 0;
    sigset_t sigset;
    int sig = 0;
    int i;
    struct worker* workers;

    slots = num_slots;

    // block INT and TERM so that helper threads don't receive them
    if (sigemptyset(&sigset) || sigaddset(&sigset, SIGINT) || sigaddset(&sigset, SIGTERM)) {
        perror("sigset");
        abort();
    }
    if (sigprocmask(SIG_BLOCK, &sigset, NULL)) {
        perror("sigprocmask");
        abort();
    }
    // ignore PIPE in case a client closes on us during a transaction
    if (signal(SIGPIPE, SIG_IGN)) {
        perror("signal");
        abort();
    }

    workers = malloc(num_workers * sizeof(struct worker));
    if (!workers) {
        perror("malloc");
        abort();
    }

    if (bind_ipv4) {
attempt_bind_ipv4:
        v4sock = socket_prepare(port_string, false);
        if (v4sock < 0) {
            perror("socket_prepare (ipv4)");
            abort();
        }
    }
    if (bind_ipv6) {
        v6sock = socket_prepare(port_string, true);
        if (v6sock < 0) {
            perror("socket_prepare (ipv6)");
            if (errno == EADDRINUSE && bind_ipv4) {
                fprintf(stderr,
                        "\tDual binding may cause this issue.\n"
                        "\tIf your system has dual binding enabled, try running with -6.\n");
            } else if (errno == EAFNOSUPPORT) {
                fprintf(stderr, "\tNo IPv6 found. Falling back to IPv4\n");
                bind_ipv6 = false;
                v6sock = 0;
                goto attempt_bind_ipv4;
            }
            abort();
        }
    }

    for (i = 0; i < num_workers; i++) {
        workers[i].acceptfd_ipv4 = v4sock;
        workers[i].acceptfd_ipv6 = v6sock;

        if (pthread_create(&workers[i].tid, NULL, client_worker, &workers[i])) {
            perror("pthread_create");
            abort();
        }
    }

    // TODO: rather than waiting for a signal here, place a signalfd in the queue of each of the
    //       worker threads - they will notice it and shut down, letting us collect them
    while (sigwait(&sigset, &sig)) {
        perror("sigwait");
        return;
    }
    printf("Caught signal %d.\n", sig);
    exit(0);
}
