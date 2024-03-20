#include "rwlock.h"

#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>

// State for the N_WAY priority rwlock
typedef struct {
    // The "N" in N_WAY priority
    uint32_t n;
    // indicates how many readers are currently waiting for the lock
    uint32_t readers_waiting;
    // indicates how many readers have passed while the lock was being waited for by a writer
    uint32_t readers_passed;
    // indicates how many writers are currently waiting for the lock
    // includes the writer that is currently holding the lock, if any
    uint32_t writers_waiting;
    // signals to writers that they can try to acquire the lock
    pthread_cond_t wr_cond;
    // signals to readers that they can try to acquire the lock
    pthread_cond_t rd_cond;
} N_WAY_STATE;

// State for the reader priority rwlock
typedef struct {
    // boolean that indicates if a writer is currently holding the lock
    bool writer_holding;
    // indicates how many writers are currently waiting for the lock
    // does not include a writer that is currently holding the lock
    uint32_t writers_waiting;
    // signals to writers that they can try to acquire the lock
    pthread_cond_t wr_cond;
} RD_PR_STATE;

// State for the writer priority rwlock
typedef struct {
    // indicates how many writers are currently waiting for the lock
    // includes the writer that is currently holding the lock, if any
    uint32_t writers_waiting;
    // indicates how many readers are currently waiting for the lock
    uint32_t readers_waiting;
    // signals to readers that they can try to acquire the lock
    pthread_cond_t rd_cond;
} WR_PR_STATE;

struct rwlock {
    // internal state of the rwlock, depending on the priority
    union {
        N_WAY_STATE nway;
        RD_PR_STATE rd;
        WR_PR_STATE wr;
    } state;

    PRIORITY priority;

    // used for READERS, WRITERS, and N_WAY priority
    // indicates how many readers are currently holding the lock
    uint32_t readers_holding;

    // mutex guarding internal rwlock state
    pthread_mutex_t mutex;

    // semaphore used to implement the write lock
    sem_t write_lock;
};

rwlock_t *rwlock_new(PRIORITY p, uint32_t n) {
    rwlock_t *rw;

    switch (p) {
    case N_WAY:
        if (n == 0) {
            // invalid n
            return NULL;
        }

        rw = malloc(sizeof(rwlock_t));

#define STATE (rw->state.nway)

        STATE.n = n;
        STATE.readers_waiting = 0;
        STATE.readers_passed = 0;
        STATE.writers_waiting = 0;

        pthread_cond_init(&STATE.wr_cond, NULL);
        pthread_cond_init(&STATE.rd_cond, NULL);

#undef STATE

        break;

    case WRITERS:

#define STATE (rw->state.wr)

        rw = malloc(sizeof(rwlock_t));

        STATE.writers_waiting = 0;
        STATE.readers_waiting = 0;
        pthread_cond_init(&STATE.rd_cond, NULL);

#undef STATE

        break;

    case READERS:

#define STATE (rw->state.rd)

        rw = malloc(sizeof(rwlock_t));

        STATE.writer_holding = false;
        STATE.writers_waiting = 0;
        pthread_cond_init(&STATE.wr_cond, NULL);

#undef STATE

        break;

    // somehow passed an invalid priority
    default: return NULL;
    }

    rw->priority = p;
    rw->readers_holding = 0;

    pthread_mutex_init(&rw->mutex, NULL);
    sem_init(&rw->write_lock, 0, 1);

    return rw;
}

void rwlock_delete(rwlock_t **rw) {
    if (rw == NULL || *rw == NULL) {
        return;
    }

    switch ((*rw)->priority) {
    case N_WAY:

#define STATE ((*rw)->state.nway)

        pthread_cond_destroy(&STATE.rd_cond);
        pthread_cond_destroy(&STATE.wr_cond);
        break;

#undef STATE

    case WRITERS:

#define STATE ((*rw)->state.wr)

        pthread_cond_destroy(&STATE.rd_cond);
        break;

#undef STATE

    case READERS:

#define STATE ((*rw)->state.rd)

        pthread_cond_destroy(&STATE.wr_cond);
        break;

#undef STATE
    }

    pthread_mutex_destroy(&(*rw)->mutex);
    sem_destroy(&(*rw)->write_lock);

    free(*rw);
    *rw = NULL;
}

static void rd_priority_rd_lock(rwlock_t *rw);
static void rd_priority_rd_unlock(rwlock_t *rw);
static void rd_priority_wr_lock(rwlock_t *rw);
static void rd_priority_wr_unlock(rwlock_t *rw);

static void wr_priority_rd_lock(rwlock_t *rw);
static void wr_priority_rd_unlock(rwlock_t *rw);
static void wr_priority_wr_lock(rwlock_t *rw);
static void wr_priority_wr_unlock(rwlock_t *rw);

static void nway_priority_rd_lock(rwlock_t *rw);
static void nway_priority_rd_unlock(rwlock_t *rw);
static void nway_priority_wr_lock(rwlock_t *rw);
static void nway_priority_wr_unlock(rwlock_t *rw);

void reader_lock(rwlock_t *rw) {
    if (rw == NULL) {
        return;
    }

    switch (rw->priority) {
    case N_WAY: nway_priority_rd_lock(rw); break;
    case READERS: rd_priority_rd_lock(rw); break;
    case WRITERS: wr_priority_rd_lock(rw); break;
    }
}

void reader_unlock(rwlock_t *rw) {
    if (rw == NULL) {
        return;
    }

    switch (rw->priority) {
    case N_WAY: nway_priority_rd_unlock(rw); break;
    case READERS: rd_priority_rd_unlock(rw); break;
    case WRITERS: wr_priority_rd_unlock(rw); break;
    }
}

void writer_lock(rwlock_t *rw) {
    if (rw == NULL) {
        return;
    }

    switch (rw->priority) {
    case N_WAY: nway_priority_wr_lock(rw); break;
    case READERS: rd_priority_wr_lock(rw); break;
    case WRITERS: wr_priority_wr_lock(rw); break;
    }
}

void writer_unlock(rwlock_t *rw) {
    if (rw == NULL) {
        return;
    }

    switch (rw->priority) {
    case N_WAY: nway_priority_wr_unlock(rw); break;
    case READERS: rd_priority_wr_unlock(rw); break;
    case WRITERS: wr_priority_wr_unlock(rw); break;
    }
}

// Implementation of the different priority methods

// READER PRIORITY
// Any number of readers can hold the lock simultaneously
// Only one writer can hold the lock at a time
// A writer holding the lock blocks all other readers and writers
// The lock is unfair in favor of readers- readers will always get the lock unless a writer is holding it

// State of the READER priority rwlock
#define STATE (rw->state.rd)

static void rd_priority_rd_lock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);

    if (!rw->readers_holding) {
        // first reader gets the write lock
        sem_wait(&rw->write_lock);
    }

    rw->readers_holding++;

    pthread_mutex_unlock(&rw->mutex);
}

static void rd_priority_rd_unlock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);
    rw->readers_holding--;

    if (!rw->readers_holding) {
        // last reader releases the write lock
        sem_post(&rw->write_lock);
        if (STATE.writers_waiting) {
            // wake up a waiting writer
            pthread_cond_signal(&STATE.wr_cond);
        }
    }

    pthread_mutex_unlock(&rw->mutex);
}

static void rd_priority_wr_lock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);

    STATE.writers_waiting++;
    while (rw->readers_holding || STATE.writer_holding) {
        // wait until there are no readers before trying to acquire the write lock
        pthread_cond_wait(&STATE.wr_cond, &rw->mutex);
    }

    STATE.writers_waiting--;
    STATE.writer_holding = true;
    pthread_mutex_unlock(&rw->mutex);
    sem_wait(&rw->write_lock);
}

static void rd_priority_wr_unlock(rwlock_t *rw) {
    // sem post before mutex lock because a reader might be holding it
    // that reader should be let through asap
    // also this prevents deadlock
    sem_post(&rw->write_lock);

    pthread_mutex_lock(&rw->mutex);

    STATE.writer_holding = false;
    if (!rw->readers_holding && STATE.writers_waiting) {
        // wake up any waiting writers
        pthread_cond_signal(&STATE.wr_cond);
    }

    pthread_mutex_unlock(&rw->mutex);
}

#undef STATE

// WRITER PRIORITY
// Any number of readers can hold the lock simultaneously
// Only one writer can hold the lock at a time
// A writer holding the lock blocks all other readers and writers
// The lock is unfair in favor of writers- writers will always get the lock before a reader

// State of the WRITER priority rwlock
#define STATE (rw->state.wr)

static void wr_priority_rd_lock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);

    STATE.readers_waiting++;
    while (STATE.writers_waiting) {
        // wait until there are no writers before trying to acquire the write lock
        pthread_cond_wait(&STATE.rd_cond, &rw->mutex);
    }

    if (!rw->readers_holding) {
        // first reader gets the write lock
        sem_wait(&rw->write_lock);
    }

    STATE.readers_waiting--;
    rw->readers_holding++;

    pthread_mutex_unlock(&rw->mutex);
}

static void wr_priority_rd_unlock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);

    rw->readers_holding--;
    if (!rw->readers_holding) {
        // last reader releases the write lock
        sem_post(&rw->write_lock);
    } else if (!STATE.writers_waiting && STATE.readers_waiting) {
        // no writers waiting
        // readers are waiting, wake them up
        pthread_cond_broadcast(&STATE.rd_cond);
    }

    pthread_mutex_unlock(&rw->mutex);
}

static void wr_priority_wr_lock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);

    STATE.writers_waiting++;
    pthread_mutex_unlock(&rw->mutex);
    sem_wait(&rw->write_lock);
}

static void wr_priority_wr_unlock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);

    STATE.writers_waiting--;
    if (!STATE.writers_waiting && STATE.readers_waiting) {
        // no writers waiting
        // wake up any waiting readers
        pthread_cond_broadcast(&STATE.rd_cond);
    }

    sem_post(&rw->write_lock);
    pthread_mutex_unlock(&rw->mutex);
}

#undef STATE

// N-WAY PRIORITY
// Any number of readers can hold the lock simultaneously
// Only one writer can hold the lock at a time
// A writer holding the lock blocks all other readers and writers
// if there are no writers waiting, readers will get the lock
// if there are no readers waiting, writers will get the lock
//
// While a writer waits, N readers can get the lock before the writer is guaranteed to get the lock

// State of the N_WAY priority rwlock
#define STATE (rw->state.nway)

static void nway_priority_rd_lock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);

    STATE.readers_waiting++;
    // wait until there are either:
    // less than N readers passed, or
    // no writers waiting

    // if (STATE.readers_passed < STATE.n || !STATE.writers_waiting) {
    //     break;
    // }
    // demorgan's law of this condition
    while (STATE.readers_passed >= STATE.n && STATE.writers_waiting) {
        pthread_cond_wait(&STATE.rd_cond, &rw->mutex);
    }

    if (STATE.readers_passed < STATE.n) {
        // avoid overflow
        STATE.readers_passed++;
    }

    STATE.readers_waiting--;

    if (!rw->readers_holding) {
        // if this is the first reader, acquire the write lock
        sem_wait(&rw->write_lock);
    }

    rw->readers_holding++;

    pthread_mutex_unlock(&rw->mutex);
}

static void nway_priority_rd_unlock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);

    rw->readers_holding--;
    // if this is the last reader, release the write lock
    if (!rw->readers_holding) {
        sem_post(&rw->write_lock);

        if (STATE.writers_waiting) {
            // there is a writer waiting

            if (STATE.readers_passed >= STATE.n || !STATE.readers_waiting) {
                // if N readers have passed, or
                // there are no readers waiting
                // wake up the writer
                pthread_cond_signal(&STATE.wr_cond);
            } else {
                // for large numbers of readers > N, lock contention can get bad if we simply wake all threads
                // so if the number of readers waiting is greater than N, only wake up N readers
                const uint32_t should_wake = STATE.n - STATE.readers_passed;
                if (should_wake > STATE.readers_waiting) {
                    // wake up all readers if there are less waiting than needed
                    pthread_cond_broadcast(&STATE.rd_cond);
                } else {
                    // only wake up the needed amount of readers
                    for (uint32_t i = 0; i < should_wake; i++) {
                        pthread_cond_signal(&STATE.rd_cond);
                    }
                }
            }
        } else {
            // there are no writers waiting
            // wake up waiting readers
            pthread_cond_broadcast(&STATE.rd_cond);
        }
    }

    pthread_mutex_unlock(&rw->mutex);
}

static void nway_priority_wr_lock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);

    STATE.writers_waiting++;
    // wait until there are either:
    // N readers have passed, or
    // no readers waiting/holding
    while (rw->readers_holding || (STATE.readers_passed < STATE.n && STATE.readers_waiting)) {
        pthread_cond_wait(&STATE.wr_cond, &rw->mutex);
    }

    pthread_mutex_unlock(&rw->mutex);
    // acquire the write lock
    sem_wait(&rw->write_lock);
}

static void nway_priority_wr_unlock(rwlock_t *rw) {
    // release the write lock
    sem_post(&rw->write_lock);

    pthread_mutex_lock(&rw->mutex);

    STATE.writers_waiting--;
    // reset the number of readers that have passed back to 0
    STATE.readers_passed = 0;

    // wake up any waiting readers, if any
    if (STATE.readers_waiting) {
        // for large numbers of readers > N, lock contention can get bad if we simply wake all threads
        // so if the number of readers waiting is greater than N, only wake up N readers
        const uint32_t should_wake = STATE.n;
        if (STATE.readers_waiting > should_wake) {
            // wake up N readers
            for (uint32_t i = 0; i < should_wake; i++) {
                pthread_cond_signal(&STATE.rd_cond);
            }
        } else {
            // wake up all waiting readers
            pthread_cond_broadcast(&STATE.rd_cond);
        }
    } else {
        // otherwise, wake up a waiting writer, if any
        pthread_cond_signal(&STATE.wr_cond);
    }

    pthread_mutex_unlock(&rw->mutex);
}

#undef STATE
