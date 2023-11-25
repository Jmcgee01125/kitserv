/* Part of FileKit, licensed under the GNU Affero GPL. */

#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>

#include "http.h"

/**
 * Start the server on the given port, binding it and creating workers.
 * Expects that the HTTP system has been initialized.
 * Returns only once the server has shut down.
 */
void server_start(char* port_string, bool bind_ipv4, bool bind_ipv6, int num_workers, int num_slots);

#endif
