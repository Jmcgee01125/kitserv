/* Part of FileKit, licensed under the GNU Affero GPL. */

#include <stdbool.h>

#include "api.h"
#include "auth.h"
#include "buffer.h"
#include "http.h"

/**
 * Parameters: %s name, %s value, %d max age
 */
static const char* COOKIE_FMT = "%s=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Strict";

static char* data_dir;
static char* cookie_name;
static int cookie_age;

static void api_login_get(struct http_client*);
static void api_login_post(struct http_client*);
static void api_logout_post(struct http_client*);
static void d(struct http_client*);

static struct http_api_entry tree_api_entries[3] = {{
                                                        .prefix = "login",
                                                        .prefix_length = 5,
                                                        .method = HTTP_GET,
                                                        .handler = api_login_get,
                                                        .finishes_path = true,
                                                    },
                                                    {
                                                        .prefix = "login",
                                                        .prefix_length = 5,
                                                        .method = HTTP_POST,
                                                        .handler = api_login_post,
                                                        .finishes_path = true,
                                                    },
                                                    {
                                                        .prefix = "logout",
                                                        .prefix_length = 6,
                                                        .method = HTTP_POST,
                                                        .handler = api_logout_post,
                                                        .finishes_path = true,
                                                    }};

static struct http_api_tree tree_api_branch = {
    .prefix = "api",
    .prefix_length = 3,
    .entries = tree_api_entries,
    .num_subtrees = 0,
    .num_entries = 3,
};

static struct http_api_entry tree_data_entry = {
    .prefix = "d",
    .prefix_length = 1,
    .method = HTTP_GET,
    .handler = d,
    .finishes_path = false,
};

static struct http_api_tree api_tree = {
    .subtrees = &tree_api_branch,
    .entries = &tree_data_entry,
    .num_subtrees = 1,
    .num_entries = 1,
};

struct http_api_tree* api_init(char* directory, char* cookie, int expiration_time)
{
    data_dir = directory;
    cookie_name = cookie;
    cookie_age = expiration_time;
    auth_init(expiration_time);
    return &api_tree;
}

static void api_login_get(struct http_client* client)
{
    if (client->ta.req_cookie) {
        buffer_appendf(&client->resp_body, "Logged in with cookie %s", client->ta.req_cookie);
    } else {
        buffer_append(&client->resp_body, "You are not logged in", sizeof("You are not logged in") - 1);
    }
    client->ta.resp_status = HTTP_200_OK;
}

static void api_login_post(struct http_client* client)
{
    http_header_add_set_cookie(client, COOKIE_FMT, cookie_name, "debug_token", cookie_age);
    client->ta.resp_status = HTTP_200_OK;
}

static void api_logout_post(struct http_client* client)
{
    http_header_add_set_cookie(client, COOKIE_FMT, cookie_name, "", cookie_age);
    client->ta.resp_status = HTTP_200_OK;
}

static void d(struct http_client* client)
{
    buffer_appendf(&client->resp_body, "Serving %s", client->ta.req_path);
    client->ta.resp_status = HTTP_200_OK;
}
