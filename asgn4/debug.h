#pragma once

#include <stdio.h>

#ifdef DEBUG
#define debug(...)                                                                                 \
    do {                                                                                           \
        fprintf(stdout, "[%s:%s():%d]\t", __FILE__, __func__, __LINE__);                           \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
    } while (0);
#else
#define debug(...) ((void) 0)
#endif

#ifdef DEBUG
#define fdebug(stream, ...)                                                                        \
    do {                                                                                           \
        fprintf(stream, "[%s:%s():%d]\t", __FILE__, __func__, __LINE__);                           \
        fprintf(stream, __VA_ARGS__);                                                              \
        fprintf(stream, "\n");                                                                     \
    } while (0);
#else
#define fdebug(stream, ...) ((void) 0)
#endif
