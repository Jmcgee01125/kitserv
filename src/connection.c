/* Part of FileKit, licensed under the GNU Affero GPL. */

#include "connection.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "http.h"
#include "socket.h"

void connection_init(struct connection_container* container, int slots)
{
    int i, rc;

    if ((rc = pthread_mutex_init(&container->conn_lock, NULL))) {
        errno = rc;
        perror("connection_init (pthread_mutex_init)");
        abort();
    }

    container->connections = malloc(slots * sizeof(struct connection));
    if (!container->connections) {
        perror("connection_init (malloc)");
        abort();
    }

    container->freelist_count = slots;
    container->first_free_conn = container->connections;
    for (i = 0; i < slots - 1; i++) {
        container->connections[i].next_conn = &container->connections[i + 1];
    }

    for (i = 0; i < slots; i++) {
        if (http_create_client_struct(&container->connections[i].client)) {
            perror("connection_init (http_create_client_struct)");
            abort();
        }
    }
}

struct connection* connection_accept(struct connection_container* container, int socket)
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
        assert(conn->client.ta.state == 0);
        assert(conn->client.ta.parse_state == 0);
        assert(conn->client.ta.resp_status == 0);
    }
    pthread_mutex_unlock(&container->conn_lock);

    return conn;
}

void connection_close(struct connection_container* container, struct connection* connection)
{
    http_reset_client(&connection->client);
    pthread_mutex_lock(&container->conn_lock);
    connection->next_conn = container->first_free_conn;
    container->first_free_conn = connection;
    container->freelist_count++;
    pthread_mutex_unlock(&container->conn_lock);
}

int connection_serve(struct connection* connection)
{
    struct http_client* client = &connection->client;
    enum http_transaction_state* state = &client->ta.state;

    /*
     * Keep switching on the connection state to parse the request.
     * If 0 is returned, it means that either the connection has blocked or it's time for the next step.
     * Otherwise, an error occurred and it should be handled as appropriate.
     */

    while (1) {
        switch (*state) {
            case HTTP_STATE_READ:
                if (http_recv_request(client)) {
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
                if (http_serve_request(client)) {
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
                if (http_prepare_response(client)) {
                    return -1;
                } else if (*state == HTTP_STATE_PREPARE_RESPONSE) {
                    return 0;
                }
                /* fallthrough */
            case HTTP_STATE_SEND:
                if (http_send_response(client)) {
                    return -1;
                } else if (*state == HTTP_STATE_SEND) {
                    return 0;
                }
                /* fallthrough */
            case HTTP_STATE_DONE:
                http_finalize_transaction(client);
                continue;
            default:
                fprintf(stderr, "Unknown connection state: %d\n", *state);
                return -1;
        }
    }
}
