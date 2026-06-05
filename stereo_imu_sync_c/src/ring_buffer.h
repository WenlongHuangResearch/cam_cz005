/*
 * ring_buffer.h —— 线程安全的有界环形队列 (单/多生产者-单/多消费者)。
 *
 * 设计要点 (针对实时取流):
 *   - 生产者 (采集线程) 调用 rb_push 永不阻塞: 队列满时丢弃最旧的一帧,
 *     避免采集线程被慢编码器拖住而导致 USB 丢帧。被丢弃的元素用创建时
 *     传入的 free_fn 释放, 并累加 dropped 计数。
 *   - 消费者 (编码线程) 调用 rb_pop 阻塞等待, 直到有数据或队列被关闭。
 *   - rb_close 用于优雅退出: 关闭后 push 失败, pop 取完剩余元素后返回 -1。
 *
 * 元素是 void*; 不关心具体类型, 释放交给 free_fn。
 */
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stddef.h>

typedef void (*rb_free_fn)(void *item);

typedef struct RingBuffer RingBuffer;

/* 创建容量为 capacity 的环形队列; free_fn 用于释放被丢弃/残留的元素 (可为 NULL)。 */
RingBuffer *rb_create(size_t capacity, rb_free_fn free_fn);

/* 销毁队列, 残留元素用 free_fn 释放。 */
void rb_destroy(RingBuffer *rb);

/* 入队。队列满时丢弃最旧元素 (用 free_fn 释放) 再放入新元素, 永不阻塞。
 * 返回 0 成功; -1 表示队列已关闭 (此时 item 不入队, 调用方应自行释放)。 */
int rb_push(RingBuffer *rb, void *item);

/* 出队 (阻塞)。返回 0 并把元素写入 *out; 当队列已关闭且为空时返回 -1。 */
int rb_pop(RingBuffer *rb, void **out);

/* 关闭队列: 唤醒所有等待的消费者, 之后 push 失败。 */
void rb_close(RingBuffer *rb);

/* 当前队列内元素个数 (瞬时值)。 */
size_t rb_size(RingBuffer *rb);

/* 累计因队满被丢弃的元素个数。 */
size_t rb_dropped(RingBuffer *rb);

#endif /* RING_BUFFER_H */
