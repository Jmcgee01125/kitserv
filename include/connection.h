/* Part of FileKit, licensed under the GNU Affero GPL. */

#ifndef CONNECTION_H
#define CONNECTION_H

#include "http.h"

struct connection {
    struct connection* next_free_conn;  // if this connection is free, pointer to the next valid unused connection
    struct http_client client;
};

struct connection_container {
    struct connection* first_free_conn;  // NULL if full
    struct connection* connections;
};

/*
 * Initialize a connection container and all of its sub-connections.
 * Aborts on failure.
 */
void connection_init(struct connection_container*, int slots);

/*
 * Accept a connection on the given socket.
 * Returns the connection to be served, or NULL on error.
 */
struct connection* connection_accept(struct connection_container*, int socket);

/*
 * Shut down the given connection, marking it as vacant and adding it to the free list.
 */
void connection_close(struct connection_container*, struct connection*);

/*
 * Serve the given connection as much as possible.
 * Returns 0 if the connection is still alive, -1 if it should be closed.
 */
int connection_serve(struct connection*);

#endif
