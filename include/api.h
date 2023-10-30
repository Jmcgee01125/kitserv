#ifndef API_H
#define API_H

#include "http.h"

/**
 * Initialize the API for the target API.
 * The API is indicated to run in the associated directory.
 * Returns the API tree to be parsed by the HTTP system.
 */
struct http_api_tree* api_init(char* directory);

#endif
