/* Part of FileKit, licensed under the GNU Affero GPL. */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/sendfile.h>
#endif

#include "buffer.h"

#define bufscmp(s, target) (!memcmp(s, target, sizeof(target) - 1))

char* server_name;
char* web_root;
char* web_fallback_path;       // may be null
char* web_root_fallback_path;  // may be null
bool html_append_fallback;
char* target_cookie_name = NULL;
struct http_api_tree* api_tree = NULL;

void http_init(char* servername, char* http_directory, char* fallback_path, char* root_fallback_path,
               bool use_html_append_fallback, char* auth_cookie_name, struct http_api_tree* http_api_list)
{
    server_name = servername;
    web_root = http_directory;
    web_fallback_path = fallback_path;
    web_root_fallback_path = root_fallback_path;
    html_append_fallback = use_html_append_fallback;
    if (auth_cookie_name && http_api_list) {
        target_cookie_name = auth_cookie_name;
        api_tree = http_api_list;
    }
}

int http_create_client_struct(struct http_client* client)
{
    if (!(client->req_headers = malloc(HTTP_BUFSZ * 2))) {
        return -1;
    }
    if (!(client->resp_start = malloc(HTTP_BUFSZ))) {
        free(client->req_headers);
        return -1;
    }
    if (!(client->resp_headers = malloc(HTTP_BUFSZ))) {
        free(client->req_headers);
        free(client->resp_start);
        return -1;
    }
    if (buffer_init(&client->resp_body, HTTP_BUFSZ)) {
        free(client->req_headers);
        free(client->resp_start);
        free(client->resp_headers);
        return -1;
    }
    http_reset_client(client);
    return 0;
}

static void cleanup_client(struct http_client* client)
{
    memset(&client->ta, 0, sizeof(struct http_transaction));
    buffer_reset(&client->resp_body, HTTP_BUFSZ);
}

void http_finalize_transaction(struct http_client* client)
{
    // in case the client sent part of their next request into the buffers for this one
    // so, what we considered the payload length is actually now the header length
    memmove(client->req_headers, &client->req_payload[client->ta.req_payload_pos], client->ta.req_payload_len);
    client->req_headers_len = client->ta.req_payload_len;
    cleanup_client(client);
}

void http_reset_client(struct http_client* client)
{
    client->req_headers_len = 0;
    cleanup_client(client);
}

/**
 * Convert the given string to a number, storing the result in *dest.
 * If an error has occured, *dest is undefined, and -1 is returned.
 */
static int strtonum(char* str, int64_t* dest)
{
    char* endptr;
    errno = 0;
    *dest = strtoll(str, &endptr, 10);
    if (errno || *endptr) {
        return -1;
    }
    return 0;
}

/**
 * Silently close the given fd and set it to zero.
 */
static void close_set_zero(int* fd)
{
    close(*fd);
    *fd = 0;
}

/**
 * Append to buf at the given offset (which is incremented), respecting max.
 * Returns 0 on success, -1 on failure.
 * *offset is not incremented on failure, but buf[offset..max] is undefined.
 */
static inline int str_vappend(char* buf, int* offset, int max, char* fmt, va_list* ap)
{
    int rc = vsnprintf(&buf[*offset], max - *offset, fmt, *ap);
    if (rc >= max - *offset) {
        // didn't fit
        return -1;
    }
    *offset += rc;
    return 0;
}

/**
 * Append to buf at the given offset (which is incremented), respecting max.
 * Returns 0 on success, -1 on failure.
 * *offset is not incremented on failure, but buf[offset..max] is undefined.
 */
static int str_append(char* buf, int* offset, int max, char* fmt, ...)
{
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = str_vappend(buf, offset, max, fmt, &ap);
    va_end(ap);
    return rc;
}

static inline bool status_is_error(enum http_response_status status)
{
    return status >= 400;
}

static int parse_header_cookie(struct http_client* client, char* value)
{
    // Cookie: NAME=VALUE; NAME=VALUE

    char* p = value;
    char* r;  // =, then value
    char* q;  // ; or NULL if end

    while (1) {
        r = strchr(p, '=');
        if (!r) {
            client->ta.resp_status = HTTP_400_BAD_REQUEST;
            return -1;
        }
        q = strchr(r, ';');
        if (q) {
            *q = '\0';
        }
        *r = '\0';
        r++;

        // if value exists, parse wanted cookies
        if (r != q) {
            if (target_cookie_name && !strcmp(p, target_cookie_name)) {
                client->ta.req_cookie = r;
            }
        }

        // go to next cookie if it exists, otherwise break out
        if (!q) {
            break;
        }
        p = q + 1;
    }

    return 0;
}

static int parse_header_range(struct http_client* client, char* value)
{
    client->ta.range_requested = true;
    client->ta.req_range = value;
    return 0;
}

static int parse_header_if_modified_since(struct http_client* client, char* value)
{
    // If-Modified-Since: DAYNAME, DAY MONTH YEAR HH:MM:SS GMT
    client->ta.req_modified_since = value;
    return 0;
}

static int parse_header_content_length(struct http_client* client, char* value)
{
    // Content-Length: LENGTH
    if (strtonum(value, &client->ta.req_content_len) || client->ta.req_content_len < 0) {
        goto bad_request;
    }
    return 0;
bad_request:
    client->ta.resp_status = HTTP_400_BAD_REQUEST;
    return -1;
}

static int parse_header_content_type(struct http_client* client, char* value)
{
    // Content-Type: MIME/TYPE
    client->ta.req_mimetype = value;
    return 0;
}

static int parse_header_content_disposition(struct http_client* client, char* value)
{
    // Content-Disposition: attachment; filename=FILENAME
    client->ta.req_disposition = value;
    return 0;
}

struct header {
    char* name;
    int len;
    int (*func)(struct http_client* client, char* value); /* returns 0 on successs, -1 on error, setting resp_status */
};

#define HEADERS_NUM (6)
static const struct header headers[] = {
    {.name = "cookie", .len = 6, .func = parse_header_cookie},
    {.name = "range", .len = 5, .func = parse_header_range},
    {.name = "if-modified-since", .len = 17, .func = parse_header_if_modified_since},
    {.name = "content-length", .len = 14, .func = parse_header_content_length},
    {.name = "content-type", .len = 12, .func = parse_header_content_type},
    {.name = "content-disposition", .len = 19, .func = parse_header_content_disposition},
};

/**
 * Parse the range request of a client, setting the resp_body_pos and resp_body_end fields appropriately.
 * Assumes `client->ta.range_requested` is true, and `client->ta.req_range` is set.
 * Returns -1 if the range is malformed (or multipart), setting `client->ta.resp_status`. Fields may be modified.
 */
static int parse_range(struct http_client* client, off_t filesize)
{
    char* p = client->ta.req_range;
    char* q = NULL;
    char* r;
    char* hyphen = NULL;
    off_t conv;

    if (strncmp(p, "bytes=", 6)) {
        goto bad_request;
    }
    p += 6;

    // find the position of the hyphen, set p to before and q after
    for (r = p; *r != '\0'; r++) {
        if (*r < '0' || *r > '9') {
            if (*r == '-') {
                if (hyphen) {
                    goto bad_request;  // two hyphens
                }
                hyphen = r;
            } else if (*r == ',') {
                // multipart - we don't support
                goto not_satisfiable;
            } else {
                goto bad_request;
            }
        }
    }
    if (!hyphen) {
        goto bad_request;
    }
    if (p == hyphen) {
        // i.e. bytes=-YYY
        p = NULL;
    }
    if (hyphen[1] != '\0') {
        q = hyphen + 1;
    }

    // parse individual sections
    if (p) {
        *hyphen = '\0';  // cap off p
        // bytes=XXX- or bytes=XXX-YYY
        if (strtonum(p, &conv)) {
            goto bad_request;
        }
        if (conv > filesize) {
            goto not_satisfiable;
        }
        client->ta.resp_body_pos = conv;
        if (q) {
            // bytes=XXX-YYY
            if (strtonum(q, &conv)) {
                goto bad_request;
            }
            client->ta.resp_body_end = conv > filesize - 1 ? filesize - 1 : conv;
        } else {
            // bytes=XXX-
            client->ta.resp_body_end = filesize;
        }
    } else if (q) {
        // bytes=-YYY
        if (strtonum(q, &conv)) {
            goto bad_request;
        }
        if (conv > filesize) {
            goto not_satisfiable;
        }
        client->ta.resp_body_pos = filesize - conv;
        client->ta.resp_body_end = filesize;
    } else {
        // mm yes `bytes=-`, very good
        goto bad_request;
    }

    return 0;
bad_request:
    client->ta.resp_status = HTTP_400_BAD_REQUEST;
    return -1;
not_satisfiable:
    client->ta.resp_status = HTTP_416_RANGE_NOT_SATISFIABLE;
    return -1;
}

/**
 * Guess the mime type from a limited list.
 * On null, return text/plain.
 */
static const char* guess_mime_type(char* extension)
{
    if (extension == NULL) {
        return "text/plain";
    } else if (!strcasecmp(extension, ".js")) {
        return "text/javascript";
    } else if (!strcasecmp(extension, ".html")) {
        return "text/html";
    } else if (!strcasecmp(extension, ".css")) {
        return "text/css";
    } else if (!strcasecmp(extension, ".json")) {
        return "application/json";
    } else if (!strcasecmp(extension, ".svg")) {
        return "image/svg+xml";
    } else if (!strcasecmp(extension, ".png")) {
        return "image/png";
    } else if (!strcasecmp(extension, ".jpg")) {
        return "image/jpeg";
    } else if (!strcasecmp(extension, ".gif")) {
        return "image/gif";
    } else if (!strcasecmp(extension, ".mp4")) {
        return "video/mp4";
    }

    return "text/plain";
}

/**
 * Decode URL-encoded characters, writing the modified string in-place.
 */
static void url_decode(char* str)
{
    char* s = str;    // source
    char* r = str;    // result, chases p
    unsigned char x;  // decoded char

    while (*s) {
        if (*s == '%' && isxdigit(s[1]) && isxdigit(s[2])) {
            sscanf(s + 1, "%2hhx", &x);
            *r = x;
            s += 3;
        } else {
            *r = *s;
            s++;
        }
        r++;
    }
    *r = '\0';
}

static inline int http_header_add_ap(struct http_client* client, char* key, char* fmt, va_list* ap)
{
    int pre_sz;
    pre_sz = client->ta.resp_headers_len;

    // append key
    if (str_append(client->resp_headers, &client->ta.resp_headers_len, HTTP_BUFSZ, "%s: ", key)) {
        goto too_large;
    }

    // append value
    if (str_vappend(client->resp_headers, &client->ta.resp_headers_len, HTTP_BUFSZ, fmt, ap)) {
        goto too_large;
    }

    // append EOL
    if (str_append(client->resp_headers, &client->ta.resp_headers_len, HTTP_BUFSZ, "\r\n")) {
        goto too_large;
    }
    return 0;

too_large:
    // couldn't fit the header, reset and return
    client->ta.resp_headers_len = pre_sz;
    client->ta.resp_status = HTTP_507_INSUFFICIENT_STORAGE;
    return -1;
}

int http_header_add(struct http_client* client, char* key, char* fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = http_header_add_ap(client, key, fmt, &ap);
    va_end(ap);
    return ret;
}

int http_header_add_content_type(struct http_client* client, char* mime)
{
    return http_header_add(client, "content-type", "%s", mime);
}

int http_header_add_content_type_guess(struct http_client* client, char* extension)
{
    return http_header_add(client, "content-type", "%s", guess_mime_type(extension));
}

int http_header_add_set_cookie(struct http_client* client, char* fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = http_header_add(client, "cookie", fmt, ap);
    va_end(ap);
    return ret;
}

int http_header_add_last_modified(struct http_client* client, time_t time)
{
    // take time_t from (struct stat).st_mtim.tv_sec
    struct tm tm;
    char buf[32];
    if (!gmtime_r(&time, &tm) || !strftime(buf, 32, "%a, %d %b %Y %T GMT", &tm)) {
        return -1;
    }
    return http_header_add(client, "last-modified", "%s", buf);
}

static int http_header_add_content_range(struct http_client* client, off_t start, off_t end, off_t total)
{
    return http_header_add(client, "content-range", "bytes %ld-%ld/%ld", start, end, total);
}

static int http_header_add_content_length(struct http_client* client, off_t length)
{
    return http_header_add(client, "content-length", "%ld", length);
}

static int http_header_add_allow(struct http_client* client)
{
    if (client->ta.hit_api) {
        // TODO: if we have a 405 Method Not Allowed, we MUST return an Allow header *for the target resource*
        //      so, we must detect the api endpoints here and determine the rules as well
        //      (since the api endpoints will fail in serve_api)
        //      /api/upload   - POST
        //      /api/delete   - DELETE
        //      /api/files    - GET/HEAD
        //      /d/           - GET/HEAD
        //      http internal - GET/HEAD
        return -1;
    } else {
        return http_header_add(client, "allow", "GET, HEAD");
    }
}

int http_recv_request(struct http_client* client)
{
    int readrc, i;
    char* p = client->ta.req_parse_blk;   // beginning of unconsumed block
    char* r = client->ta.req_parse_iter;  // segment iterator
    char *q, *s;                          // extra iters - in between p and r

#define parse_past_end(ptr) (ptr - client->req_headers > client->req_headers_len)
#define parse_advance \
    do {              \
        r++;          \
        p = r;        \
    } while (0)

    // always start by trying to read some more off of the connection, since otherwise we wouldn't be here
    // (there is one special case where we might already have a request, but it's fine to just assume we try to read)
    do {
        readrc =
            read(client->sockfd, &client->req_headers[client->req_headers_len], HTTP_BUFSZ - client->req_headers_len);
        if (readrc > 0) {
            client->req_headers_len += readrc;
        } else if (readrc == 0 || (readrc < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            // EOF or error - we'll handle the EAGAIN / EWOULDBLOCK case later
            client->ta.resp_status = HTTP_X_HANGUP;
            return -1;
        }
    } while (readrc > 0);

    switch (client->ta.parse_state) {
        case HTTP_PS_NEW:
            p = client->req_headers;
            r = client->req_headers;
            /* fallthrough */
        case HTTP_PS_REQ_METHOD:
            /* 'GET ' */
            for (; !parse_past_end(r) && *r != ' '; r++)
                ;
            if (parse_past_end(r)) {
                client->ta.parse_state = HTTP_PS_REQ_METHOD;
                goto past_end;
            }
            switch (r - p) {
                case 3:
                    if (bufscmp(p, "GET ")) {
                        client->ta.req_method = HTTP_GET;
                        break;
                    } else if (bufscmp(p, "PUT ")) {
                        client->ta.req_method = HTTP_PUT;
                        break;
                    }
                    goto method_not_allowed;
                case 4:
                    if (bufscmp(p, "HEAD ")) {
                        client->ta.req_method = HTTP_HEAD;
                        break;
                    } else if (bufscmp(p, "POST ")) {
                        client->ta.req_method = HTTP_POST;
                        break;
                    }
                    goto method_not_allowed;
                case 6:
                    if (bufscmp(p, "DELETE ")) {
                        client->ta.req_method = HTTP_DELETE;
                        break;
                    }
                    goto method_not_allowed;
                default:
method_not_allowed:
                    client->ta.resp_status = HTTP_405_METHOD_NOT_ALLOWED;
                    return -1;
            }
            parse_advance;
            /* fallthrough */

        case HTTP_PS_REQ_PATH:
            /* '/request/path?query ' */
            /*  p            s     r/q */
            for (; !parse_past_end(r) && *r != ' '; r++)
                ;
            if (parse_past_end(r)) {
                client->ta.parse_state = HTTP_PS_REQ_PATH;
                goto past_end;
            }
            s = NULL;
            for (q = p; q < r; q++) {
                if (!isprint(*q)) {
                    goto bad_request;
                }
                if (!s && *q == '?') {
                    s = q;
                }
            }
            *r = '\0';
            if (s) {
                *s = '\0';
                url_decode(s + 1);
                client->ta.req_query = s + 1;
            }
            url_decode(p);
            client->ta.req_path = p;
            parse_advance;
            /* fallthrough */

        case HTTP_PS_REQ_VERSION:
            /* 'HTTP/1.1\r' */
            for (; !parse_past_end(r) && *r != '\r'; r++)
                ;
            if (parse_past_end(r)) {
                client->ta.parse_state = HTTP_PS_REQ_VERSION;
                goto past_end;
            }
            if (r - p < 5 || !bufscmp(p, "HTTP/")) {
                goto bad_request;
            }
            p += sizeof("HTTP/") - 1;
            switch (r - p) {
                case 3:
                    if (bufscmp(p, "1.1")) {
                        client->ta.req_version = HTTP_1_1;
                        break;
                    } else if (bufscmp(p, "1.0")) {
                        client->ta.req_version = HTTP_1_0;
                        break;
                    }
                    /* fallthrough - go to failure */
                default:
                    client->ta.resp_status = HTTP_505_VERSION_NOT_SUPPORTED;
                    return -1;
            }
            parse_advance;
            /* fallthrough */

        case HTTP_PS_REQ_VERSION_LF:
            /* we saw \r to cap off req version, now wait for \n */
            if (parse_past_end(r)) {
                client->ta.parse_state = HTTP_PS_REQ_VERSION_LF;
                goto past_end;
            }
            if (*r != '\n') {
                goto bad_request;
            }
            parse_advance;
            /* fallthrough */

        case HTTP_PS_REQ_HEAD:
parse_header:
            /* 'Header: value\r' or simply '\r' if no more */
            for (; !parse_past_end(r) && *r != '\r'; r++)
                ;
            if (parse_past_end(r)) {
                client->ta.parse_state = HTTP_PS_REQ_HEAD;
                goto past_end;
            }
            if (r != p) {
                /*  header:  value\r\n
                 *  header\0 value\0\n
                 *  ^     ^  ^    ^
                 *  p     s  q    r
                 */
                // split off this header into its own string
                *r = '\0';
                // separate name/value
                q = strchr(p, ':');
                if (!q) {
                    goto bad_request;
                }
                *q = '\0';
                s = q;
                // skip whitespace
                for (q++; *q == ' ' || *q == '\t'; q++)
                    ;
                // process the header if it's one we care about
                for (i = 0; i < HEADERS_NUM; i++) {
                    if (s - p == headers[i].len && !strcasecmp(p, headers[i].name)) {
                        if (headers[i].func(client, q)) {
                            return -1;
                        }
                        break;
                    }
                }
                parse_advance;
            } else {
                // saw \r immediately, should be at the end now
                // we don't advance p here because we want to allow the _LF check to know if we had \r\n\r\n or a header
                r++;
            }
            /* fallthrough */

        case HTTP_PS_REQ_HEAD_LF:
            /* we saw \r to cap off req head, now wait for \n */
            if (parse_past_end(r)) {
                client->ta.parse_state = HTTP_PS_REQ_HEAD_LF;
                goto past_end;
            }
            if (*r != '\n') {
                goto bad_request;
            }
            if (r == p) {
                // keep on reading more headers - they advanced both p and r to the expected \n
                parse_advance;
                goto parse_header;
            } else {
                // read an empty \r\n, we're done!
                parse_advance;  // still need to advance to point at the start of the payload, if it exists
                break;
            }

        default:
            fprintf(stderr, "Unknown parse state: %d\n", client->ta.parse_state);
            client->ta.resp_status = HTTP_500_INTERNAL_ERROR;
            return -1;
    }

    client->req_payload = p;  // the payload will follow what we've just parsed
    client->ta.state = HTTP_STATE_API;
    return 0;

    /* before jumping to `past_end`, set the parse state to wherever you came from */
past_end:
    if (readrc == 0 && client->req_headers_len == HTTP_BUFSZ) {
        client->ta.resp_status = HTTP_431_REQUEST_HEADER_FIELDS_TOO_LARGE;
        return -1;
    }
    // no error, just don't have data yet (read hit EAGAIN/EWOULDBLOCK) - save our current position
    client->ta.req_parse_blk = p;
    client->ta.req_parse_iter = r;
    return 0;

bad_request:
    client->ta.resp_status = HTTP_400_BAD_REQUEST;
    return -1;

#undef parse_past_end
#undef parse_advance
}

int http_serve_api(struct http_client* client)
{
    if (api_tree) {
        // TODO: parse the API endpoints according to the list, and call off to them if we match
        //       they will fill out the request information instead of us, indicating so by setting the resp status

        // TODO: save the address of the function being called, if not null then jump immediately (no list check)

        // TODO: API probably needs their own data storage for managing internal state, like whether or not validation
        //       has succeeded for a given user - give them a void* in the ta struct

        // TODO: if we hit api, set `client->ta.hit_api = true;`
        fprintf(stderr, "API subsystem not implemented. Hanging up.\n");
        return -1;
    }
    client->ta.state = HTTP_STATE_PREPARE_RESPONSE;
    return 0;
}

static const char* get_version_string(enum http_version version)
{
    // space at the end is relevant for easy append to status
    switch (version) {
        case HTTP_1_1:
            return "HTTP/1.1 ";
        case HTTP_1_0:
        default:
            return "HTTP/1.0 ";
    }
}

static const char* get_status_string(enum http_response_status status)
{
    // \r\n since we're always going to add it anyway
    switch (status) {
        case HTTP_200_OK:
            return "200 OK\r\n";
        case HTTP_206_PARTIAL_CONTENT:
            return "206 Partial Content\r\n";
        case HTTP_400_BAD_REQUEST:
            return "400 Bad Request\r\n";
        case HTTP_403_PERMISSION_DENIED:
            return "403 Permission Denied\r\n";
        case HTTP_404_NOT_FOUND:
            return "404 Not Found\r\n";
        case HTTP_405_METHOD_NOT_ALLOWED:
            return "405 Method Not Allowed\r\n";
        case HTTP_408_REQUEST_TIMEOUT:
            return "408 Request Timeout\r\n";
        case HTTP_413_CONTENT_TOO_LARGE:
            return "413 Content Too Large\r\n";
        case HTTP_414_URI_TOO_LONG:
            return "414 URI Too Long\r\n";
        case HTTP_416_RANGE_NOT_SATISFIABLE:
            return "416 Range Not Satisfiable\r\n";
        case HTTP_431_REQUEST_HEADER_FIELDS_TOO_LARGE:
            return "431 Request Header Fields Too Large\r\n";
        case HTTP_501_NOT_IMPLEMENTED:
            return "501 Not Implemented\r\n";
        case HTTP_503_SERVICE_UNAVAILABLE:
            return "503 Service Unavailable\r\n";
        case HTTP_505_VERSION_NOT_SUPPORTED:
            return "505 Version Not Supported\r\n";
        case HTTP_507_INSUFFICIENT_STORAGE:
            return "507 Insufficient Storage\r\n";
        case HTTP_500_INTERNAL_ERROR:
        case HTTP_X_RESP_STATUS_MISSING:
        default:
            return "500 Internal Server Error\r\n";
    }
}

static void prepare_resp_start(struct http_client* client)
{
    // should always fit, unless some idiot changed HTTP_BUFSZ to be really small
    client->ta.resp_start_len = 0;
    str_append(client->resp_start, &client->ta.resp_start_len, HTTP_BUFSZ, "%s",
               get_version_string(client->ta.req_version));
    str_append(client->resp_start, &client->ta.resp_start_len, HTTP_BUFSZ, "%s",
               get_status_string(client->ta.resp_status));
}

/**
 * Returns true if the given path contains attempted IDOR - /../
 */
static bool attempted_idor(char* path)
{
    char* found = strstr(path, "..");
    if (!found) {
        return false;
    }
    // we don't want to accidentally reject paths like `hello......world!`
    // first, ensure that .. either ends the path or is followed by a /
    if (found[2] == '\0' || found[2] == '/') {
        // if so, ensure it either started the path or was preceded by a /
        if (path == found || found[-1] == '/') {
            return true;
        }
    }
    return false;
}

/**
 * Return true if a file is valid, false otherwise, setting errno to indicate the error
 */
static bool file_is_valid(char* path)
{
    struct stat st;
    if (stat(path, &st)) {
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        errno = EISDIR;
        return false;
    }
    return true;
}

/**
 * Create and validate a full path into the given string, which must be at least PATH_MAX bytes
 * On error, sets the response status and returns -1
 */
static int validate_http_path(struct http_client* client, char* fname)
{
    int rc;

    if (attempted_idor(client->ta.req_path)) {
        client->ta.resp_status = HTTP_400_BAD_REQUEST;
        return -1;
    }

    // regular direct path or the root fallback
    if (web_root_fallback_path && !strcmp(client->ta.req_path, "/")) {
        rc = snprintf(fname, PATH_MAX, "%s/%s", web_root, web_root_fallback_path);
    } else {
        rc = snprintf(fname, PATH_MAX, "%s/%s", web_root, client->ta.req_path);
    }
    if (rc < 0 || rc >= PATH_MAX) {
        client->ta.resp_status = HTTP_414_URI_TOO_LONG;
        return -1;
    }
    if (file_is_valid(fname)) {
        return 0;
    }
    if (errno == EACCES) {
        client->ta.resp_status = HTTP_403_PERMISSION_DENIED;
        return -1;
    }

    // append .html and see if it exists
    if (html_append_fallback) {
        rc = snprintf(fname, PATH_MAX, "%s/%s.html", web_root, client->ta.req_path);
        if (rc < 0 || rc >= PATH_MAX) {
            client->ta.resp_status = HTTP_414_URI_TOO_LONG;
            return -1;
        }
        if (file_is_valid(fname)) {
            return 0;
        }
        if (errno == EACCES) {
            client->ta.resp_status = HTTP_403_PERMISSION_DENIED;
            return -1;
        }
    }

    // serve the generic fallback
    if (web_fallback_path) {
        rc = snprintf(fname, PATH_MAX, "%s/%s", web_root, web_fallback_path);
        if (rc < 0 || rc >= PATH_MAX) {
            client->ta.resp_status = HTTP_414_URI_TOO_LONG;
            return -1;
        }
        if (file_is_valid(fname)) {
            return 0;
        }
        if (errno == EACCES) {
            client->ta.resp_status = HTTP_403_PERMISSION_DENIED;
            return -1;
        }
    }

    client->ta.resp_status = HTTP_404_NOT_FOUND;
    return -1;
}

/**
 * Handle a static request path - the request is not an API endpoint
 */
static int handle_static_path(struct http_client* client)
{
    char fname[PATH_MAX];
    struct stat st;
    struct tm tm;
    int rc;

    if (client->ta.req_method != HTTP_GET && client->ta.req_method != HTTP_HEAD) {
        client->ta.resp_status = HTTP_405_METHOD_NOT_ALLOWED;
        return -1;
    }

    // TODO: take advantage of the fact that this will call `stat` on our target file, perhaps also have it open it
    if (validate_http_path(client, fname)) {
        return -1;
    }

    // validate... told us this exists, so any error here is weird
    if (stat(fname, &st)) {
        client->ta.resp_status = HTTP_500_INTERNAL_ERROR;
        return -1;
    }
    rc = open(fname, O_RDONLY);
    if (rc < 0) {
        client->ta.resp_status = HTTP_500_INTERNAL_ERROR;
        return -1;
    }
    client->ta.resp_fd = rc;
    client->ta.resp_fd_size = st.st_size;

    if (client->ta.range_requested) {
        // we have a range request, parse it and set the header, erroring if necessary
        if (parse_range(client, st.st_size) ||
            http_header_add_content_range(client, client->ta.resp_body_pos, client->ta.resp_body_end,
                                          client->ta.resp_fd_size)) {
            close_set_zero(&client->ta.resp_fd);
            return -1;
        }
    } else {
        // resp_body_pos already set - 0
        client->ta.resp_body_end = client->ta.resp_fd_size - 1;
    }

    // add content type, accept-ranges, and last modified headers
    if (http_header_add_content_type_guess(client, strrchr(fname, '.')) ||
        http_header_add(client, "accept-ranges", "bytes") || http_header_add_last_modified(client, st.st_mtim.tv_sec)) {
        close_set_zero(&client->ta.resp_fd);
        return -1;
    }

    if (client->ta.req_modified_since) {
        if (!strptime(client->ta.req_modified_since, "%a, %d %b %Y %T GMT", &tm)) {
            client->ta.resp_status = HTTP_400_BAD_REQUEST;
            close_set_zero(&client->ta.resp_fd);
            return -1;
        }
        if (difftime(st.st_mtim.tv_sec, timegm(&tm)) <= 0) {
            client->ta.resp_status = HTTP_304_NOT_MODIFIED;
            client->ta.req_method = HTTP_HEAD;  // since the response is otherwise identical
            close_set_zero(&client->ta.resp_fd);
        }
    } else if (client->ta.range_requested) {
        client->ta.resp_status = HTTP_206_PARTIAL_CONTENT;
    } else {
        client->ta.resp_status = HTTP_200_OK;
    }

    return 0;
}

/**
 * Make an error response on a given client
 * If this fails, you're having a very bad time
 */
static int prepare_error_response(struct http_client* client)
{
    int rc;

    // discard anything written so far and start over
    prepare_resp_start(client);
    client->ta.resp_headers_len = 0;
    // client->resp_body.len = 0;  // except the body

    if ((rc = http_header_add_content_type(client, "text/plain"))) {
        return rc;
    }

    switch (client->ta.resp_status) {
        case HTTP_400_BAD_REQUEST:
            return buffer_append(&client->resp_body, "Bad request.", sizeof("Bad request.") - 1);
        case HTTP_403_PERMISSION_DENIED:
            return buffer_append(&client->resp_body, "Permission denied.", sizeof("Permission denied.") - 1);
        case HTTP_404_NOT_FOUND:
            // TODO: should probably put the path in here
            return buffer_append(&client->resp_body, "Not found.", sizeof("Not found.") - 1);
        case HTTP_405_METHOD_NOT_ALLOWED:
            if ((rc = http_header_add_allow(client))) {
                return rc;
            }
            return buffer_append(&client->resp_body, "Method not allowed.", sizeof("Method not allowed.") - 1);
        case HTTP_408_REQUEST_TIMEOUT:
            return buffer_append(&client->resp_body, "Request timeout.", sizeof("Request timeout.") - 1);
        case HTTP_413_CONTENT_TOO_LARGE:
            return buffer_append(&client->resp_body, "Content too large.", sizeof("Content too large.") - 1);
        case HTTP_414_URI_TOO_LONG:
            return buffer_append(&client->resp_body, "URI too long.", sizeof("URI too long.") - 1);
        case HTTP_416_RANGE_NOT_SATISFIABLE:
            return buffer_append(&client->resp_body, "Range not satisfiable.", sizeof("Range not satisfiable.") - 1);
        case HTTP_431_REQUEST_HEADER_FIELDS_TOO_LARGE:
            return buffer_append(&client->resp_body, "Request header fields too large.",
                                 sizeof("Request header fields too large.") - 1);
        case HTTP_501_NOT_IMPLEMENTED:
            return buffer_append(&client->resp_body, "Not implemented.", sizeof("Not implemented.") - 1);
        case HTTP_503_SERVICE_UNAVAILABLE:
            return buffer_append(&client->resp_body, "Service unavailable.", sizeof("Service unavailable.") - 1);
        case HTTP_505_VERSION_NOT_SUPPORTED:
            return buffer_append(&client->resp_body, "Version not supported.", sizeof("Version not supported.") - 1);
        case HTTP_507_INSUFFICIENT_STORAGE:
            return buffer_append(&client->resp_body, "Insufficient storage.", sizeof("Insufficient storage.") - 1);
        case HTTP_500_INTERNAL_ERROR:
        case HTTP_X_RESP_STATUS_MISSING:
        default:
            return buffer_append(&client->resp_body, "Internal server error.", sizeof("Internal server error.") - 1);
    }
}

int http_prepare_response(struct http_client* client)
{
    bool already_errored = false;

    if (client->ta.resp_status == HTTP_X_HANGUP) {
        return -1;
    }

    if (status_is_error(client->ta.resp_status)) {
        goto error_response;
    }

    if (!client->ta.hit_api && handle_static_path(client)) {
        // API didn't handle it, and when we tried to do it internally we resulted in an error
        goto error_response;
    }

success:
    prepare_resp_start(client);

    // still need to add some headers here: content-length and server
    // others should have been set already

    // different measurements based on whether we're sending a file or the body buffer
    if (client->ta.resp_fd == 0) {
        if (http_header_add_content_length(client, client->resp_body.len)) {
            goto error_response;
        }
    } else {
        if (http_header_add_content_length(client, client->ta.resp_body_end - client->ta.resp_body_pos + 1)) {
            goto error_response;
        }
    }

    if (http_header_add(client, "server", "%s", server_name)) {
        goto error_response;
    }

    if (str_append(client->resp_headers, &client->ta.resp_headers_len, HTTP_BUFSZ, "\r\n")) {
        client->ta.resp_fd = HTTP_507_INSUFFICIENT_STORAGE;
        goto error_response;
    }
    client->ta.state = HTTP_STATE_SEND_START;
    return 0;

error_response:
    if (client->ta.resp_fd != 0) {
        close_set_zero(&client->ta.resp_fd);
    }
    if (already_errored || prepare_error_response(client)) {
        // highly unlikely to be salvageable, so just drop the connection
        return -1;
    }
    already_errored = true;
    goto success;
}

/**
 * Send to the given socket fd, starting at *pos until len, incrementing *pos
 * Returns 0 on success (at least partial transfer, and ended with EAGAIN/EWOULDBLOCK)
 * Returns -1 on failure (e.g. EPIPE)
 */
static inline int send_buf(int fd, char* buf, int* pos, int len)
{
    int rc;
    while (*pos < len) {
        rc = write(fd, buf, len - *pos);
        if (rc < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
        *pos += rc;
    }
    return 0;
}

int http_send_resp_start(struct http_client* client)
{
    if (send_buf(client->sockfd, client->resp_start, &client->ta.resp_start_pos, client->ta.resp_start_len)) {
        return -1;
    }
    if (client->ta.resp_start_pos == client->ta.resp_start_len) {
        client->ta.state = HTTP_STATE_SEND_HEAD;
    }
    return 0;
}

int http_send_resp_head(struct http_client* client)
{
    if (send_buf(client->sockfd, client->resp_headers, &client->ta.resp_headers_pos, client->ta.resp_headers_len)) {
        return -1;
    }
    if (client->ta.resp_headers_pos == client->ta.resp_headers_len) {
        client->ta.state = HTTP_STATE_SEND_BODY;
    }
    return 0;
}

int http_send_resp_body(struct http_client* client)
{
    ssize_t rc;

    if (client->ta.req_method == HTTP_HEAD) {
        // we don't need to send anything
        goto done;
    }

    // have a file to send
    if (client->ta.resp_fd != 0) {
#ifdef __linux__
        do {
            rc = sendfile(client->sockfd, client->ta.resp_fd, &client->ta.resp_body_pos,
                          client->ta.resp_body_end - client->ta.resp_body_pos + 1);
        } while (rc > 0);
#else
            // TODO: send the file here using `send(2)`
            //       cycle it within the client->resp_body.buf area (use .max)
#endif
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            close_set_zero(&client->ta.resp_fd);
            return -1;
        }
        close_set_zero(&client->ta.resp_fd);
    }

    // data to send is in the body buffer
    else {
        while (client->ta.resp_body_pos < client->resp_body.len) {
            rc = write(client->sockfd, client->resp_body.buf, client->resp_body.len - client->ta.resp_body_pos);
            if (rc < 0) {
                return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
            }
            client->ta.resp_body_pos += rc;
        }
    }

done:
    client->ta.state = HTTP_STATE_DONE;
    if (status_is_error(client->ta.resp_status)) {
        return -1;
    }
    return 0;
}
