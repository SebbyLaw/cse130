#include "asgn2_helper_funcs.h"

#include "seb_http.h"

#include <sys/stat.h>
#include <sys/socket.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#define _PNB_BUF_SIZE 4096

/**
 * pass_n_bytes from the provided helper files does not return the correct number of bytes written.
*/
ssize_t pass_n_bytes_fr(const int src, const int dst, const ssize_t n) {
    char buf[_PNB_BUF_SIZE];
    ssize_t total_wb = 0;
    int rb, wb;

    while (total_wb < n) {
        unsigned int to_read = n - total_wb;
        if (to_read > _PNB_BUF_SIZE) {
            to_read = _PNB_BUF_SIZE;
        }

        rb = read_n_bytes(src, buf, to_read);
        if (rb == -1) {
            return -1;
        }

        wb = write_n_bytes(dst, buf, rb);
        if (wb == -1) {
            return -1;
        }

        total_wb += wb;
    }
    return total_wb;
}

/**
 * @brief Validates a request after it has been parsed.
 *
 * @param req The request to validate.
 *
 * @return 0 if the request is valid, or a status code if the request is invalid.
*/
int validate_request(const Request *req) {
    // check the method of the request
    switch (req_get_method(req)) {
    // GET and PUT are the only methods we support
    case GET:
        if (req_get_body_size(req) != 0) {
            // GET requests should not have a body
            return 400;
        }
    case PUT: break;
    default: return 501;
    }

    // check the HTTP version is 1.1
    if (req_get_http_ver_major(req) != '1' || req_get_http_ver_minor(req) != '1') {
        return 505;
    }

    return 0;
}

int handle_get(const Request *req) {

    const char *URI = req_get_uri(req);

    // try to open the file
    const int fd = open(URI, O_RDONLY);
    if (fd == -1) {
        switch (errno) {
        case EACCES:
        case ENAMETOOLONG:
        case EPERM:
        case EROFS: return 403;
        case ENOENT: return 404;
        default: return 500;
        }
    }

    // check if the URI is a directory using fstat
    struct stat st;
    const int st_res = fstat(fd, &st);
    if (st_res == -1) {
        switch (errno) {
        case EACCES:
        case EBADF:
        case EFAULT: close(fd); return 403;
        case ENOENT: close(fd); return 404;
        default: close(fd); return 500;
        }
    }

    if (S_ISDIR(st.st_mode)) {
        close(fd);
        return 403;
    }

    // get the file size
    const off_t file_size = st.st_size;
    const int sock = req_get_sockfd(req);

    write_n_bytes(sock, "HTTP/1.1 200 OK\r\n", 17);
    // write file size
    char file_size_str[64];
    sprintf(file_size_str, "Content-Length: %lu\r\n", file_size);
    write_n_bytes(sock, file_size_str, strlen(file_size_str));
    write_n_bytes(sock, "\r\n", 2);

    // send the file directly to the client
    pass_n_bytes_fr(fd, sock, file_size);

    // close the file
    close(fd);

    return -1;
}

int handle_put(Request *req) {
    // get the content length from the headers
    const ssize_t content_length = req_get_content_length(req);

    // if the content length is invalid, return 400
    if (content_length < 0) {
        return 400;
    }

    const char *URI = req_get_uri(req);
    const bufsize_t body_size = req_get_body_size(req);

    int fd = open(URI, O_WRONLY | O_TRUNC, 0);
    int res;
    if (fd == -1) {
        switch (errno) {
        case EISDIR: // is directory
        case EACCES: // no access
        case ENAMETOOLONG: // name too long
        case EPERM: // no permission
        case EROFS: // readonly file
            return 403;

        case ENOENT:
            // file doesn't exist
            // we are going to create it (below)
            break;
        default: return 500;
        }

        fd = creat(URI, 0666);

        res = 201;
    } else {
        res = 200;
    }

    if (body_size > content_length) {
        // the body is too long, return 400
        close(fd);
        if (res == 201) {
            // we created the file, so we should delete it
            unlink(URI);
        } else {
            // truncate the file, so we don't have any partial data
            truncate(URI, 0);
        }
        return 400;
    }

    if (content_length == 0) {
        // no content to write, we're just done here
        close(fd);
        return res;
    }

    ssize_t total_wb = 0;
    ssize_t wb;

    if (body_size > 0) {
        // write the body that's already in the buffer
        char *body = req_get_body(req);
        wb = write_n_bytes(fd, body, body_size);
        total_wb += wb;
    }

    if (total_wb == content_length) {
        // we're done
        close(fd);
        return res;
    }

    // pass the rest of the body to the file
    const int sock = req_get_sockfd(req);
    wb = pass_n_bytes_fr(sock, fd, content_length - total_wb);
    total_wb += wb;

    close(fd);

    if (total_wb < content_length) {
        // we didn't write the entire body
        if (res == 201) {
            // we created the file, so we should delete it
            unlink(URI);
        } else {
            // truncate the file, so we don't have any partial data
            truncate(URI, 0);
        }
        return 400;
    }

    // check if there's any more data in the buffer
    const int left = recv(sock, NULL, 1, MSG_PEEK);

    if (left > 0) {
        // there's more data in the buffer, return 400
        if (res == 201) {
            // we created the file, so we should delete it
            unlink(URI);
        } else {
            // truncate the file, so we don't have any partial data
            truncate(URI, 0);
        }
        return 400;
    }

    return res;
}

int handle_connection(Request *req) {
    if (req_parse(req) != 0) {
        return 400;
    }

    int validated = validate_request(req);
    if (validated != 0) {
        return validated;
    }

    switch (req_get_method(req)) {
    case GET: return handle_get(req);
    case PUT: return handle_put(req);
    default: return 501;
    }
}

/**
 * Responds with pre-written responses based on the status code.
 * Any errors during writing are ignored.
*/
void respond(const int conn, const int status) {
    char *status_line, *body;

    switch (status) {
    case 200:
        status_line = "200 OK";
        body = "OK\n";
        break;
    case 201:
        status_line = "201 Created";
        body = "Created\n";
        break;
    case 400:
        status_line = "400 Bad Request";
        body = "Bad Request\n";
        break;
    case 403:
        status_line = "403 Forbidden";
        body = "Forbidden\n";
        break;
    case 404:
        status_line = "404 Not Found";
        body = "Not Found\n";
        break;
    case 501:
        status_line = "501 Not Implemented";
        body = "Not Implemented\n";
        break;
    case 505:
        status_line = "505 Version Not Supported";
        body = "Version Not Supported\n";
        break;
    case 500:
    default:
        // also return 500 if we somehow try to return an invalid status code
        status_line = "500 Internal Server Error";
        body = "Internal Server Error\n";
    }

    /*
    HTTP/1.1 <status_line>\r\n
    Content-Length: <length>\r\n
    \r\n
    <body>
    */

    // write status line
    write_n_bytes(conn, "HTTP/1.1 ", 9);
    write_n_bytes(conn, status_line, strlen(status_line));

    write_n_bytes(conn, "\r\n", 2);

    // now write headers
    // content length (for curl to work properly)
    write_n_bytes(conn, "Content-Length: ", 16);
    char content_len[130];
    sprintf(content_len, "%lu\r\n", strlen(body));
    write_n_bytes(conn, content_len, strlen(content_len));

    write_n_bytes(conn, "\r\n", 2);

    // now write body
    write_n_bytes(conn, body, strlen(body));
}

static Listener_Socket sock;

static void sigint_handler(const int n) {
    if (n == SIGINT) {
        seb_http_regex_cleanup();
        close(sock.fd);
    }

    exit(0);
}

int main(const int argc, const char *argv[]) {
    // make sure we can access argv[1]
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    // get port from argv[1]
    int port;
    if (sscanf(argv[1], "%d", &port) != 1) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }

    // make sure the port is in the valid range
    if (port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", port);
        return 1;
    }

    sock.fd = 0;
    // try to listen on the port, if it fails print Invalid port: <port>
    if (listener_init(&sock, port) == -1) {
        fprintf(stderr, "Invalid port: %d\n", port);
        return 1;
    }

    // register signal handler for SIGINT
    signal(SIGINT, sigint_handler);

    if (seb_http_regex_init() != 0) {
        fprintf(stderr, "Failed to initialize regex\n");
        return 1;
    }

    // le listening infinite loop
    int conn;
    int response;
    while (true) {
        if ((conn = listener_accept(&sock)) != -1) {
            Request *req = req_create(conn);

            response = handle_connection(req);

            if (response != -1) {
                respond(conn, response);
            }

            req_close(req);
            req_free(req);
        }
    }

    seb_http_regex_cleanup();

    return 0;
}
