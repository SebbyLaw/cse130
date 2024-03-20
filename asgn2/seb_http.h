/**
 * @file seb_http.h
 *
 * Seb's HTTP utility functions
 *
 * @author Sebastian Law
*/

#pragma once

#include <stdbool.h>
#include <stdio.h>

// Request max size as defined by the assignment
#define REQ_MAX_SIZE 2048
// int is currently large enough to describe positions in the buffer (2048)
typedef int bufsize_t;

/**
 * @enum Method
 * @brief Enumerated type for HTTP methods
*/
typedef enum {
    GET,
    PUT,
    UNSUPPORTED,
} Method;

/**
 * @struct Header
 * @brief Structure that contains information about a HTTP header
*/
typedef struct {
    // The header's key
    // This string is null-terminated
    char *key;

    // The header's value
    // This string is null-terminated
    char *value;
} Header;

/**
 * @struct Request
 * @brief Structure that contains information about a HTTP request
*/
typedef struct request Request;

/**
 * @brief Creates a new Request structure from a socket file descriptor
 *
 * @param sockfd The socket file descriptor to read from. The socket must be connected.
*/
Request *req_create(const int sockfd);

/**
 * @brief Cleanly closes the underlying socket file descriptor
 * @param req The Request structure to close the socket of
*/
void req_close(Request *req);

/**
 * @brief Closes the connection and frees the Request structure
 *
 * @param req The Request structure to free
*/
void req_free(Request *req);

/**
 * @brief Returns the socket file descriptor of the request
 *
 * @param req The Request structure to get the socket file descriptor from
 * @return The socket file descriptor of the request
*/
int req_get_sockfd(const Request *req);

/**
 * @brief Parses the request from the socket file descriptor
 *
 * @param req The Request structure to parse into
 * @return 0 if successful, -1 if the request is invalid
*/
int req_parse(Request *req);

/**
 * @brief Returns the method of the request
 *
 * @param req The Request structure to get the method from
 * @return The method of the request
*/
Method req_get_method(const Request *req);

/**
 * @brief Returns the URI of the request
 *
 * @param req The Request structure to get the URI from
 * @return The URI of the request
*/
char *req_get_uri(const Request *req);

/**
 * @brief Returns the major HTTP version of the request
 *
 * @param req The Request structure to get the version from
 * @return The version of the request
*/
char req_get_http_ver_major(const Request *req);

/**
 * @brief Returns the minor HTTP version of the request
 *
 * @param req The Request structure to get the version from
 * @return The version of the request
*/
char req_get_http_ver_minor(const Request *req);

/**
 * @brief Returns the number of headers in the request
 *
 * @param req The Request structure to get the headers from
 * @return The number of headers in the request
*/
int req_get_num_headers(const Request *req);

/**
 * @brief Returns the headers of the request
 *
 * @param req The Request structure to get the headers from
 * @return The headers of the request
*/
Header *req_get_headers(const Request *req);

/**
 * @brief Returns the value of a header in the request
 *
 * @param req The Request structure to get the header value from
 * @param key The key of the header to get the value of
 * @return The value of the header in the request. NULL if the header does not exist in the request.
*/
char *req_get_header_value(const Request *req, const char *key);

/**
 * @brief Returns the Content-Length of the request
 *
 * @param req The Request structure to get the Content-Length from
 * @return The Content-Length of the request.
 * -1 if the request does not have a Content-Length header.
 * -2 if the Content-Length header is invalid (not a number or negative).
*/
ssize_t req_get_content_length(const Request *req);

/**
 * @brief Returns the body of the request
 *
 * @param req The Request structure to get the body from
 * @return The body of the request
*/
bufsize_t req_get_body_size(const Request *req);

/**
 * @brief Returns the body of the request
 *
 * @param req The Request structure to get the body from
 * @return The body of the request. Can be NULL if the request has no body.
*/
char *req_get_body(const Request *req);

typedef struct response {
    // Boolean to indicate if the response has already been sent to the client
    bool responded;
    // The status code of the response
    int status;
} Response;

/**
 * Initializes regex for parsing HTTP requests
 *
 * This should be called once at the beginning of your program before any other functions in this module are called
 *
 * Returns 0 if successful, -1 if failed
*/
int seb_http_regex_init();

/**
 * Cleans up regex for parsing HTTP requests
 *
 * This should be called once at the end of your program to free memory claimed by seb_http_regex_init
 *
 * You should not call this function if seb_http_regex_init has not been called
 * or the call to seb_http_regex_init failed (returned -1)
*/
void seb_http_regex_cleanup();
