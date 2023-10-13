/* Part of FileKit, licensed under the GNU Affero GPL. */

#ifndef HTTP_H
#define HTTP_H

#include <inttypes.h>
#include <stdbool.h>

#include "buffer.h"

#define HTTP_BUFSZ (4096)

enum http_transaction_state {
    HTTP_STATE_READ = 0,
    HTTP_STATE_API,
    HTTP_STATE_PREPARE_RESPONSE,
    HTTP_STATE_SEND_START,
    HTTP_STATE_SEND_HEAD,
    HTTP_STATE_SEND_BODY,
    HTTP_STATE_DONE,
};

enum http_parse_state {
    HTTP_PS_NEW = 0,
    HTTP_PS_REQ_METHOD,
    HTTP_PS_REQ_PATH,
    HTTP_PS_REQ_VERSION,
    HTTP_PS_REQ_VERSION_LF,  // reading ver, unterminated CR
    HTTP_PS_REQ_HEAD,
    HTTP_PS_REQ_HEAD_LF,  // reading header, unterminated CR
};

enum http_method {
    HTTP_GET,
    HTTP_PUT,
    HTTP_HEAD,
    HTTP_POST,
    HTTP_DELETE,
};

enum http_version {
    HTTP_1_1 = 0,
    HTTP_1_0,
};

enum http_response_status {
    HTTP_X_RESP_STATUS_MISSING = 0,
    HTTP_X_HANGUP = 1,  // connection has closed, do not bother generating a response
    HTTP_200_OK = 200,
    HTTP_206_PARTIAL_CONTENT = 206,
    HTTP_304_NOT_MODIFIED = 304,
    HTTP_400_BAD_REQUEST = 400,
    HTTP_403_PERMISSION_DENIED = 403,
    HTTP_404_NOT_FOUND = 404,
    HTTP_405_METHOD_NOT_ALLOWED = 405,
    HTTP_408_REQUEST_TIMEOUT = 408,
    HTTP_413_CONTENT_TOO_LARGE = 413,
    HTTP_414_URI_TOO_LONG = 414,
    HTTP_416_RANGE_NOT_SATISFIABLE = 416,
    HTTP_431_REQUEST_HEADER_FIELDS_TOO_LARGE = 431,
    HTTP_500_INTERNAL_ERROR = 500,
    HTTP_501_NOT_IMPLEMENTED = 501,
    HTTP_503_SERVICE_UNAVAILABLE = 503,
    HTTP_505_VERSION_NOT_SUPPORTED = 505,
    HTTP_507_INSUFFICIENT_STORAGE = 507,
};

struct http_transaction {
    enum http_transaction_state state;
    enum http_parse_state parse_state;

    /* Request fields */
    enum http_method req_method;
    enum http_version req_version;
    int req_payload_pos;  // start position of unconsumed payload information
    int req_payload_len;  // number of bytes to available to read past req_payload_pos, MUST NOT imply wrapping
    off_t req_content_len;
    char* req_parse_blk;
    char* req_parse_iter;
    // for the following: NULL if not found, or null-terminated string inside req_headers
    char* req_path;
    char* req_query;
    char* req_cookie;
    char* req_mimetype;
    char* req_range;
    char* req_disposition;
    char* req_modified_since;

    /* Response fields */
    // pos - current position from which data has not yet been sent
    // len - amount of data to send from that buffer in total
    enum http_response_status resp_status;
    int resp_start_pos;
    int resp_start_len;
    int resp_headers_pos;
    int resp_headers_len;
    /**
     * if 0 - use client.resp_body and ignore client.ta.resp_body_end (in favor of client.resp_body's internal size)
     * else - send this fd, be sure to set client.ta.resp_fd_size and client.ta.resp_body_end
     * any nonzero value indicates that this fd is open - set to 0 when closing
     */
    int resp_fd;
    off_t resp_fd_size;   // total length of resp_fd, if set
    off_t resp_body_pos;  // send progress, initial value of range start, should always be set
    off_t resp_body_end;  // final offset, end of range or content length
    bool range_requested;
    bool hit_api;
};

struct http_client {
    /**
     * both req_headers and req_payload point to the same buffer
     * req_headers is the owner, and always points to the beginning of the space
     * req_payload points into this buffer at the position where the payload starts
     *      (the position immediately after the end of the headers)
     * neither "segment" may exceed `HTTP_BUFSZ` - they are essentially treated as separate but "overlapping" buffers
     */
    char* req_headers;  // persistent request header information
    char* req_payload;  // cycling receive buffer

    char* resp_start;    // response start buffer (HTTP_BUFSZ)
    char* resp_headers;  // response headers buffer (HTTP_BUFZ)
    buffer_t resp_body;  // response body buffer (at least HTTP_BUFZ, may grow)

    struct http_transaction ta;
    /**
     * req_headers_len is stored here because it may persist across transactions
     * note that it is not always up-to-date: it is only up to date between request start and the beginning of the last
     * call to parse headers (as there is no need to update it after that, until a new transaction begins)
     */
    int req_headers_len;  // stored here because it may persist across transaction

    int sockfd;
};

struct http_api_entry {
    char* prefix;  // unique prefix of the api endpoint, e.g. `login`
    int prefix_length;
    enum http_method method;  // http method to allow, GET implies HEAD
    /**
     * Function to receive the client so that the api endpoint code can do processing.
     * The client's `req_*` fields will be set before calling, but the payload may only be partially read.
     * Write custom headers, if necessary, using `http_add_header*(...)`. Do not set content-length or server.
     * Write response data, if necessary, into `client->resp_body` (see `buffer_append(...)`).
     * Write custom file to send, if necessary, into `client->ta.resp_fd`. Leave 0 otherwise. See that field's comment.
     * Set `client->ta.resp_status` before returning to indicate the result of processing.
     * Headers will be wiped on error, but the body will still be sent.
     */
    void (*handler)(struct http_client* client);
};

/**
 * API endpoint tree.
 * When parsing, nodes are considered in the following order:
 * The path is split on the first /.
 * The entries of the tree are iterated.
 * If any entry's method and prefix matches, the handler is called and iteration is finished.
 * The subtrees of the tree are iterated.
 * If the subtree's prefix matches, the tree is recursed for the next segment of the path.
 * If at any point there are no more path components or none of the entries match, API matching is canceled.
 */
struct http_api_tree {
    char* prefix;  // prefix of this tree, ignored for entry point
    int prefix_length;
    struct http_api_tree* subtrees;
    struct http_api_entry* entries;
    int num_subtrees;
    int num_entries;
};

/**
 * Initalize HTTP system.
 * Pass NULL to disable fallbacks, auth, and/or API. Do not pass NULL for others.
 */
void http_init(char* servername, char* http_directory, char* fallback_path, char* root_fallback_path,
               bool use_http_append_fallback, char* auth_cookie_name, struct http_api_tree* http_api_list);

/**
 * Allocate the internal structures of a client and its associated transaction.
 * Return 0 on success, -1 on failure.
 */
int http_create_client_struct(struct http_client*);

/*
 * Reset a client to serve a new transaction on the same connection.
 * Some of the data for that transaction may have been read, this preserves it.
 */
void http_finalize_transaction(struct http_client*);

/**
 * Reset a client that will not be used again with the same connection.
 */
void http_reset_client(struct http_client*);

/**
 * Add a general formatted header to a given client's current transaction.
 * Returns 0 on success, -1 on failure (i.e. if the header does not fit).
 */
int http_header_add(struct http_client*, char* key, char* fmt, ...);

/**
 * The following functions add specific headers to the given client's current transaction.
 * Returns 0 on success, -1 on failure (i.e. if the header does not fit).
 */
int http_header_add_content_type(struct http_client*, char* mime);
int http_header_add_content_type_guess(struct http_client*, char* extension);
int http_header_add_set_cookie(struct http_client*, char* fmt, ...);
int http_header_add_last_modified(struct http_client*, time_t time);

/**
 * The following functions process a request.
 *
 * Returns -1 on error, or 0 if data was successfully read (even if partially).
 * On success, check client->ta.state for if state has advanced or it was blocked.
 * On error, skip to http_prepare_response and continue (ignore state) or, if past, close the connection.
 */
int http_recv_request(struct http_client* client);
int http_serve_api(struct http_client* client);
int http_prepare_response(struct http_client* client);
int http_send_resp_start(struct http_client* client);
int http_send_resp_head(struct http_client* client);
int http_send_resp_body(struct http_client* client);

#endif