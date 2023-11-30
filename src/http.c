/* Part of FileKit, licensed under the GNU Affero GPL. */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "http.h"

#include <assert.h>
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
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/sendfile.h>
#endif

#include "buffer.h"

#define bufscmp(s, target) (!memcmp(s, target, sizeof(target) - 1))

static char* server_name;
static struct http_request_context* default_context;
static char* target_cookie_name;
static struct http_api_tree* api_tree;

void http_init(char* servername, struct http_request_context* http_default_context, char* auth_cookie_name,
               struct http_api_tree* http_api_list)
{
    assert(servername);
    assert(http_default_context);
    assert(http_default_context->root);

    server_name = servername;
    default_context = http_default_context;
    target_cookie_name = auth_cookie_name;
    api_tree = http_api_list;
}

int http_create_client_struct(struct http_client* client)
{
    assert(client != NULL);
    if (!(client->req_headers = malloc(HTTP_BUFSZ * 2))) {
        goto err_reqheaders;
    }
    if (!(client->resp_start = malloc(HTTP_BUFSZ))) {
        goto err_respstart;
    }
    if (!(client->resp_headers = malloc(HTTP_BUFSZ))) {
        goto err_respheaders;
    }
    if (buffer_init(&client->resp_body, HTTP_BUFSZ)) {
        goto err_respbody;
    }
    http_reset_client(client);
    return 0;

err_respbody:
    free(client->resp_headers);
err_respheaders:
    free(client->resp_start);
err_respstart:
    free(client->req_headers);
err_reqheaders:
    return -1;
}

static inline void cleanup_client(struct http_client* client)
{
    memset(&client->ta, 0, sizeof(struct http_transaction));
    buffer_reset(&client->resp_body, HTTP_BUFSZ);
}

void http_finalize_transaction(struct http_client* client)
{
    // in case the client sent part of their next request into the buffers for this one
    // so, what we considered the payload length is actually now the header length
    const int remaining_payload = client->ta.req_payload_len - client->ta.req_payload_pos;
    assert(remaining_payload >= 0 && remaining_payload <= HTTP_BUFSZ);
    memmove(client->req_headers, &client->req_payload[client->ta.req_payload_pos], remaining_payload);
    client->req_headers_len = remaining_payload;
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
 * If the fd is already zero or negative, do nothing.
 */
static inline void close_fd_to_zero(int* fd)
{
    if (*fd > 0) {
        close(*fd);
        *fd = 0;
    }
}

/**
 * Append to buf at the given offset (which is incremented), respecting max.
 * Returns 0 on success, -1 on failure.
 * *offset is not incremented on failure, but buf[offset..max] is undefined.
 */
static inline int str_vappend(char* buf, int* offset, int max, const char* fmt, va_list* ap)
{
    int rc = vsnprintf(&buf[*offset], max - *offset, fmt, *ap);
    if (rc >= max - *offset || rc < 0) {
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

/**
 * Returns true if the given path contains attempted path traversal - /../ or similar
 */
static bool attempted_path_traversal(char* path)
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
        // consume whitespace
        for (; *p == ' ' || *p == '\t'; p++)
            ;

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

    assert(client->ta.range_requested);
    assert(client->ta.req_range != NULL);

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
            client->ta.resp_body_end = filesize - 1;
        }
    } else if (q) {
        // bytes=-YYY
        if (strtonum(q, &conv)) {
            goto bad_request;
        }
        if (conv > filesize) {
            goto not_satisfiable;
        }
        client->ta.resp_body_pos = 0;
        client->ta.resp_body_end = filesize - conv;
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

static inline int http_header_add_ap(struct http_client* client, char* key, const char* fmt, va_list* ap)
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

int http_header_add_set_cookie(struct http_client* client, const char* fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = http_header_add_ap(client, "set-cookie", fmt, &ap);
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

static inline int http_header_add_content_range(struct http_client* client, off_t start, off_t end, off_t total)
{
    return http_header_add(client, "content-range", "bytes %ld-%ld/%ld", start, end, total);
}

static inline int http_header_add_content_length(struct http_client* client, off_t length)
{
    return http_header_add(client, "content-length", "%ld", length);
}

static int http_header_add_allow(struct http_client* client)
{
    const int ALLOW_BODY_MAX = sizeof("GET, PUT, HEAD, POST, DELETE, ");
    char allow_body[ALLOW_BODY_MAX];
    int len = 0;

    if (client->ta.api_allow_flags != 0) {
        // we set allow flags when parsing the tree, so extract what the legal ones for that endpoint are
        if (client->ta.api_allow_flags & HTTP_GET) {
            len += snprintf(&allow_body[len], ALLOW_BODY_MAX - len, "GET, HEAD, ");
        }
        if (client->ta.api_allow_flags & HTTP_PUT) {
            len += snprintf(&allow_body[len], ALLOW_BODY_MAX - len, "PUT, ");
        }
        if (client->ta.api_allow_flags & HTTP_POST) {
            len += snprintf(&allow_body[len], ALLOW_BODY_MAX - len, "POST, ");
        }
        if (client->ta.api_allow_flags & HTTP_DELETE) {
            len += snprintf(&allow_body[len], ALLOW_BODY_MAX - len, "DELETE, ");
        }
        assert(len > 2);
        allow_body[len - 2] = '\0';  // cut off trailing comma
        return http_header_add(client, "allow", "%s", allow_body);
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

#define parse_past_end(ptr) (ptr - client->req_headers >= client->req_headers_len)
#define parse_advance \
    do {              \
        r++;          \
        p = r;        \
    } while (0)

    /* before jumping here, set the parse state to wherever you came from */
read_more:
    readrc = read(client->sockfd, &client->req_headers[client->req_headers_len], HTTP_BUFSZ - client->req_headers_len);
    if (readrc <= 0) {
        // a few different cases to catch:
        // 1) -1 - socket is blocking, parsed    - save parsing and return 0
        // 2) -1 - socket is blocking, unparsed  - continue to parser
        // 3) -1 - socket is error               - indicate hangup and return -1
        // 4) 0  - header buffer full, parsed    - prepare for a 431 error and return -1
        // 5) 0  - header buffer full, unparsed  - parse as much as we can, hope that it fit (continue to parser)
        // 6) 0  - socket has hit EOF            - indicate hangup and return -1
        if (readrc != 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (parse_past_end(r)) {
                    client->ta.req_parse_blk = p;
                    client->ta.req_parse_iter = r;
                    return 0;
                }
                // ignore else case here - we want to cascade to the parser below
            } else {
                client->ta.resp_status = HTTP_X_HANGUP;
                return -1;
            }
        } else {
            if (client->req_headers_len >= HTTP_BUFSZ) {
                if (parse_past_end(r)) {
                    client->ta.resp_status = HTTP_431_REQUEST_HEADER_FIELDS_TOO_LARGE;
                    return -1;
                }
                // ignore else case here - we want to cascade to the parser below
            } else {
                client->ta.resp_status = HTTP_X_HANGUP;
                return -1;
            }
        }
    } else {
        client->req_headers_len += readrc;
    }

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
                goto read_more;
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
                    goto method_not_recognized;
                case 4:
                    if (bufscmp(p, "HEAD ")) {
                        client->ta.req_method = HTTP_HEAD;
                        break;
                    } else if (bufscmp(p, "POST ")) {
                        client->ta.req_method = HTTP_POST;
                        break;
                    }
                    goto method_not_recognized;
                case 6:
                    if (bufscmp(p, "DELETE ")) {
                        client->ta.req_method = HTTP_DELETE;
                        break;
                    }
                    goto method_not_recognized;
                default:
method_not_recognized:
                    /*
                     * It appears as though we should return 405, but that would:
                     * "indicate that the method received ... is known by the origin server"
                     * Since we don't recognize the method, a 400 is more apt
                     */
                    client->ta.req_method = HTTP_GET;  // default on errors
                    goto bad_request;
            }
            parse_advance;
            /* fallthrough */

        case HTTP_PS_REQ_PATH:
            /* '/request/path?query '
             *  ^            ^     ^
             *  p            s     r/q
             */
            for (; !parse_past_end(r) && *r != ' '; r++)
                ;
            if (parse_past_end(r)) {
                client->ta.parse_state = HTTP_PS_REQ_PATH;
                goto read_more;
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
            if (attempted_path_traversal(client->ta.req_path)) {
                goto bad_request;
            }
            parse_advance;
            /* fallthrough */

        case HTTP_PS_REQ_VERSION:
            /* 'HTTP/1.1\r' */
            for (; !parse_past_end(r) && *r != '\r'; r++)
                ;
            if (parse_past_end(r)) {
                client->ta.parse_state = HTTP_PS_REQ_VERSION;
                goto read_more;
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
                goto read_more;
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
                goto read_more;
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
                goto read_more;
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
    client->ta.req_payload_len = client->req_headers_len - (client->req_payload - client->req_headers);
    client->ta.state = HTTP_STATE_SERVE;
    return 0;

bad_request:
    client->ta.resp_status = HTTP_400_BAD_REQUEST;
    return -1;

#undef parse_past_end
#undef parse_advance
}

/**
 * Verify a given path, stat'ing it into *st
 * Returns 0 on success, or -1 on error (catastrophic errors also set resp_status)
 * assumed_pathlen is the size that path was attempted to be, which may be either invalid or too long (>= PATH_MAX)
 * (if it is so, then the status is set as 414_URI_TOO_LONG)
 */
static int verify_static_path(struct stat* st, struct http_client* client, int assumed_pathlen, char* path)
{
    if (assumed_pathlen < 0 || assumed_pathlen >= PATH_MAX) {
        client->ta.resp_status = HTTP_414_URI_TOO_LONG;
    } else if (!stat(path, st) && S_ISREG(st->st_mode)) {
        return 0;
    }
    if (errno == EACCES) {
        client->ta.resp_status = HTTP_403_PERMISSION_DENIED;
    }
    return -1;
}

int http_handle_static_path(struct http_client* client, char* path, struct http_request_context* ctx)
{
    char fname[PATH_MAX];
    struct stat st;
    struct tm tm;
    int rc;

    if (!ctx) {
        ctx = default_context;
    }

    if (!(client->ta.req_method & HTTP_GET)) {
        // we only allow GET or HEAD here
        client->ta.resp_status = HTTP_405_METHOD_NOT_ALLOWED;
        return -1;
    }

    // regular direct path or the root fallback
    if (!strcmp(client->ta.req_path, "/") && ctx->root_fallback) {
        rc = snprintf(fname, PATH_MAX, "%s/%s", ctx->root, ctx->root_fallback);
    } else {
        rc = snprintf(fname, PATH_MAX, "%s/%s", ctx->root, path);
    }
    if (verify_static_path(&st, client, rc, fname)) {
        if (client->ta.resp_status) {
            return -1;
        }
    } else {
        goto path_set;
    }

    // failed standard, append .html and see if it exists
    if (ctx->use_http_append_fallback) {
        rc = snprintf(fname, PATH_MAX, "%s/%s.html", ctx->root, path);
        if (verify_static_path(&st, client, rc, fname)) {
            if (client->ta.resp_status) {
                return -1;
            }
        } else {
            goto path_set;
        }
    }
    // failed .http append, serve the generic fallback
    if (ctx->fallback) {
        rc = snprintf(fname, PATH_MAX, "%s/%s", ctx->root, ctx->fallback);
        if (verify_static_path(&st, client, rc, fname)) {
            if (client->ta.resp_status) {
                return -1;
            }
        } else {
            goto path_set;
        }
    }

    // could not find a path from above, abort
    client->ta.resp_status = HTTP_404_NOT_FOUND;
    return -1;

path_set:

    // don't open on a HEAD - we already got our info from the stat
    if (client->ta.req_method == HTTP_GET) {
        rc = open(fname, O_RDONLY);
        if (rc < 0) {
            client->ta.resp_status = HTTP_500_INTERNAL_ERROR;
            return -1;
        }
        client->ta.resp_fd = rc;
    }

    if (client->ta.range_requested) {
        // we have a range request, parse it and set the header, erroring if necessary
        if (parse_range(client, st.st_size) ||
            http_header_add_content_range(client, client->ta.resp_body_pos, client->ta.resp_body_end, st.st_size)) {
            close_fd_to_zero(&client->ta.resp_fd);
            return -1;
        }
    } else {
        // resp_body_pos already set - 0
        client->ta.resp_body_end = st.st_size - 1;
    }

    // add content type, accept-ranges, and last modified headers
    if (http_header_add_content_type_guess(client, strrchr(fname, '.')) ||
        http_header_add(client, "accept-ranges", "bytes") || http_header_add_last_modified(client, st.st_mtim.tv_sec)) {
        close_fd_to_zero(&client->ta.resp_fd);
        return -1;
    }

    if (client->ta.req_modified_since) {
        if (!strptime(client->ta.req_modified_since, "%a, %d %b %Y %T GMT", &tm)) {
            client->ta.resp_status = HTTP_400_BAD_REQUEST;
            close_fd_to_zero(&client->ta.resp_fd);
            return -1;
        }
        if (difftime(st.st_mtim.tv_sec, timegm(&tm)) <= 0) {
            client->ta.resp_status = HTTP_304_NOT_MODIFIED;
            client->ta.req_method = HTTP_HEAD;  // since the response is otherwise identical
            close_fd_to_zero(&client->ta.resp_fd);
            return 0;
        }
        // else cascade below for a standard response
    }
    if (client->ta.range_requested) {
        client->ta.resp_status = HTTP_206_PARTIAL_CONTENT;
    } else {
        client->ta.resp_status = HTTP_200_OK;
    }

    return 0;
}

/**
 * Parse API tree for a given client, writing their handler if any is found (otherwise unchanged).
 * Return 0 if parsing either succeeded or found no matches.
 * Return -1 if parsing found a match for the path, but not the method.
 */
static int parse_api_tree(struct http_client* client, char* path, struct http_api_tree* current_tree)
{
    int i;
    char *q, *r;

    /* /request/path
     *  ^      ^
     *  path   q
     */

recurse:
    for (q = path; *q && *q != '/'; q++)
        ;
    for (i = 0; i < current_tree->num_entries; i++) {
        if (current_tree->entries[i].prefix_length == q - path &&
            !strncmp(path, current_tree->entries[i].prefix, q - path)) {
            // if the entry should finish the current path, make sure it does
            if (current_tree->entries[i].finishes_path) {
                for (r = q; *r == '/'; r++)
                    ;
                if (*r) {
                    // didn't finish path
                    continue;
                }
            }
            // matched an endpoint, see if method matches
            client->ta.api_allow_flags |= current_tree->entries[i].method;
            if (client->ta.req_method & current_tree->entries[i].method) {
                client->ta.api_endpoint_hit = current_tree->entries[i].handler;
                return 0;
            }
        }
    }
    if (client->ta.api_allow_flags) {
        // we hit an endpoint but didn't match any of its methods - stop parsing
        client->ta.resp_status = HTTP_405_METHOD_NOT_ALLOWED;
        return -1;
    }
    for (i = 0; i < current_tree->num_subtrees; i++) {
        if (current_tree->subtrees[i].prefix_length == q - path &&
            !strncmp(path, api_tree->subtrees[i].prefix, q - path)) {
            // return parse_api_tree(client, q + 1, &current_tree->subtrees[i]);
            current_tree = &current_tree->subtrees[i];
            path = q + 1;
            goto recurse;
        }
    }
    return 0;
}

int http_serve_request(struct http_client* client)
{
    char* p;

    if (api_tree) {
        // haven't been here yet, parse the tree and see if we hit it
        if (!client->ta.api_endpoint_hit) {
            assert(client->ta.api_internal_data == NULL);
            assert(client->ta.api_allow_flags == 0);
            // cut off leading /, if it exists
            for (p = client->ta.req_path; *p == '/'; p++)
                ;
            if (parse_api_tree(client, p, api_tree)) {
                goto cont;
            }
        }

        // hit an endpoint, call to it
        if (client->ta.api_endpoint_hit) {
            client->ta.api_endpoint_hit(client);
            // they must set resp_status to indicate advancement
            if (client->ta.resp_status == HTTP_X_RESP_STATUS_MISSING) {
                return 0;
            }
            goto cont;
        }
    }
    // if here, it's an internal request (either because it didn't match or there was no API tree)
    http_handle_static_path(client, client->ta.req_path, NULL);
cont:
    client->ta.state = HTTP_STATE_PREPARE_RESPONSE;
    return 0;
}

static inline const char* get_version_string(enum http_version version)
{
    // space at the end is relevant for easy append to status
    if (version == HTTP_1_1) {
        return "HTTP/1.1 ";
    } else if (version == HTTP_1_0) {
        return "HTTP/1.0 ";
    } else {
        fprintf(stderr, "Unknown HTTP version: %d (using HTTP/1.1)\n", version);
        return "HTTP/1.1 ";
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
        case HTTP_304_NOT_MODIFIED:
            return "304 Not Modified\r\n";
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
        case HTTP_X_RESP_STATUS_MISSING:
            fprintf(stderr, "Status missing while getting status string\n");
            /* fallthrough */
        case HTTP_500_INTERNAL_ERROR:
        default:
            return "500 Internal Server Error\r\n";
    }
}

static inline void prepare_resp_start(struct http_client* client)
{
    // should always fit, unless some idiot changed HTTP_BUFSZ to be really small
    client->ta.resp_start_len = 0;
    str_append(client->resp_start, &client->ta.resp_start_len, HTTP_BUFSZ, "%s",
               get_version_string(client->ta.req_version));
    str_append(client->resp_start, &client->ta.resp_start_len, HTTP_BUFSZ, "%s",
               get_status_string(client->ta.resp_status));
}

/**
 * Make an error response on a given client
 * If this fails, you're having a very bad time
 */
static int prepare_fresh_error_response(struct http_client* client)
{
    int rc;

    client->ta.resp_headers_len = 0;
    client->resp_body.len = 0;
    close_fd_to_zero(&client->ta.resp_fd);

    if ((rc = http_header_add_content_type(client, "text/plain"))) {
        return rc;
    }

    switch (client->ta.resp_status) {
        case HTTP_400_BAD_REQUEST:
            return buffer_append(&client->resp_body, "Bad request.", sizeof("Bad request.") - 1);
        case HTTP_403_PERMISSION_DENIED:
            return buffer_append(&client->resp_body, "Permission denied.", sizeof("Permission denied.") - 1);
        case HTTP_404_NOT_FOUND:
            if (buffer_append(&client->resp_body, "Not found: ", sizeof("Not found: ") - 1)) {
                return -1;
            }
            return buffer_append(&client->resp_body, client->ta.req_path, strlen(client->ta.req_path));
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
    return 0;
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

success:
    prepare_resp_start(client);

    // still need to add some headers here: content-length and server
    // others should have been set already

    // different measurements based on whether we're sending a file or the body buffer
    // can't just check if the ta.resp_fd is 0, since HEAD requests will leave it alone
    // instead, we require that ta.resp_body_end is 0 when sending body buffer, so use that
    if (client->ta.resp_body_end == 0) {
        if (http_header_add_content_length(client, client->resp_body.len - client->ta.resp_body_pos)) {
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

    client->ta.state = HTTP_STATE_SEND;
    return 0;

error_response:
    // if we error here or end up back here, it is highly unlikely to be salvageable so just drop the connection
    if (already_errored) {
        fprintf(stderr, "Unsalvageable handling during error number %d.\n", client->ta.resp_status);
        return -1;
    }
    if (!client->ta.api_endpoint_hit || client->ta.resp_fd < 0) {
        // our error or the client wants to use the default error response
        if (prepare_fresh_error_response(client)) {
            return -1;
        }
    }
    already_errored = true;
    goto success;
}

int http_send_response(struct http_client* client)
{
    ssize_t rc;
    struct iovec sendbufs[3];
    int sendbufs_idx;
    bool sent_start, sent_head, sent_body;

    // TODO: have some status to know if we need to do this initial loop or we can jump directly to the sendfile bit

    do {
        // load buffers that need to be sent
        sendbufs_idx = 0;
        sent_start = false, sent_head = false, sent_body = false;
        if (client->ta.resp_start_pos < client->ta.resp_start_len) {
            sendbufs[sendbufs_idx].iov_base = &client->resp_start[client->ta.resp_start_pos];
            sendbufs[sendbufs_idx].iov_len = client->ta.resp_start_len - client->ta.resp_start_pos;
            sendbufs_idx++;
            sent_start = true;
        }
        if (client->ta.resp_headers_pos < client->ta.resp_headers_len) {
            sendbufs[sendbufs_idx].iov_base = &client->resp_headers[client->ta.resp_headers_pos];
            sendbufs[sendbufs_idx].iov_len = client->ta.resp_headers_len - client->ta.resp_headers_pos;
            sendbufs_idx++;
            sent_head = true;
        }
        if (client->ta.resp_fd <= 0 && client->ta.req_method != HTTP_HEAD &&
            client->ta.resp_body_pos <= client->ta.resp_body_end) {
            sendbufs[sendbufs_idx].iov_base = &client->resp_body.buf[client->ta.resp_body_pos];
            sendbufs[sendbufs_idx].iov_len = client->resp_body.len - client->ta.resp_body_pos;
            sendbufs_idx++;
            sent_body = true;
        }

        // send the buffers that got filled in
        rc = writev(client->sockfd, sendbufs, sendbufs_idx);
        if (rc < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }

        // increment our send variables - take bytes out of rc, up to the space that's unsent
        if (sent_start) {
            client->ta.resp_start_pos += rc;
            // set rc to the number of bytes not consumed here, or negative if we couldn't finish this segment
            rc = client->ta.resp_start_pos - client->ta.resp_start_len;
        }
        if (sent_head && rc > 0) {
            client->ta.resp_headers_pos += rc;
            rc = client->ta.resp_headers_pos - client->ta.resp_headers_len;
        }
        if (sent_body && rc > 0) {
            client->ta.resp_body_pos += rc;
            rc = client->ta.resp_body_pos - client->resp_body.len;
        }
        assert(rc <= 0);  // should have either consumed all bytes or have more to send

    } while (rc < 0);  // while rc is below 0, we didn't send everything we wanted to

    // have a file to send
    if (client->ta.resp_fd > 0) {
#ifdef __linux__
        do {
            rc = sendfile(client->sockfd, client->ta.resp_fd, &client->ta.resp_body_pos,
                          client->ta.resp_body_end - client->ta.resp_body_pos + 1);
        } while (rc > 0);
#else
        // TODO: send the file here using `send(2)`
        fprintf(stderr, "Cannot send file.\n");
        client->ta.resp_body_pos = client->ta.resp_body_end + 1;
#endif
        if (client->ta.resp_body_pos > client->ta.resp_body_end) {
            // finished sending the file - pos should be one greater than end here
            close_fd_to_zero(&client->ta.resp_fd);
        } else if (rc < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
    }

    client->ta.state = HTTP_STATE_DONE;
    if (status_is_error(client->ta.resp_status) || client->ta.req_version == HTTP_1_0) {
        return -1;
    }
    return 0;
}
