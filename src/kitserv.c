/* Kitserv
 * Copyright (C) 2023 Jmcgee01125
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "kitserv.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

#include "http.h"
#include "queue.h"
#include "socket.h"

#define MAX_EVENTS (64)

bool kitserv_silent_mode = false;

static int slots;
static pthread_barrier_t startup_barrier;

struct connection {
    struct connection* next_conn;
    struct kitserv_client client;
};

struct connection_container {
    pthread_mutex_t conn_lock;  // protects freelist and count, active connections should not be shared
    int freelist_count;
    struct connection* first_free_conn;  // NULL if no free connections
    struct connection* connections;      // all connections, should not be directly iterated
};

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
 * Initialize a connection container and all of its sub-connections.
 * Aborts on failure.
 */
static void connection_init(struct connection_container* container, int container_slots)
{
    int i, rc;

    if ((rc = pthread_mutex_init(&container->conn_lock, NULL))) {
        errno = rc;
        perror("connection_init (pthread_mutex_init)");
        abort();
    }

    container->connections = malloc(container_slots * sizeof(struct connection));
    if (!container->connections) {
        perror("connection_init (malloc)");
        abort();
    }

    container->freelist_count = container_slots;
    container->first_free_conn = container->connections;
    for (i = 0; i < container_slots - 1; i++) {
        container->connections[i].next_conn = &container->connections[i + 1];
    }

    for (i = 0; i < container_slots; i++) {
        if (kitserv_http_create_client_struct(&container->connections[i].client)) {
            perror("connection_init (http_create_client_struct)");
            abort();
        }
    }
}

/**
 * Allocate a connection struct using the given socket.
 * Returns the connection to be served, or NULL on error (i.e. no slots).
 */
static struct connection* connection_accept(struct connection_container* container, int socket)
{
    struct connection* conn;

    pthread_mutex_lock(&container->conn_lock);
    conn = container->first_free_conn;
    if (conn) {
        assert(container->freelist_count > 0);

        container->first_free_conn = conn->next_conn;
        container->freelist_count--;

        conn->client.sockfd = socket;

        // ensure that this connection is fresh
        // this doesn't ensure everything is reset, but is good enough to detect blatant mistakes
        assert(conn->client.req_headers_len == 0);
        assert(conn->client.resp_body.len == 0);
        assert(conn->client.ta.resp_fd == 0);
        assert(conn->client.ta.resp_fd == KITSERV_FD_DISABLE);  // since it is assumed 0 in http.c
        assert(conn->client.ta.state == 0);
        assert(conn->client.ta.parse_state == 0);
        assert(conn->client.ta.resp_status == 0);
    }
    pthread_mutex_unlock(&container->conn_lock);

    return conn;
}

/**
 * Shut down the given connection struct, marking it as vacant and adding it to the free list.
 */
static void connection_close(struct connection_container* container, struct connection* connection)
{
    kitserv_http_reset_client(&connection->client);
    pthread_mutex_lock(&container->conn_lock);
    connection->next_conn = container->first_free_conn;
    container->first_free_conn = connection;
    container->freelist_count++;
    pthread_mutex_unlock(&container->conn_lock);
}

/**
 * Serve the given connection as much as possible.
 * Returns 0 if the connection is still alive, -1 if it should be closed.
 */
static int connection_serve(struct connection* connection)
{
    struct kitserv_client* client = &connection->client;
    enum http_transaction_state* state = &client->ta.state;

    /*
     * Keep switching on the connection state to parse the request.
     * If 0 is returned, it means that either the connection has blocked or it's time for the next step.
     * Otherwise, an error occurred and it should be handled as appropriate.
     */

    while (1) {
        switch (*state) {
            case HTTP_STATE_READ:
                if (kitserv_http_recv_request(client)) {
                    if (client->ta.resp_status == HTTP_X_HANGUP) {
                        // don't bother trying to do anything else
                        return -1;
                    }
                    goto prep_response;
                } else if (*state == HTTP_STATE_READ) {
                    return 0;
                }
                /* fallthrough */
            case HTTP_STATE_SERVE:
                if (kitserv_http_serve_request(client)) {
                    if (client->ta.resp_status == HTTP_X_HANGUP) {
                        // don't bother trying to do anything else
                        return -1;
                    }
                    goto prep_response;
                } else if (*state == HTTP_STATE_SERVE) {
                    return 0;
                }
                /* fallthrough */
            case HTTP_STATE_PREPARE_RESPONSE:
prep_response:
                client->ta.state = HTTP_STATE_PREPARE_RESPONSE;
                if (kitserv_http_prepare_response(client)) {
                    return -1;
                } else if (*state == HTTP_STATE_PREPARE_RESPONSE) {
                    return 0;
                }
                /* fallthrough */
            case HTTP_STATE_SEND:
                if (kitserv_http_send_response(client)) {
                    return -1;
                } else if (*state == HTTP_STATE_SEND) {
                    return 0;
                }
                /* fallthrough */
            case HTTP_STATE_DONE:
                kitserv_http_finalize_transaction(client);
                continue;
            default:
                fprintf(stderr, "Unknown connection state: %d\n", *state);
                return -1;
        }
    }
}

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
    queue_event events[MAX_EVENTS];
    int nevents, i;

    connection_init(&self->conn_container, slots);
    self->queuefd = kitserv_queue_init();
    if (self->queuefd < 0) {
        perror("queue_init");
        abort();
    }

    pthread_barrier_wait(&startup_barrier);

    while (1) {
        nevents = kitserv_queue_wait(self->queuefd, events, MAX_EVENTS);
        if (nevents < 0) {
            if (!kitserv_silent_mode) {
                perror("queue_wait");
            }
            continue;
        }
        for (i = 0; i < nevents; i++) {
            conn = kitserv_queue_event_to_data(&events[i]);
            if (connection_serve(conn)) {
                // that transaction was the last one on this connection, so drop it
                kitserv_queue_remove(self->queuefd, conn->client.sockfd);
                kitserv_socket_close(conn->client.sockfd);  // ignore errors like ENOTCONN
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
        sockfd = kitserv_socket_accept(self->acceptfd);
        if (sockfd < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN && !kitserv_silent_mode) {
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
            if (!kitserv_silent_mode) {
                fprintf(stderr, "Target worker has no free slots!\n");
            }
            kitserv_socket_close(sockfd);
            // TODO: when this happens, we should tell workers to close extranneous connections (e.g. DoS prevention)
        } else {
            kitserv_queue_add(best_worker->queuefd, sockfd, new_conn, QUEUE_IN | QUEUE_OUT, false);
        }
    }

    return NULL;
}

void kitserv_server_start(struct kitserv_config* config)
{
    int v4sock = 0;
    int v6sock = 0;
    bool bind_ipv4, bind_ipv6;
    sigset_t sigset;
    int sig = 0;
    int i;
    struct worker* workers;
    struct accepter* accepters;

    kitserv_silent_mode = config->silent_mode;

    if (config->num_slots <= 0) {
        fprintf(stderr, "Invalid slot count: %d <= 0\n", config->num_slots);
        abort();
    }
    if (config->num_workers <= 0) {
        fprintf(stderr, "Invalid worker count: %d <= 0\n", config->num_workers);
        abort();
    }
    if (config->num_slots < config->num_workers) {
        fprintf(stderr, "Invalid slot/worker ratio: %d/%d\n", config->num_slots, config->num_workers);
        abort();
    }

    slots = config->num_slots / config->num_workers;  // share slots between workers

    kitserv_http_init(config->http_root_context, config->api_tree);

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

    workers = malloc(config->num_workers * sizeof(struct worker));
    if (!workers) {
        perror("malloc");
        abort();
    }

    // we might modify them for fallback, and the client may well have put this config in read-only data
    bind_ipv4 = config->bind_ipv4;
    bind_ipv6 = config->bind_ipv6;

    if (bind_ipv6) {
        v6sock = kitserv_socket_prepare(config->port_string, true, false);
        if (v6sock < 0) {
            perror("socket_prepare (ipv6)");
            if (errno == EAFNOSUPPORT) {
                fprintf(stderr, "\tNo IPv6 support found. Falling back to IPv4\n");
                bind_ipv6 = false;
                bind_ipv4 = true;
            } else {
                exit(1);
            }
        }
    }
    if (bind_ipv4) {
        v4sock = kitserv_socket_prepare(config->port_string, false, false);
        if (v4sock < 0) {
            perror("socket_prepare (ipv4)");
            if (errno == EADDRINUSE && bind_ipv6) {
                fprintf(stderr, "\tIf your system has dual binding enabled, suppress this by running with -6.\n");
                bind_ipv4 = false;
            } else {
                exit(1);
            }
        }
    }

    // slot 0 = ipv4, slot 1 = ipv6
    accepters = malloc(2 * sizeof(struct accepter));
    if (!accepters) {
        perror("malloc");
        abort();
    }

    //                                               workers               accept threads              self
    if (pthread_barrier_init(&startup_barrier, NULL, config->num_workers + !!bind_ipv4 + !!bind_ipv6 + 1)) {
        perror("pthread_barrier_init");
        abort();
    }
    for (i = 0; i < config->num_workers; i++) {
        if (pthread_create(&workers[i].tid, NULL, client_worker, &workers[i])) {
            perror("pthread_create");
            abort();
        }
    }
    if (bind_ipv4) {
        accepters[0].acceptfd = v4sock;
        accepters[0].workers_list = workers;
        accepters[0].num_workers = config->num_workers;
        if (pthread_create(&accepters[0].tid, NULL, accept_worker, &accepters[0])) {
            perror("pthread_create");
            abort();
        }
    }
    if (bind_ipv6) {
        accepters[1].acceptfd = v6sock;
        accepters[1].workers_list = workers;
        accepters[1].num_workers = config->num_workers;
        if (pthread_create(&accepters[1].tid, NULL, accept_worker, &accepters[1])) {
            perror("pthread_create");
            abort();
        }
    }
    pthread_barrier_wait(&startup_barrier);
    if (pthread_barrier_destroy(&startup_barrier)) {
        if (!kitserv_silent_mode) {
            perror("pthread_barrier_destroy");
        }
        // no abort, just warn
    }

    // TODO: proper signal handling and joining (and cleanup...)
    while (sigwait(&sigset, &sig)) {
        perror("sigwait");
        abort();
    }
    printf("Caught signal %d.\n", sig);
}
