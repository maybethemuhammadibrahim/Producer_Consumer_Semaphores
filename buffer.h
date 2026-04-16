/*============================================================================
 * buffer.h — Shared Definitions for Producer-Consumer Bounded Buffer
 *
 * This header defines:
 *   • The circular bounded-buffer data structure
 *   • Compile-time configuration constants
 *   • Buffer manipulation primitives (NOT thread-safe; caller must sync)
 *   • A microsecond-resolution timestamp helper
 *
 * Both the semaphore variant and the monitor variant include this header
 * to share a single, canonical buffer implementation.
 *
 * IMPORTANT: All functions here are static inline so that each translation
 *            unit gets its own copy — no separate buffer.c is needed.
 *============================================================================*/

#ifndef BUFFER_H
#define BUFFER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Configurable Defaults (override via command-line flags)
 * ═══════════════════════════════════════════════════════════════════ */
#define DEFAULT_BUFFER_SIZE      5      /* N — bounded buffer capacity   */
#define DEFAULT_NUM_PRODUCERS    2      /* P — number of producer threads*/
#define DEFAULT_NUM_CONSUMERS    2      /* C — number of consumer threads*/
#define DEFAULT_ITEMS_PER_PROD   10     /* I — items each producer emits */

/* Simulated processing delays (microseconds, upper bound for rand) */
#define PRODUCER_DELAY_US        400000 /* 0 … 0.4 s */
#define CONSUMER_DELAY_US        600000 /* 0 … 0.6 s */

/* Sentinel stored in empty buffer slots for visualization */
#define SLOT_EMPTY               (-1)

/* ═══════════════════════════════════════════════════════════════════
 *  Types
 * ═══════════════════════════════════════════════════════════════════ */

/** Type of items flowing through the buffer. */
typedef int buffer_item;

/**
 * Circular bounded buffer.
 *
 *   data[]  — dynamically allocated array of `size` slots
 *   in      — index where the next item will be INSERTED  (tail)
 *   out     — index where the next item will be REMOVED   (head)
 *   count   — current number of occupied slots
 *
 * Invariants (maintained by callers under proper synchronization):
 *   0 <= count <= size
 *   in  = (out + count) % size   (always holds)
 */
typedef struct {
    buffer_item *data;
    int          size;      /* capacity N                           */
    int          in;        /* next write position  (producer end)  */
    int          out;       /* next read  position  (consumer end)  */
    int          count;     /* current occupancy                    */
} bounded_buffer_t;

/** Thread argument passed to producer / consumer entry points. */
typedef struct {
    int id;                 /* logical thread id (0-based)          */
    int num_items;          /* items to produce (producers only)    */
} thread_arg_t;

/* ═══════════════════════════════════════════════════════════════════
 *  Buffer Operations  (NOT thread-safe — caller must hold lock)
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * Allocate and zero-initialize a bounded buffer of `capacity` slots.
 * Every slot is set to SLOT_EMPTY so the visualizer can distinguish
 * occupied from vacant positions.
 */
static inline void buffer_init(bounded_buffer_t *buf, int capacity)
{
    buf->data  = (buffer_item *)malloc(capacity * sizeof(buffer_item));
    if (!buf->data) { perror("malloc"); exit(EXIT_FAILURE); }
    buf->size  = capacity;
    buf->in    = 0;
    buf->out   = 0;
    buf->count = 0;
    for (int i = 0; i < capacity; i++)
        buf->data[i] = SLOT_EMPTY;
}

/** Free the underlying array. */
static inline void buffer_destroy(bounded_buffer_t *buf)
{
    free(buf->data);
    buf->data = NULL;
}

/**
 * Insert `item` at the tail of the circular buffer.
 * Pre-condition: count < size  (caller guarantees via semaphore/condvar).
 */
static inline void buffer_insert(bounded_buffer_t *buf, buffer_item item)
{
    buf->data[buf->in] = item;
    buf->in = (buf->in + 1) % buf->size;
    buf->count++;
}

/**
 * Remove and return the item at the head of the circular buffer.
 * The vacated slot is reset to SLOT_EMPTY for the ASCII visualizer.
 * Pre-condition: count > 0  (caller guarantees via semaphore/condvar).
 */
static inline buffer_item buffer_remove(bounded_buffer_t *buf)
{
    buffer_item item   = buf->data[buf->out];
    buf->data[buf->out] = SLOT_EMPTY;
    buf->out = (buf->out + 1) % buf->size;
    buf->count--;
    return item;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Utility: Microsecond-Resolution Timestamp
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * Write the current wall-clock time into `buf` as  HH:MM:SS.uuuuuu
 * Uses clock_gettime(CLOCK_REALTIME, …) for µs precision.
 */
static inline void get_timestamp(char *buf, size_t len)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm *tm_info = localtime(&ts.tv_sec);
    snprintf(buf, len, "%02d:%02d:%02d.%06ld",
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec,
             ts.tv_nsec / 1000);
}

#endif /* BUFFER_H */
