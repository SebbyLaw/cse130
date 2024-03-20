#include "seb_http.h"

#include "asgn2_helper_funcs.h"

#include <sys/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <regex.h>

// try to parse a chunk of data using a regex pattern with n_groups
#define try_parse_chunk(req, chunk_size, pattern, n_groups)                                        \
    regmatch_t matches[n_groups];                                                                  \
    const int res = parse_chunk(req, chunk_size, pattern, matches, n_groups);                      \
    if (res != 0) {                                                                                \
        return res;                                                                                \
    }

#define _BUF_EXTRA 256

typedef struct {
    // the string buffer itself
    // to make sure we never segfault,
    // we'll make this buffer a bit larger than the maximum request size
    char buf[REQ_MAX_SIZE + _BUF_EXTRA];

    // "parse cursor"
    // the current position in the buffer to start parsing from
    bufsize_t pc;
    // "write cursor"
    // the current position in the buffer to write into
    bufsize_t wc;
} InputBuffer;

struct request {
    // The input buffer for the request
    // This is used internally to store read data from the socket
    InputBuffer in;

    // socket file descriptor
    int sockfd;

    // The HTTP method used in the request
    Method method;

    // The URI of the request
    // This string is null-terminated
    char *uri;

    // The Major version of the HTTP request
    char http_ver_major;
    // The Minor version of the HTTP request
    char http_ver_minor;

    // The number of headers in the request
    int num_headers;

    // The headers in the request
    // This array is num_headers long
    // This array can be NULL if num_headers is 0
    Header *headers;

    // The size of the body of the request
    bufsize_t body_size;

    /**
     * The body of the request
     *
     * This string is NOT null-terminated
     * This string can be NULL if there is no body
     * This string is body_size bytes long
     *
     * NOTE: This is not guaranteed to be the full body, only the first body_size bytes.
     *       More data may be available from the socket.
    */
    char *body;
    // NOTE: The body pointer internally points to a location in the InputBuffer.
    //       This means that the body should not be freed separately from the request.
};

// public constructor and destructor functions

Request *req_create(const int sockfd) {
    Request *req = malloc(sizeof(Request));

    req->in.pc = 0;
    req->in.wc = 0;

    req->sockfd = sockfd;
    req->method = UNSUPPORTED;

    req->uri = NULL;

    req->http_ver_major = '0';
    req->http_ver_minor = '0';

    req->num_headers = 0;
    req->headers = NULL;

    req->body_size = 0;
    req->body = NULL;

    return req;
}

void req_close(Request *req) {
    // read the rest of the request
    // this ensures the client has read our response before we close the connection
    // directly raw recv() on the socket is the fastest way to do this.
    recv(req->sockfd, req->in.buf + req->in.wc, _BUF_EXTRA, 0);
    close(req->sockfd);
}

void req_free(Request *req) {
    if (req->uri != NULL) {
        free(req->uri);
    }

    if (req->headers != NULL) {
        for (int i = 0; i < req->num_headers; i++) {
            free(req->headers[i].key);
            free(req->headers[i].value);
        }

        free(req->headers);
    }

    free(req);
}

// internal parse functions

int parse_chunk(Request *req, const bufsize_t chunk_size, regex_t *reg, regmatch_t *matches,
    const size_t n_matches) {

    // The amount of data already read into the buffer that is not yet parsed
    const bufsize_t cur_size = req->in.wc - req->in.pc;

    if (cur_size < chunk_size && req->in.wc < REQ_MAX_SIZE) {
        // If there is not enough data in the buffer to parse the chunk, read more data

        // How much more data is needed to parse the chunk
        bufsize_t need = chunk_size - cur_size;

        // If the requested read amount would exceed the maximum buffer size, read only up to the maximum buffer size
        if (req->in.wc + need > REQ_MAX_SIZE) {
            need = REQ_MAX_SIZE - req->in.wc;
        }

        // Read the data from the socket
        const ssize_t rb = read_n_bytes(req->sockfd, req->in.buf + req->in.wc, need);
        switch (rb) {
        case 0:
        case -1:
            // if no data is read or an error occurs, consider this an invalid request
            return -1;
        // if data is read, update the write cursor
        default: req->in.wc += rb;
        }
    }

    // Assume that enough data is in the buffer to parse the chunk

    char str[req->in.wc - req->in.pc + 1];
    strncpy(str, req->in.buf + req->in.pc, req->in.wc - req->in.pc);
    str[req->in.wc - req->in.pc] = '\0';

    const int reg_res = regexec(reg, str, n_matches, matches, 0);

    if (reg_res != 0) {
        // the pattern does not match, return bad request
        return -1;
    }

    return 0;
}

/*
A valid Method contains at most eight (8) characters from the character range [a-zA-Z]. Your server
only needs to implement (i.e., perform the semantics) of GET and PUT.
*/
#define _METHOD_PATTERN "^([a-zA-Z]{1,8}) "
// 8 alphabet characters max, plus 1 to include the trailing space
#define _METHOD_CHUNK_LEN 9

static regex_t *_METHOD_REG;

int parse_method(Request *req) {
    try_parse_chunk(req, _METHOD_CHUNK_LEN, _METHOD_REG, 2);

    const bufsize_t method_len = matches[1].rm_eo - matches[1].rm_so;
    char method_str[method_len + 1];
    strncpy(method_str, req->in.buf + req->in.pc + matches[1].rm_so, method_len);
    method_str[method_len] = '\0';

    if (strcasecmp(method_str, "GET") == 0) {
        req->method = GET;
    } else if (strcasecmp(method_str, "PUT") == 0) {
        req->method = PUT;
    } else {
        req->method = UNSUPPORTED;
    }

    // move the parse cursor to the end of the match
    req->in.pc += matches[0].rm_eo;

    return 0;
}

/*
A valid URI starts with the character ‘/’, includes at least 2 characters and at most 64 characters
(including the ‘/’), and except for the leading ‘/’, only includes characters from the character set [a-zA-Z0-9.-]
(this character set includes 64 total valid characters)
*/
#define _URI_PATTERN "^/([a-zA-Z0-9\\.-]{1,63}) "
// 64 characters max, plus 1 to include the trailing space
#define _URI_CHUNK_LEN 65

static regex_t *_URI_REG;

int parse_uri(Request *req) {
    try_parse_chunk(req, _URI_CHUNK_LEN, _URI_REG, 2);

    const bufsize_t uri_len = matches[1].rm_eo - matches[1].rm_so;
    // include 1 for null terminator
    req->uri = malloc((uri_len + 1) * sizeof(char));
    strncpy(req->uri, req->in.buf + req->in.pc + matches[1].rm_so, uri_len);
    req->uri[uri_len] = '\0';

    // move the parse cursor to the end of the match
    req->in.pc += matches[0].rm_eo;

    return 0;
}

/*
A valid Version has the format HTTP/#.#, where each # is a single digit number. Your httpserver
should only implement version 1.1, so it should only perform the semantics of GET and PUT requests
that include a version equal to HTTP/1.1.
*/
#define _HTTP_VERSION_PATTERN "^HTTP/([0-9])\\.([0-9])\r\n"
// (HTTP/#.#\r\n) is 10 characters long, plus 1 to include the trailing space
#define _HTTP_VERSION_CHUNK_LEN 11

static regex_t *_HTTP_VERSION_REG;

int parse_http_version(Request *req) {
    try_parse_chunk(req, _HTTP_VERSION_CHUNK_LEN, _HTTP_VERSION_REG, 3);

    req->http_ver_major = req->in.buf[req->in.pc + matches[1].rm_so];
    req->http_ver_minor = req->in.buf[req->in.pc + matches[2].rm_so];

    // move the parse cursor to the end of the match
    req->in.pc += matches[0].rm_eo;

    return 0;
}

/*
Valid requests include zero (0) or more header-fields after request-line.
A header-field is a key-value pair with the format:

key: value\r\n

• The key ends with the first instance of a ‘:’ character. A valid request’s header-field keys will be at least
1 character, at most 128 characters, and only contain characters from the character set [a-zA-Z0-9.-].

• A valid request’s header-field values will contain at most 128 characters and only contain characters
from the set of printable ASCII characters (i.e., a valid value will not contain any ASCII “Device
Control” characters nor any other binary data).

• Valid requests separate each header-field using the sequence \r\n, and will terminate the list
of header-fields with a blank header terminating in \r\n. (Essentially, regardless of how many
header-fields a request contains, the list will terminate with the sequence \r\n\r\n).
*/

// printable ascii characters are in the range [ -~] (32-126, inclusive, ASCII space to tilde)

#define _HEADERS_PATTERN "^([a-zA-Z0-9\\.-]{1,128}: [ -~]{1,128}\r\n)*\r\n"
// 0 since we are going to circumvent the chunking of read_n_bytes
// by reading as much as possible using recv() directly
#define _HEADERS_CHUNK_LEN 0

static regex_t *_HEADERS_REG;

// Pattern that matches a single header
#define _HEADER_PATTERN "([a-zA-Z0-9\\.-]{1,128}): ([ -~]{1,128})\r\n"

static regex_t *_HEADER_REG;

int parse_headers(Request *req) {
    try_parse_chunk(req, _HEADERS_CHUNK_LEN, _HEADERS_REG, 2);

    if (matches[1].rm_so == matches[1].rm_eo) {
        // no headers, move the parse cursor to the end of the match, and return
        req->in.pc += matches[0].rm_eo;
        return 0;
    }

    int num_headers = 0;
    Header *headers = malloc(0);

    const regoff_t headers_end = req->in.pc + matches[1].rm_eo;

    // iterate over matches
    for (regmatch_t header_match[3]; req->in.pc < headers_end; num_headers++) {
        // group 1: key
        // group 2: value
        const int reg_res = regexec(_HEADER_REG, req->in.buf + req->in.pc, 3, header_match, 0);
        if (reg_res != 0) {
            // *somehow* the pattern doesn't match, return bad request
            free(headers);
            return -1;
        }

        const bufsize_t key_len = header_match[1].rm_eo - header_match[1].rm_so;
        const bufsize_t value_len = header_match[2].rm_eo - header_match[2].rm_so;

        // allocate space for the new header
        headers = realloc(headers, (num_headers + 1) * sizeof(Header));

        // the new header
        Header *header = &headers[num_headers];

        // copy the key and value into the new header
        header->key = malloc((key_len + 1) * sizeof(char));
        strncpy(header->key, req->in.buf + req->in.pc + header_match[1].rm_so, key_len);
        header->key[key_len] = '\0';

        // copy the value into the new header
        header->value = malloc((value_len + 1) * sizeof(char));
        strncpy(header->value, req->in.buf + req->in.pc + header_match[2].rm_so, value_len);
        header->value[value_len] = '\0';

        // move the parse cursor to the end of the match
        req->in.pc += header_match[0].rm_eo;
    }

    req->num_headers = num_headers;
    req->headers = headers;

    // move the parse cursor to the end of the match
    req->in.pc += 2; // 2 for the \r\n

    return 0;
}

int parse_body(Request *req) {
    // The amount of data already read into the buffer that is not yet parsed
    const bufsize_t cur_size = req->in.wc - req->in.pc;

    if (cur_size > 0) {
        req->body_size = cur_size;
        req->body = req->in.buf + req->in.pc;
    }

    // move the parse cursor up to the write cursor
    req->in.pc = req->in.wc;

    return 0;
}

// public parse function

int req_parse(Request *req) {
    if (parse_method(req) != 0) {
        return -1;
    }

    if (parse_uri(req) != 0) {
        return -1;
    }

    if (parse_http_version(req) != 0) {
        return -1;
    }

    // read as much as possible from the socket
    const ssize_t rb = recv(req->sockfd, req->in.buf + req->in.wc, REQ_MAX_SIZE - req->in.wc, 0);
    if (rb > 0) {
        req->in.wc += rb;
    }

    if (parse_headers(req) != 0) {
        return -1;
    }

    if (parse_body(req) != 0) {
        return -1;
    }

    return 0;
}

// public getters

int req_get_sockfd(const Request *req) {
    return req->sockfd;
}

Method req_get_method(const Request *req) {
    return req->method;
}

char *req_get_uri(const Request *req) {
    return req->uri;
}

char req_get_http_ver_major(const Request *req) {
    return req->http_ver_major;
}

char req_get_http_ver_minor(const Request *req) {
    return req->http_ver_minor;
}

int req_get_num_headers(const Request *req) {
    return req->num_headers;
}

Header *req_get_headers(const Request *req) {
    return req->headers;
}

char *req_get_header_value(const Request *req, const char *key) {
    for (int i = 0; i < req->num_headers; i++) {
        if (strcasecmp(req->headers[i].key, key) == 0) {
            return req->headers[i].value;
        }
    }

    return NULL;
}

// helper to convert string to long, returns -1 if invalid
// we need this because sscanf stops parsing before null-terminator in a lot of cases
ssize_t _str_to_long(const char *str) {
    ssize_t res = 0;
    for (unsigned int i = 0; str[i] != '\0'; i++) {
        res *= 10;
        switch (str[i]) {
        case '1': res += 1; break;
        case '2': res += 2; break;
        case '3': res += 3; break;
        case '4': res += 4; break;
        case '5': res += 5; break;
        case '6': res += 6; break;
        case '7': res += 7; break;
        case '8': res += 8; break;
        case '9': res += 9;
        case '0': continue;
        default: return -1;
        }
    }
    return res;
}

ssize_t req_get_content_length(const Request *req) {
    const char *content_length_str = req_get_header_value(req, "Content-Length");
    if (content_length_str == NULL) {
        // no content-length header
        return -1;
    }

    const ssize_t content_length = _str_to_long(content_length_str);
    if (content_length < 0) {
        // string is not a positive number consisting of only digits (invalid)
        return -2;
    }

    return content_length;
}

bufsize_t req_get_body_size(const Request *req) {
    return req->body_size;
}

char *req_get_body(const Request *req) {
    return req->body;
}

// public regex initialization and cleanup functions
static regex_t _regs[5];
static int _regs_initialized = 0;

void seb_http_regex_cleanup() {
    for (int i = 0; i < _regs_initialized; i++) {
        regfree(&_regs[i]);
    }
}

#define INIT_REGEX(reg)                                                                            \
    if (regcomp(&_regs[_regs_initialized], _##reg##_PATTERN, REG_EXTENDED | REG_NEWLINE) != 0) {   \
        seb_http_regex_cleanup();                                                                  \
        fprintf(stderr, "Failed to initialize %s regex pattern (%s)\n", #reg, _##reg##_PATTERN);   \
        return -1;                                                                                 \
    } else {                                                                                       \
        _##reg##_REG = &_regs[_regs_initialized++];                                                \
    }

int seb_http_regex_init() {
    INIT_REGEX(METHOD);
    INIT_REGEX(URI);
    INIT_REGEX(HTTP_VERSION);
    INIT_REGEX(HEADERS);
    INIT_REGEX(HEADER);
    return 0;
}

#undef INIT_REGEX
