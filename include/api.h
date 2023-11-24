/* Part of FileKit, licensed under the GNU Affero GPL. */

#ifndef API_H
#define API_H

#include "http.h"

/**
 * Initialize the API for the target API.
 * The API is indicated to run in the associated directory.
 * Gives API information in out parameters.
 */
void api_init(struct http_api_tree** out_api_tree, char** out_cookie_name, char* directory, int expiration_time);

#endif
