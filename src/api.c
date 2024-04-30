#define _XOPEN_SOURCE 500
#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buffer.h"
#include "http.h"
#include "kitserv.h"

enum kitserv_http_method kitserv_api_get_request_method(struct kitserv_client* client)
{
    return client->ta.req_method;
}

const char* kitserv_api_get_request_path(struct kitserv_client* client)
{
    return client->ta.req_path;
}

const char* kitserv_api_get_request_query(struct kitserv_client* client)
{
    return client->ta.req_query;
}

off_t kitserv_api_get_request_content_length(struct kitserv_client* client)
{
    return client->ta.req_content_len;
}

const char* kitserv_api_get_request_cookie(struct kitserv_client* client, const char* key)
{
    if (!key) {
        errno = EINVAL;
        return NULL;
    }
    return kitserv_api_get_request_cookie_n(client, key, strlen(key));
}

const char* kitserv_api_get_request_cookie_n(struct kitserv_client* client, const char* key, int keylen)
{
    int i;
    if (!key || keylen <= 0) {
        errno = EINVAL;
        return NULL;
    }
    if (client->ta.req_fresh_cookies) {
        kitserv_http_parse_cookies(client);
    }
    for (i = 0; i < client->ta.req_num_cookies; i++) {
        if (keylen == client->req_cookies[i].keylen && !strcmp(key, client->req_cookies[i].key)) {
            return client->req_cookies[i].value;
        }
    }
    return NULL;
}

const char* kitserv_api_get_request_mime_type(struct kitserv_client* client)
{
    return client->ta.req_mimetype;
}

const char* kitserv_api_get_request_disposition(struct kitserv_client* client)
{
    return client->ta.req_disposition;
}

int kitserv_api_get_request_range(struct kitserv_client* client, off_t* start, off_t* end)
{
    if (kitserv_http_parse_range(client, start, end)) {
        return -1;
    }
    return 0;
}

int kitserv_api_get_request_modified_since_difference(struct kitserv_client* client, double* difference, time_t time)
{
    struct tm tm;
    if (!client->ta.req_modified_since || !strptime(client->ta.req_modified_since, "%a, %d %b %Y %T GMT", &tm)) {
        return -1;
    }
    *difference = difftime(time, timegm(&tm));
    return 0;
}

int kitserv_api_read_payload(struct kitserv_client* client, char* buf, int nbytes)
{
    int rc;
    int written = 0;

    // if we overread from the headers, give them that data now
    rc = client->ta.req_payload_len - client->ta.req_payload_pos;
    if (rc > 0) {
        written = nbytes > rc ? rc : nbytes;
        memcpy(buf, &client->ta.req_payload[client->ta.req_payload_pos], written);
        client->ta.req_payload_pos += written;
        nbytes -= written;
    }

    // didn't overread enough, so let's read some more directly to them
    // DO NOT overread here!
    while (nbytes > 0) {
        rc = read(client->sockfd, &buf[written], nbytes);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return written > 0 ? written : -1;
            }
            client->ta.resp_status = HTTP_X_HANGUP;
            return -1;
        } else if (rc == 0) {
            // client has closed their end of the connection,
            // but if we read something we should pass it along
            if (written > 0) {
                return written;
            }
            return -1;
        }
        written += rc;
        nbytes -= rc;
    }

    return written;
}

int kitserv_api_write_body(struct kitserv_client* client, const char* buf, int buflen)
{
    int pre_sz = client->resp_body.len;
    if (kitserv_buffer_append(&client->resp_body, buf, buflen)) {
        errno = ENOMEM;
        return -1;
    }
    return client->resp_body.len - pre_sz;
}

int kitserv_api_write_body_fmt(struct kitserv_client* client, const char* fmt, ...)
{
    va_list ap;
    int pre_sz = client->resp_body.len;
    va_start(ap, fmt);
    if (kitserv_buffer_appendva(&client->resp_body, fmt, &ap)) {
        errno = ENOMEM;
        va_end(ap);
        return -1;
    }
    va_end(ap);
    return client->resp_body.len - pre_sz;
}

void kitserv_api_reset_headers(struct kitserv_client* client)
{
    client->ta.resp_headers_len = 0;
}

void kitserv_api_reset_body(struct kitserv_client* client)
{
    kitserv_buffer_reset(&client->resp_body, HTTP_BUFSZ);
}

int kitserv_api_send_file(struct kitserv_client* client, int fd, off_t filesize)
{
    if (client->ta.resp_fd > 0) {
        close(client->ta.resp_fd);
    }
    client->ta.resp_fd = fd;
    client->ta.resp_body_pos = 0;
    if (fd != KITSERV_FD_DISABLE) {
        client->ta.resp_body_end = filesize - 1;
    }
    return 0;
}

int kitserv_api_set_send_range(struct kitserv_client* client, off_t from, off_t to)
{
    if (from < 0 || to < 0) {
        errno = EINVAL;
        return -1;
    }
    client->ta.resp_body_pos = from;
    client->ta.resp_body_end = to;
    return 0;
}

void kitserv_api_set_preserve_headers_on_error(struct kitserv_client* client, bool preserve_enabled)
{
    client->ta.preserve_headers_on_error = preserve_enabled;
}

void kitserv_api_set_preserve_body_on_error(struct kitserv_client* client, bool preserve_enabled)
{
    client->ta.preserve_body_on_error = preserve_enabled;
}

void kitserv_api_set_response_status(struct kitserv_client* client, enum kitserv_http_response_status status)
{
    client->ta.resp_status = status;
}

void kitserv_api_save_state(struct kitserv_client* client, void* state)
{
    client->ta.api_internal_data = state;
}
