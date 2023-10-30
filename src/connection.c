/* Part of FileKit, licensed under the GNU Affero GPL. */

#include "connection.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "http.h"
#include "socket.h"

void connection_init(struct connection_container* container, int slots)
{
    int i;

    container->connections = malloc(slots * sizeof(struct connection));
    if (!container->connections) {
        perror("connection_init");
        abort();
    }

    container->first_free_conn = container->connections;
    for (i = 0; i < slots - 1; i++) {
        container->connections[i].next_free_conn = &container->connections[i + 1];
    }

    for (i = 0; i < slots; i++) {
        if (http_create_client_struct(&container->connections[i].client)) {
            perror("connection_init");
            abort();
        }
    }
}

struct connection* connection_accept(struct connection_container* container, int socket)
{
    int acceptfd;
    struct connection* conn;

    acceptfd = socket_accept(socket);
    if (acceptfd < 0) {
        perror("accept");
        return NULL;
    }

    conn = container->first_free_conn;
    if (conn) {
        container->first_free_conn = conn->next_free_conn;
    } else {
        fprintf(stderr, "No connection slot for new client.\n");
        // TODO: run an algorithm to determine who of our slots we should drop
        //       remember to log that we dropped someone, including who
        //       also, we should not close connections unless the client sends a hangup - instead, just shutdown the
        //       connection and wait (make them top priority for this algorithm)
        //       would need to remove it from queue as well, but:
        //          1) we don't have the queuefd here
        //          2) does that break the nevents loop or client worker logic?
        socket_close(acceptfd);
        return NULL;
    }

    conn->client.sockfd = acceptfd;

    // ensure that this connection is fresh
    // this doesn't ensure everything is reset, but is good enough to detect blatant mistakes
    assert(conn->client.req_headers_len == 0);
    assert(conn->client.resp_body.len == 0);
    assert(conn->client.ta.state == 0);
    assert(conn->client.ta.parse_state == 0);
    assert(conn->client.ta.resp_status == 0);

    return conn;
}

void connection_close(struct connection_container* container, struct connection* connection)
{
    socket_close(connection->client.sockfd);  // ignore errors like ENOTCONN
    http_reset_client(&connection->client);
    connection->next_free_conn = container->first_free_conn;
    container->first_free_conn = connection;
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
                    goto prep_response;
                } else if (*state == HTTP_STATE_READ) {
                    return 0;
                }
                /* fallthrough */
            case HTTP_STATE_API:
                if (http_serve_api(client)) {
                    goto prep_response;
                } else if (*state == HTTP_STATE_API) {
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
            case HTTP_STATE_SEND_START:
                if (http_send_resp_start(client)) {
                    return -1;
                } else if (*state == HTTP_STATE_SEND_START) {
                    return 0;
                }
                /* fallthrough */
            case HTTP_STATE_SEND_HEAD:
                if (http_send_resp_head(client)) {
                    return -1;
                } else if (*state == HTTP_STATE_SEND_HEAD) {
                    return 0;
                }
                /* fallthrough */
            case HTTP_STATE_SEND_BODY:
                if (http_send_resp_body(client)) {
                    return -1;
                } else if (*state == HTTP_STATE_SEND_BODY) {
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
