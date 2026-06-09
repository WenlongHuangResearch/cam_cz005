#include "ring_buffer.h"

#include <pthread.h>
#include <stdlib.h>

struct RingBuffer {
    void **buf;
    size_t cap;
    size_t head;      /* 出队位置 */
    size_t tail;      /* 入队位置 */
    size_t count;
    size_t dropped;
    int closed;
    rb_free_fn free_fn;
    pthread_mutex_t mtx;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;   /* 供阻塞式 push 等待 */
};

RingBuffer *rb_create(size_t capacity, rb_free_fn free_fn)
{
    if (capacity == 0)
        capacity = 1;
    RingBuffer *rb = calloc(1, sizeof(*rb));
    if (!rb)
        return NULL;
    rb->buf = calloc(capacity, sizeof(void *));
    if (!rb->buf) {
        free(rb);
        return NULL;
    }
    rb->cap = capacity;
    rb->free_fn = free_fn;
    pthread_mutex_init(&rb->mtx, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    pthread_cond_init(&rb->not_full, NULL);
    return rb;
}

void rb_destroy(RingBuffer *rb)
{
    if (!rb)
        return;
    while (rb->count > 0) {
        void *item = rb->buf[rb->head];
        rb->head = (rb->head + 1) % rb->cap;
        rb->count--;
        if (rb->free_fn && item)
            rb->free_fn(item);
    }
    pthread_mutex_destroy(&rb->mtx);
    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);
    free(rb->buf);
    free(rb);
}

int rb_push(RingBuffer *rb, void *item)
{
    pthread_mutex_lock(&rb->mtx);
    if (rb->closed) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    if (rb->count == rb->cap) {
        /* 队满: 丢弃最旧的一帧, 保证生产者不被阻塞 */
        void *old = rb->buf[rb->head];
        rb->head = (rb->head + 1) % rb->cap;
        rb->count--;
        rb->dropped++;
        if (rb->free_fn && old)
            rb->free_fn(old);
    }
    rb->buf[rb->tail] = item;
    rb->tail = (rb->tail + 1) % rb->cap;
    rb->count++;
    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

int rb_push_block(RingBuffer *rb, void *item)
{
    pthread_mutex_lock(&rb->mtx);
    while (rb->count == rb->cap && !rb->closed)
        pthread_cond_wait(&rb->not_full, &rb->mtx);
    if (rb->closed) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    rb->buf[rb->tail] = item;
    rb->tail = (rb->tail + 1) % rb->cap;
    rb->count++;
    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

int rb_pop(RingBuffer *rb, void **out)
{
    pthread_mutex_lock(&rb->mtx);
    while (rb->count == 0 && !rb->closed)
        pthread_cond_wait(&rb->not_empty, &rb->mtx);
    if (rb->count == 0 && rb->closed) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    *out = rb->buf[rb->head];
    rb->head = (rb->head + 1) % rb->cap;
    rb->count--;
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

void rb_close(RingBuffer *rb)
{
    pthread_mutex_lock(&rb->mtx);
    rb->closed = 1;
    pthread_cond_broadcast(&rb->not_empty);
    pthread_cond_broadcast(&rb->not_full);
    pthread_mutex_unlock(&rb->mtx);
}

size_t rb_size(RingBuffer *rb)
{
    pthread_mutex_lock(&rb->mtx);
    size_t n = rb->count;
    pthread_mutex_unlock(&rb->mtx);
    return n;
}

size_t rb_dropped(RingBuffer *rb)
{
    pthread_mutex_lock(&rb->mtx);
    size_t n = rb->dropped;
    pthread_mutex_unlock(&rb->mtx);
    return n;
}
