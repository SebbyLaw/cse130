#include "asgn2_helper_funcs.h"

#include "queue.h"
#include "rwlock.h"
#include "seb_http.h"

#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#define RESPONSE_UNSENT(code)                                                                      \
    (Response) {                                                                                   \
        false, code                                                                                \
    }

#define RESPONSE_SENT(code)                                                                        \
    (Response) {                                                                                   \
        true, code                                                                                 \
    }

static Listener_Socket sock;
static pthread_t *threads_arr;
static int thread_count;
static volatile bool running = true;
static pthread_mutex_t file_locks_mutex = PTHREAD_MUTEX_INITIALIZER;

struct file_lock {
    rwlock_t *lock;
    char *filename;
    int users;
};

static struct file_lock *file_locks;

static struct file_lock *find_file_lock(const char *URI) {
    pthread_mutex_lock(&file_locks_mutex);
    for (int i = 0; i < thread_count; i++) {
        if (file_locks[i].filename == NULL) {
            // unused slot, use it
            file_locks[i].filename = strdup(URI);
            file_locks[i].users = 1;
            pthread_mutex_unlock(&file_locks_mutex);
            return &file_locks[i];
        } else if (strcmp(file_locks[i].filename, URI) == 0) {
            file_locks[i].users++;
            pthread_mutex_unlock(&file_locks_mutex);
            return &file_locks[i];
        }
    }
    pthread_mutex_unlock(&file_locks_mutex);
    return NULL;
}

static void release_file_lock(struct file_lock *lock) {
    pthread_mutex_lock(&file_locks_mutex);
    if (--lock->users == 0) {
        free(lock->filename);
        lock->filename = NULL;
    }
    pthread_mutex_unlock(&file_locks_mutex);
}

static void write_audit_log(const char *op, const char *URI, const int status, const char *req_id) {
    // we can assume fprintf is thread safe
    fprintf(stderr, "%s,/%s,%d,%s\n", op, URI, status, req_id);
}

Response handle_get(const Request *req) {

    const char *URI = req_get_uri(req);

    // try to open the file
    const int fd = open(URI, O_RDONLY);
    if (fd == -1) {
        switch (errno) {
        case EACCES:
        case ENAMETOOLONG:
        case EPERM:
        case EROFS: return RESPONSE_UNSENT(403);
        case ENOENT: return RESPONSE_UNSENT(404);
        default: return RESPONSE_UNSENT(500);
        }
    }

    // check if the URI is a directory using fstat
    struct stat st;
    const int st_res = fstat(fd, &st);
    if (st_res == -1) {
        switch (errno) {
        case EACCES:
        case EBADF:
        case EFAULT: close(fd); return RESPONSE_UNSENT(403);
        case ENOENT: close(fd); return RESPONSE_UNSENT(404);
        default: close(fd); return RESPONSE_UNSENT(500);
        }
    }

    if (S_ISDIR(st.st_mode)) {
        close(fd);
        return RESPONSE_UNSENT(403);
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
    pass_n_bytes(fd, sock, file_size);

    // close the file
    close(fd);

    return RESPONSE_SENT(200);
}

Response handle_put(Request *req) {
    // get the content length from the headers
    const ssize_t content_length = req_get_content_length(req);

    // if the content length is invalid, return 400
    if (content_length < 0) {
        return RESPONSE_UNSENT(400);
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
            return RESPONSE_UNSENT(403);

        case ENOENT:
            // file doesn't exist
            // we are going to create it (below)
            break;
        default: return RESPONSE_UNSENT(500);
        }

        fd = creat(URI, 0666);

        res = 201;
    } else {
        res = 200;
    }

    if (content_length == 0) {
        // no content to write, we're just done here
        close(fd);
        return RESPONSE_UNSENT(res);
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
        return RESPONSE_UNSENT(res);
    }

    // pass the rest of the body to the file
    const int sock = req_get_sockfd(req);
    pass_n_bytes(sock, fd, content_length - total_wb);

    close(fd);

    return RESPONSE_UNSENT(res);
}

Response handle_connection(Request *req) {
    if (req_parse(req) != 0) {
        return RESPONSE_UNSENT(400);
    }

    const char *request_id = req_get_header_value(req, "Request-Id");
    if (request_id == NULL) {
        return RESPONSE_UNSENT(400);
    }

    Response response;
    struct file_lock *lock;
    const char *URI = req_get_uri(req);

    switch (req_get_method(req)) {
    case GET:
        lock = find_file_lock(URI);
        reader_lock(lock->lock);
        response = handle_get(req);
        write_audit_log("GET", URI, response.status, request_id);
        reader_unlock(lock->lock);
        release_file_lock(lock);

        break;
    case PUT:
        lock = find_file_lock(URI);
        writer_lock(lock->lock);
        response = handle_put(req);
        write_audit_log("PUT", URI, response.status, request_id);
        writer_unlock(lock->lock);
        release_file_lock(lock);

        break;
    default: return RESPONSE_UNSENT(501);
    }

    return response;
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

static void signal_handler(const int n) {
    switch (n) {
    case SIGINT:
    case SIGTERM: close(sock.fd);
    default: break;
    }

    for (int i = 0; i < thread_count; i++) {
        pthread_cancel(threads_arr[i]);
    }

    running = false;
}

static void parse_command(const int argc, char *const *argv, int *port, int *threads) {
    switch (getopt(argc, argv, "t:")) {
    case 't':
        if (sscanf(optarg, "%d", threads) != 1) {
            fprintf(stderr, "Invalid thread count: %s\n", optarg);
            exit(1);
        }
        break;
    case '?':
        fprintf(stderr, "Usage: %s [-t threads] <port>\n", optarg);
        exit(1);
        break;
    default:
        // default to 4 threads
        *threads = 4;
        break;
    }

    if (optind >= argc) {
        fprintf(stderr, "Usage: %s [-t threads] <port>\n", argv[0]);
        exit(1);
    }

    if (sscanf(argv[optind], "%d", port) != 1) {
        fprintf(stderr, "Invalid port: %s\n", argv[optind]);
        exit(1);
    }
}

void *worker_thread(void *arg) {
    queue_t *queue = arg;
    Request *req;

    while (true) {
        queue_pop(queue, (void **) &req);
        Response response = handle_connection(req);

        if (!response.responded) {
            respond(req_get_sockfd(req), response.status);
        }

        req_close(req);
        req_free(req);
    }

    return NULL;
}

int main(const int argc, char *const argv[]) {
    int port, threads;
    parse_command(argc, argv, &port, &threads);

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

    // register signal handler for SIGINT, SIGTERM
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (seb_http_regex_init() != 0) {
        fprintf(stderr, "Failed to initialize regex\n");
        return 1;
    }

    queue_t *queue = queue_new(threads);
    // lol
    pthread_t _real_threads_array_but_its_on_the_stack[threads];
    threads_arr = _real_threads_array_but_its_on_the_stack;
    thread_count = threads;
    // lol again
    struct file_lock _real_file_locks_array_but_its_on_the_stack[threads];
    file_locks = _real_file_locks_array_but_its_on_the_stack;

    for (int i = 0; i < threads; i++) {
        pthread_create(&threads_arr[i], NULL, worker_thread, queue);
        file_locks[i].lock = rwlock_new(N_WAY, 1);
        file_locks[i].filename = NULL;
        file_locks[i].users = 0;
    }

    int conn;
    while (running) {
        if ((conn = listener_accept(&sock)) != -1) {
            Request *req = req_create(conn);
            queue_push(queue, req);
        }
    }

    for (int i = 0; i < threads; i++) {
        pthread_join(threads_arr[i], NULL);
        rwlock_delete(&file_locks[i].lock);
    }

    queue_delete(&queue);
    seb_http_regex_cleanup();

    return 0;
}
