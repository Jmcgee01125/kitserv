/* Part of Kitserv, licensed under the GNU Affero GPL. */

#ifndef KITSERV_HTTP_H
#define KITSERV_HTTP_H

#include <inttypes.h>
#include <stdbool.h>

#include "buffer.h"
#include "kitserv.h"

#define HTTP_BUFSZ (4096)
#define HTTP_BUFSZ_SMALL (256)

#define HTTP_MAX_COOKIES (50)

enum http_transaction_state {
    HTTP_STATE_READ = 0,
    HTTP_STATE_SERVE,
    HTTP_STATE_PREPARE_RESPONSE,
    HTTP_STATE_SEND,
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

enum http_version {
    HTTP_1_1 = 0,
    HTTP_1_0,
};

struct http_cookie {
    char* key;
    char* value;
    int keylen;
};

struct http_transaction {
    enum http_transaction_state state;
    enum http_parse_state parse_state;

    /* Request fields */
    enum kitserv_http_method req_method;
    enum http_version req_version;
    char* req_payload;    // pointer past the end of the headers (same buffer), where overread payload info will be
    int req_payload_pos;  // index consumed past req_payload
    int req_payload_len;  // number of bytes to available to read past req_payload
    off_t req_content_len;
    char* req_parse_blk;
    char* req_parse_iter;
    // for the following: NULL if not found, or null-terminated string inside req_headers
    char* req_path;
    char* req_query;
    char* req_mimetype;
    char* req_range;
    char* req_disposition;
    char* req_modified_since;
    int req_num_cookies;

    /* Response fields */
    enum kitserv_http_response_status resp_status;
    int resp_start_pos;  // index of data not yet sent, >= len when done
    int resp_start_len;  // total length
    int resp_headers_pos;
    int resp_headers_len;
    /**
     * content-length header:
     *      if resp_fd == 0, set to `client.resp_body.len - client.ta.resp_body_pos`
     *      otherwise, set to `client.ta.resp_body_end - client.ta.resp_body_pos + 1`
     *
     * sending:
     *      if resp_fd == 0, send the contents of client.resp_body.buf from client.ta.resp_body_pos to its end
     *      otherwise, send from client.ta.resp_body_pos to client.ta.resp_body_end in the file
     *
     * hint: for HEAD requests on an fd, use a negative resp_fd
     */
    int resp_fd;
    off_t resp_body_pos;  // send progress, initial value of range start, always used
    off_t resp_body_end;  // final offset when sending fd, end of range or content length
    bool range_requested;
    /**
     * Preserve the headers or body (resp_body or resp_fd) for sending the result. Normally, both are wiped.
     * Note that some headers are added when the body is discarded, and should not be set by preserved headers:
     *      Always: content-type
     *      (And, as with all responses, content-length and server)
     */
    bool preserve_headers_on_error;
    bool preserve_body_on_error;

    kitserv_api_handler_t
        api_endpoint_hit;     // for re-calling API functions without re-parsing tree, and tracking if run at all
    void* api_internal_data;  // data pointer for API requests - NULL on first call
    int api_allow_flags;      // http_method bits, used in case parsing matched an endpoint but not method(s)
};

struct kitserv_client {
    char* req_headers;                // persistent request header information (HTTP_BUFSZ)
    struct http_cookie* req_cookies;  // number of cookies stored in ta

    char* resp_start;    // response start buffer (HTTP_BUFSZ_SMALL)
    char* resp_headers;  // response headers buffer (HTTP_BUFZ)
    buffer_t resp_body;  // response body buffer (at least HTTP_BUFZ, may grow)

    struct http_transaction ta;
    /**
     * req_headers_len is stored here because it may persist across transactions
     * note that it is not always up-to-date: it is only up to date between request start and the beginning of the last
     * call to parse headers (as there is no need to update it after that, until a new transaction begins)
     */
    int req_headers_len;

    int sockfd;
};

/**
 * Initalize HTTP system. Use NULL to disable API endpoints.
 */
void kitserv_http_init(struct kitserv_request_context* http_default_context, struct kitserv_api_tree* http_api_list);

/**
 * Allocate the internal structures of a client and its associated transaction.
 * Returns 0 on success, -1 on failure.
 */
int kitserv_http_create_client_struct(struct kitserv_client*);

/*
 * Reset a client to serve a new transaction on the same connection.
 * Some of the data for that transaction may have been read, this preserves it.
 */
void kitserv_http_finalize_transaction(struct kitserv_client*);

/**
 * Reset a client that will not be used again with the same connection.
 */
void kitserv_http_reset_client(struct kitserv_client*);

/**
 * Parse range request for a client, if one exists.
 * Write results into *from and *to. Use -1 if that field doesn't exist.
 * (*from will always be left of the -, *to will always be right of it).
 * Return 0 on success, -1 on parse error / no header (*from and *to unchanged).
 */
int kitserv_http_parse_range(struct kitserv_client*, off_t* out_from, off_t* out_to);

/**
 * The following functions process a request.
 *
 * Returns -1 on error, or 0 if data was successfully read (even if partially).
 * On success, check client->ta.state to see if the state has advanced or the socket blocked.
 * On error, skip to http_prepare_response and continue (ignore state) or, if passed, close the connection.
 */
int kitserv_http_recv_request(struct kitserv_client* client);
int kitserv_http_serve_request(struct kitserv_client* client);
int kitserv_http_prepare_response(struct kitserv_client* client);
int kitserv_http_send_response(struct kitserv_client* client);

#endif
