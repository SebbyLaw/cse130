#include "queue.h"

#include <stdbool.h>
#include <stdint.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/types.h>

#include <stdlib.h>

struct queue {
    // capacity of the queue
    int size;
    // circular buffer
    void **buf;

    int head;
    int tail;

    // lock for writers
    pthread_mutex_t wr_lock;
    // lock for readers
    pthread_mutex_t rd_lock;

    // semaphore for readers
    sem_t rd_sem;
    // semaphore for writers
    sem_t wr_sem;
};

queue_t *queue_new(int size) {
    if (size <= 0) {
        // bad queue size, return NULL
        return NULL;
    }

    queue_t *q = malloc(sizeof(queue_t));

    q->size = size;
    q->buf = malloc(size * sizeof(void *));
    q->head = 0;
    q->tail = 0;

    sem_init(&q->rd_sem, false, 0);
    sem_init(&q->wr_sem, false, size);
    pthread_mutex_init(&q->rd_lock, NULL);
    pthread_mutex_init(&q->wr_lock, NULL);

    return q;
}

void queue_delete(queue_t **q) {
    if (q == NULL || *q == NULL) {
        return;
    }

    sem_destroy(&(*q)->rd_sem);
    sem_destroy(&(*q)->wr_sem);
    pthread_mutex_destroy(&(*q)->rd_lock);
    pthread_mutex_destroy(&(*q)->wr_lock);
    free((*q)->buf);
    free(*q);

    *q = NULL;
}

bool queue_push(queue_t *q, void *elem) {
    if (q == NULL) {
        return false;
    }

    sem_wait(&q->wr_sem);

    pthread_mutex_lock(&q->wr_lock);

    q->buf[q->head++] = elem;
    q->head %= q->size;

    pthread_mutex_unlock(&q->wr_lock);

    sem_post(&q->rd_sem);

    return true;
}

bool queue_pop(queue_t *q, void **elem) {
    if (q == NULL) {
        return false;
    }

    sem_wait(&q->rd_sem);

    pthread_mutex_lock(&q->rd_lock);

    *elem = q->buf[q->tail++];
    q->tail %= q->size;

    pthread_mutex_unlock(&q->rd_lock);

    sem_post(&q->wr_sem);

    return true;
}
