/* Part of FileKit, licensed under the GNU Affero GPL. */

#ifndef SOCKET_H
#define SOCKET_H

#include <stdbool.h>

/**
 * Set up a listen socket on the given port. Bind IPv6 if indicated.
 * Returns socket fd on success, or -1 on error.
 */
int socket_prepare(const char* port_string, bool use_ipv6);

/**
 * Accept a new client on the given socket.
 * Returns the fd of the accepted client, or -1 on error.
 */
int socket_accept(int sockfd);

/**
 * Gracefully shuts down the given socket. Returns 0 on success, -1 on error.
 */
int socket_close(int sockfd);

#endif
