#ifndef KITSERV_H
#define KITSERV_H

#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

extern bool kitserv_silent_mode;  // internal use

struct kitserv_config;
struct kitserv_request_context;

struct kitserv_client;  // not defined here - use public functions to interact

/**
 * Extensible API tree.
 * During request parsing, the nodes in this tree are parsed to check for an API endpoint match.
 * If one is found, the function for that entry is called to service the request.
 *
 * Per request:
 * The path is split on /.
 * Entries of the current tree are iterated. If the prefix and method match, the handler is called.
 * If no match, the subtrees of the current tree are iterated. If a match is found, iteration recurses into it.
 * If nothing matches, then either return a 405 response (if any prefixes matched) or serve as a regular static file.
 *
 * Note that the request's payload may not be fully read. In particular, be prepared to return from the handler if
 * a read attempt fails. Kitserv offers a void* to remember parsing context if necessary.
 */
struct kitserv_api_tree;
struct kitserv_api_entry;
typedef void (*kitserv_api_handler_t)(struct kitserv_client* client, void* state);

/**
 * HTTP methods supported by Kitserv
 * Can be used solo or as a bit flag
 */
enum kitserv_http_method {
    HTTP_GET = 1,
    HTTP_PUT = 2,
    HTTP_HEAD = 4 | HTTP_GET,  // intentional overlap with GET, to support checking both using `method & GET`
    HTTP_POST = 8,
    HTTP_DELETE = 16,
};

/**
 * Response statuses supported by Kitserv
 */
enum kitserv_http_response_status {
    HTTP_X_RESP_STATUS_UNSET = 0,
    HTTP_X_HANGUP = 1,  // connection has closed, do not bother generating a response
    HTTP_200_OK = 200,
    HTTP_206_PARTIAL_CONTENT = 206,
    HTTP_304_NOT_MODIFIED = 304,
    HTTP_400_BAD_REQUEST = 400,
    HTTP_403_PERMISSION_DENIED = 403,
    HTTP_404_NOT_FOUND = 404,
    HTTP_405_METHOD_NOT_ALLOWED = 405,  // for internal use - use `kitserv_api_entry.method` to enforce methods
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

struct kitserv_request_context {
    char* root;                     // root directory to serve files from
    char* root_fallback;            // null to disable, fallback on '/'
    char* fallback;                 // null to disable, fallback on any 404 error as an exact path from root
    bool use_http_append_fallback;  // try to append .html on failure (/public -> /public.html)
};

struct kitserv_api_entry {
    char* prefix;  // prefix of the api endpoint, e.g. "login" - no '/' characters
    int prefix_length;
    enum kitserv_http_method method;  // GET implies HEAD, do not set a separate HEAD endpoint
    kitserv_api_handler_t handler;    // function to receive client for API processing
    bool finishes_path;               // if true, do not allow any extra path components (ignore if it does)
};

struct kitserv_api_tree {
    char* prefix;  // prefix of this tree, ignored for entry point
    int prefix_length;
    struct kitserv_api_tree* subtrees;
    struct kitserv_api_entry* entries;
    int num_subtrees;
    int num_entries;
};

struct kitserv_config {
    char* port_string;
    int num_workers;
    int num_slots;
    bool bind_ipv4;
    bool bind_ipv6;
    bool silent_mode;  // disable non-catastrophic error output and logging
    struct kitserv_request_context* http_root_context;
    struct kitserv_api_tree* api_tree;  // nullable to disable API
};

/**
 * Starts the server with the given config.
 * Aborts on failure.
 * Returns only once the server has shut down.
 * Currently does not clean up memory or threads. Treat returning as a signal to exit.
 */
void kitserv_server_start(struct kitserv_config*);

/**
 * Add a formatted header to the given client's current transaction.
 * Returns 0 on success, -1 on failure (i.e. if the header does not fit).
 */
int kitserv_http_header_add(struct kitserv_client*, const char* key, const char* fmt, ...);

/**
 * Add a content-type header to the given client's current transaction.
 * Returns 0 on success, -1 on failure (i.e. if the header does not fit).
 */
int kitserv_http_header_add_content_type(struct kitserv_client*, const char* mime);

/**
 * Add a content-type header to the given client's current transaction.
 * Guess the mime type based on file extension (include period, ex: ".html").
 * Returns 0 on success, -1 on failure (i.e. if the header does not fit).
 */
int kitserv_http_header_add_content_type_guess(struct kitserv_client*, const char* extension);

/**
 * Add a last-modified header to the given client's current transaction.
 * Returns 0 on success, -1 on failure (i.e. if the header does not fit).
 */
int kitserv_http_header_add_last_modified(struct kitserv_client*, time_t time);

/**
 * Handle a static request for client using the given path and context.
 * Pass NULL to use the default context.
 * On error, returns -1 and sets client->ta.resp_status.
 */
int kitserv_http_handle_static_path(struct kitserv_client* client, const char* path,
                                    struct kitserv_request_context* ctx);

/**
 * Get the request method (e.g. to differentiate between GET and HEAD).
 * Note that the body won't be sent on HEAD requests anyway, so this is not strictly necessary.
 */
enum kitserv_http_method kitserv_api_get_request_method(struct kitserv_client*);

/**
 * Get request path.
 */
const char* kitserv_api_get_request_path(struct kitserv_client*);

/**
 * Get request query.
 * Returns NULL if not provided.
 */
const char* kitserv_api_get_request_query(struct kitserv_client*);

/**
 * Get request content length (i.e. payload length).
 * Returns 0 if not provided.
 */
off_t kitserv_api_get_request_content_length(struct kitserv_client*);

/**
 * Get the cookie with the given key from the request headers.
 * Returns NULL if no such cookie was found.
 */
const char* kitserv_api_get_request_cookie(struct kitserv_client*, const char* key);

/**
 * Get the cookie with the given key from the request headers.
 * Returns NULL if no such cookie was found.
 */
const char* kitserv_api_get_request_cookie_n(struct kitserv_client*, const char* key, int keylen);

/**
 * Get request mime type (content-type header).
 * Returns NULL if not provided.
 */
const char* kitserv_api_get_request_mime_type(struct kitserv_client*);

/**
 * Get request disposition (content-disposition header).
 * Returns NULL if not provided.
 */
const char* kitserv_api_get_request_disposition(struct kitserv_client*);

/**
 * Get the client's requested content range.
 * Places values in `*start` and `*end`. `*start` will always be left of the dash, `*end` will always be right of it.
 * Uses -1 if that range element was not given (e.g. "Range: bytes=50-" places 50 in `*start` and -1 in `*end`).
 * Returns 0 on success, -1 on parse error (`*start` and `*end` unmodified).
 */
int kitserv_api_get_request_range(struct kitserv_client*, off_t* start, off_t* end);

/**
 * Get the time difference (in `*difference`) between the request's if-modified-since header and the given time.
 * Performs `time` - time in the header. (So, a negative `*difference` means it was modified since `time`).
 * Returns 0 if successful, -1 on failure (e.g. no if-modified-since header).
 */
int kitserv_api_get_request_modified_since_difference(struct kitserv_client*, double* difference, time_t time);

/**
 * Read up to `nbytes` from the client as part of the payload during API handling.
 * Returns the number of bytes read (up to `nbytes`).
 * Returns -1 on error, setting errno.
 * If the read blocked (errno == EWOULDBLOCK/EAGAIN), save state and return from the API handler.
 * For all other errors, clean up and return from the API handler. The handler _will_not_ be re-called.
 */
int kitserv_api_read_payload(struct kitserv_client*, char* buf, int nbytes);

/**
 * Write `buflen` bytes from `buf` into the response body.
 * Return the number of bytes written, or -1 on error.
 */
int kitserv_api_write_body(struct kitserv_client*, const char* buf, int buflen);

/**
 * Write a formatted string into the response body.
 * Return the number of bytes written, or -1 on error.
 */
int kitserv_api_write_body_fmt(struct kitserv_client*, const char* fmt, ...);

/**
 * Clear out all currently-set headers.
 */
void kitserv_api_reset_headers(struct kitserv_client*);

/**
 * Reset the response body to an empty buffer.
 */
void kitserv_api_reset_body(struct kitserv_client*);

#define KITSERV_FD_HEAD (-1)
#define KITSERV_FD_DISABLE (0)
/**
 * Send an open fd to the client.
 * Pass KITSERV_FD_HEAD as `fd` to use this to set a valid content-length header via file options + range.
 *      For example, when sending a HEAD request without opening the fd being sent.
 * Pass KITSERV_FD_DISABLE as `fd` to disable sending file (i.e. return to sending normal body).
 * Pass the number of bytes in the file as `filesize`. (so, `st.st_size`, not `st.st_size - 1`)
 * Use `kitserv_api_set_send_range` to alter the range sent within this file.
 * Use of this function overrides sending the body (`kitserv_api_write_body*`) unless KITSERV_FD_DISABLE is sent.
 * Note: `fd` will be closed after the file has been sent, or if this function is called again.
 * Returns 0 on success, -1 on error.
 */
int kitserv_api_send_file(struct kitserv_client*, int fd, off_t filesize);

/**
 * Set the range to send from, byte-inclusive.
 * Does not set the content-range header on the response!
 * Note that `to` is ignored when sending a body - always sends until the end of the buffer.
 * Returns 0 on success, -1 on error.
 */
int kitserv_api_set_send_range(struct kitserv_client*, off_t from, off_t to);

/**
 * Preserve API-set headers if an error response is indicated.
 * Note that, if the body is discarded but headers are not, a content-type header will be added.
 * If set to false (default), headers are discarded for a generic response.
 */
void kitserv_api_set_preserve_headers_on_error(struct kitserv_client*, bool preserve_enabled);

/**
 * Preserve API-set body if an error response is indicated.
 * If set to false (default), the body is discarded for a generic response.
 */
void kitserv_api_set_preserve_body_on_error(struct kitserv_client*, bool preserve_enabled);

/**
 * Set the response status on a transaction.
 * Until this is set, Kitserv will assume that the API has not finished processing.
 */
void kitserv_api_set_response_status(struct kitserv_client*, enum kitserv_http_response_status);

/**
 * Save parsing state on this client (to return from API handler and resume processing later).
 */
void kitserv_api_save_state(struct kitserv_client*, void* state);

#endif
