/* Part of Kitserv, licensed under the GNU Affero GPL. */

#ifndef KITSERV_SOCKET_H
#define KITSERV_SOCKET_H

#include <stdbool.h>

/**
 * Set up a listen socket on the given port. Bind IPv6 and enable nonblocking accepts if indicated.
 * Returns socket fd on success, or -1 on error.
 */
int kitserv_socket_prepare(const char* port_string, bool use_ipv6, bool nonblocking_accepts);

/**
 * Accept a new client on the given socket.
 * Returns the fd of the accepted client, or -1 on error.
 */
int kitserv_socket_accept(int sockfd);

/**
 * Gracefully shuts down the given socket.
 * Returns 0 on success, -1 on error.
 */
int kitserv_socket_close(int sockfd);

#endif
