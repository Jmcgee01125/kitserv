/* Part of FileKit, licensed under the GNU Affero GPL. */

#include "server.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>

#include "connection.h"
#include "http.h"
#include "queue.h"
#include "socket.h"

static int slots;
static pthread_barrier_t startup_barrier;

struct worker {
    pthread_t tid;
    struct connection_container conn_container;
    int queuefd;
};

struct accepter {
    pthread_t tid;
    struct worker* workers_list;
    int num_workers;
    int acceptfd;
};

/**
 * Score a given worker - lower = worse (prioritize high scores for new connections)
 * Always >= 0
 */
static inline int score_worker(struct worker* worker)
{
    int score;

    pthread_mutex_lock(&worker->conn_container.conn_lock);
    // easy scoring system is just the number of free slots - doesn't spread *load*, but approximately good enough
    score = worker->conn_container.freelist_count;
    pthread_mutex_unlock(&worker->conn_container.conn_lock);

    assert(score >= 0);
    return score;
}

static void* client_worker(void* data)
{
    struct worker* self = (struct worker*)data;
    struct connection* conn;
    queue_event events[64];
    int nevents, i;

    connection_init(&self->conn_container, slots);
    self->queuefd = queue_init();
    if (self->queuefd < 0) {
        perror("queue_init");
        abort();
    }

    pthread_barrier_wait(&startup_barrier);

    while (1) {
        nevents = queue_wait(self->queuefd, events, 16);
        if (nevents < 0) {
            perror("queue_wait");
            continue;
        }
        for (i = 0; i < nevents; i++) {
            conn = queue_event_to_data(&events[i]);
            if (connection_serve(conn)) {
                // that transaction was the last one on this connection, so drop it
                queue_remove(self->queuefd, conn->client.sockfd);
                socket_close(conn->client.sockfd);  // ignore errors like ENOTCONN
                connection_close(&self->conn_container, conn);
            }
        }
    }

    return NULL;
}

static void* accept_worker(void* data)
{
    struct accepter* self = (struct accepter*)data;
    struct connection* new_conn;
    struct worker* best_worker;
    int sockfd, curr_score, best_score, i;

    pthread_barrier_wait(&startup_barrier);

    while (1) {
        sockfd = socket_accept(self->acceptfd);
        if (sockfd < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                perror("accept");
            }
            continue;
        }

        // look through all workers to pick best one for this new connection
        best_worker = NULL;
        best_score = -1;
        for (i = 0; i < self->num_workers; i++) {
            curr_score = score_worker(&self->workers_list[i]);
            if (curr_score > best_score) {
                best_worker = &self->workers_list[i];
                best_score = curr_score;
            }
        }

        // give the new connection to that worker
        new_conn = connection_accept(&best_worker->conn_container, sockfd);
        if (!new_conn) {
            fprintf(stderr, "Target worker has no free slots!\n");
            socket_close(sockfd);
            // TODO: when this happens, we should tell workers to close extranneous connections (e.g. DoS prevention)
        }
        queue_add(best_worker->queuefd, sockfd, new_conn, QUEUE_IN | QUEUE_OUT, false);
    }

    return NULL;
}

void server_start(char* port_string, bool bind_ipv4, bool bind_ipv6, int num_workers, int num_slots)
{
    int v4sock = 0;
    int v6sock = 0;
    sigset_t sigset;
    int sig = 0;
    int i;
    struct worker* workers;
    struct accepter* accepters;

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
        v4sock = socket_prepare(port_string, false, false);
        if (v4sock < 0) {
            perror("socket_prepare (ipv4)");
            abort();
        }
    }
    if (bind_ipv6) {
        v6sock = socket_prepare(port_string, true, false);
        if (v6sock < 0) {
            perror("socket_prepare (ipv6)");
            if (errno == EADDRINUSE && bind_ipv4) {
                fprintf(stderr,
                        "\tDual binding may cause this issue.\n"
                        "\tIf your system has dual binding enabled, try running with -6.\n");
            } else if (errno == EAFNOSUPPORT) {
                fprintf(stderr, "\tNo IPv6 support found. Falling back to IPv4\n");
                bind_ipv6 = false;
                bind_ipv4 = true;
                v6sock = 0;
                goto attempt_bind_ipv4;
            }
            abort();
        }
    }

    // slot 0 = ipv4, slot 1 = ipv6
    accepters = malloc(2 * sizeof(struct accepter));
    if (!accepters) {
        perror("malloc");
        abort();
    }

    //                                               workers       accept threads              self
    if (pthread_barrier_init(&startup_barrier, NULL, num_workers + !!bind_ipv4 + !!bind_ipv6 + 1)) {
        perror("pthread_barrier_init");
        abort();
    }
    for (i = 0; i < num_workers; i++) {
        if (pthread_create(&workers[i].tid, NULL, client_worker, &workers[i])) {
            perror("pthread_create");
            abort();
        }
    }
    if (bind_ipv4) {
        accepters[0].acceptfd = v4sock;
        accepters[0].workers_list = workers;
        accepters[0].num_workers = num_workers;
        if (pthread_create(&accepters[0].tid, NULL, accept_worker, &accepters[0])) {
            perror("pthread_create");
            abort();
        }
    }
    if (bind_ipv6) {
        accepters[1].acceptfd = v6sock;
        accepters[1].workers_list = workers;
        accepters[1].num_workers = num_workers;
        if (pthread_create(&accepters[1].tid, NULL, accept_worker, &accepters[1])) {
            perror("pthread_create");
            abort();
        }
    }
    pthread_barrier_wait(&startup_barrier);
    if (pthread_barrier_destroy(&startup_barrier)) {
        perror("pthread_barrier_destroy");
        // no abort, just warn
    }

    // TODO: proper signal handling and joining
    while (sigwait(&sigset, &sig)) {
        perror("sigwait");
        abort();
    }
    printf("Caught signal %d.\n", sig);
}
