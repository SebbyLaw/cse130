#include <linux/limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

// sweet spot between too small (requires many reads and writes) and too large (stack gets big and slow)
#define MEM_BUF_SIZE 4096

void err_invalid_command() {
    fprintf(stderr, "Invalid Command\n");
    exit(1);
}

void err_operation_failed() {
    fprintf(stderr, "Operation Failed\n");
    exit(1);
}

/**
 * Get the file name from stdin
 *
 * The returned string must be freed by the caller
*/
char *read_location_string() {
    // string to hold the location
    char location[PATH_MAX + 1];
    int i, r;

    // read the location from stdin
    for (i = 0; i < (PATH_MAX + 1); i++) {
        // read a single character from stdin
        r = read(STDIN_FILENO, &location[i], 1);
        if (r == -1) {
            // error reading from stdin, exit with error
            err_operation_failed();
        } else if (r == 0) {
            // EOF reached, exit with error
            err_invalid_command();
        } else if (location[i] == '\n') {
            // newline reached, break out of the loop
            location[i] = '\0';
            break;
        }
    }

    if (location[0] == '\0') {
        // empty location, exit with error
        err_invalid_command();
    }

    if (i > PATH_MAX) {
        // location is too long, exit with error

        err_invalid_command();
    }

    char *location_ptr = malloc((i + 1) * sizeof(char));

    if (location_ptr == NULL) {
        // error allocating memory, exit with error
        err_operation_failed();
    }

    strcpy(location_ptr, location);

    return location_ptr;
}

/**
 * Write the buffer to the file descriptor
 * This function will keep writing to the file descriptor until all the bytes are written
 * or an error occurs
 *
 * This function returns -1 if an error occurs, otherwise it returns 0
*/
int write_to_fd(const int fd, const char *buf, const int len) {
    int total_wb = 0;
    int wb;
    while (total_wb < len) {
        wb = write(fd, &buf[total_wb], len - total_wb);
        if (wb == -1) {
            // error writing to file, return -1
            return -1;
        }
        total_wb += wb;
    }

    return 0;
}

/**
 * Implementation of the get command
*/
void get_command() {
    // format: `get\n<location>\n`

    char *location = read_location_string();

    // check if there are any more characters after the newline
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        // there are more characters after the newline, exit with error
        free(location);
        err_invalid_command();
    }

    int fd = open(location, O_RDONLY);

    free(location);

    if (fd == -1) {
        // file not found, exit with error
        err_invalid_command();
    }

    // read the file contents
    char buf[MEM_BUF_SIZE];
    int rb;
    while ((rb = read(fd, buf, MEM_BUF_SIZE)) > 0) {
        if (write_to_fd(STDOUT_FILENO, buf, rb) == -1) {
            // error writing to stdout, exit with error
            close(fd);
            err_operation_failed();
        }
    }

    close(fd);

    if (rb == -1) {
        // error reading file, exit with error
        err_invalid_command();
    }
}

/**
 * Implementation of the set command
*/
void set_command() {
    // format: `set\n<location>\n<content_length>\n<contents>`
    // <content_length> is an integer in bytes
    char *location = read_location_string();

    // read the content length from stdin
    unsigned content_length = 0;
    char c;
    int rc;
    while (1) {
        rc = read(STDIN_FILENO, &c, 1);

        if (rc == -1) {
            // error reading from stdin, exit with error
            free(location);
            err_operation_failed();
        } else if (rc == 0) {
            // EOF reached, exit with error
            // This should error since we expect a newline after the content length, not EOF
            free(location);
            err_invalid_command();
        }

        if (c == '\n') {
            break;
        }

        if (c < '0' || c > '9') {
            // invalid character (not a digit), exit with error
            free(location);
            err_invalid_command();
        }

        content_length *= 10;
        content_length += c - '0';
    }

    // now try to open the file for writing
    // open file for writing, or create it if it doesn't exist
    int fd = open(location, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    free(location);

    if (fd == -1) {
        // error opening file, exit with error
        err_operation_failed();
    }

    // read the contents from stdin
    int buf_size;
    if (content_length < MEM_BUF_SIZE) {
        buf_size = content_length;
    } else {
        buf_size = MEM_BUF_SIZE;
    }
    char buf[buf_size];
    int rb; // read bytes
    unsigned int total_rb = 0; // total read bytes from stdin
    while (total_rb < content_length) {
        rb = read(STDIN_FILENO, buf, buf_size);

        if (rb == -1) {
            // error reading from stdin, exit with error
            close(fd);
            err_operation_failed();
        } else if (rb == 0) {
            // EOF reached, just break out of the loop
            break;
        }

        if (write_to_fd(fd, buf, rb) == -1) {
            // error writing to file, exit with error
            close(fd);
            err_operation_failed();
        }

        total_rb += rb;
    }

    close(fd);

    // successful set command will always write "OK\n" to stdout
    printf("OK\n");
}

int main() {
    // Valid commands will always be 3 characters long
    // (either "get" or "set")
    // the 4th character must be a newline for the command to be considered valid,
    // so let's just check it here by reading 4 characters from stdin

    char command[4];
    int rb = 0;
    while (rb < 4) {
        int rc = read(STDIN_FILENO, &command[rb], 4 - rb);
        switch (rc) {
        case -1:
            // error reading from stdin, exit with error
            err_operation_failed();
        case 0:
            // EOF reached, exit with error
            err_invalid_command();
        }

        rb += rc;
    }

    // Check to see if the command is "get"
    if (strcmp(command, "get\n") == 0) {
        // get command
        get_command();
    } else if (strcmp(command, "set\n") == 0) {
        // set command
        set_command();
    } else {
        // invalid command, exit with return code 1 and write to stderr
        err_invalid_command();
    }

    return 0;
}
