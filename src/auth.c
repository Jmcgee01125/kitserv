/* Part of FileKit, licensed under the GNU Affero GPL. */

#define _XOPEN_SOURCE 500
#define _POSIX_C_SOURCE 200112L

#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTH_SECRET_ENVVAR_NAME ("FILEKIT_SECRET")

static char* auth_secret;
static int auth_exp_time;

int auth_init(int exp_time)
{
    char* temp_auth_secret = getenv(AUTH_SECRET_ENVVAR_NAME);
    auth_exp_time = exp_time;
    auth_secret = NULL;
    if (!temp_auth_secret) {
        fprintf(stderr, "Could not find %s variable. Authentication will be disabled.\n", AUTH_SECRET_ENVVAR_NAME);
        return -1;
    } else {
        auth_secret = strdup(temp_auth_secret);
        if (!auth_secret) {
            fprintf(stderr, "Could not duplicate auth secret. Authentication will be disabled.\n");
            return -1;
        }
        if (unsetenv(AUTH_SECRET_ENVVAR_NAME)) {
            fprintf(stderr, "Warning: Failed to unset %s variable.\n", AUTH_SECRET_ENVVAR_NAME);
        }
    }
    return 0;
}
