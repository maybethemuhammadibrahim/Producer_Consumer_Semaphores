/*============================================================================
 * monitor_variant.c — Producer-Consumer Bounded Buffer (POSIX Monitor-Style)
 *
 * Synchronization primitives:
 *   pthread_mutex_t  monitor_mtx   — mutual exclusion
 *   pthread_cond_t   cond_not_full — producers wait when buffer is full
 *   pthread_cond_t   cond_not_empty— consumers wait when buffer is empty
 *
 * This is the "monitor" equivalent of the semaphore variant:
 *   • A single mutex replaces the binary semaphore.
 *   • Two condition variables replace the counting semaphores.
 *   • Condition checks use a while-loop (spurious wakeup safe).
 *
 * Key guarantees:
 *   ✓ No busy-waiting   — all waits are pthread_cond_wait()
 *   ✓ No race conditions — monitor_mtx guards all buffer + state access
 *   ✓ No deadlocks       — single mutex, no nested locking
 *
 * Build:   make monitor_variant
 * Usage:   ./monitor_variant [-b N] [-p P] [-c C] [-n I] [-q]
 *
 * Trace output is written to  trace_monitor.log
 *============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "buffer.h"

/* ── Portable microsecond sleep (usleep removed in POSIX.1-2008) ─── */
#define usleep_portable(us) do {                          \
    struct timespec _ts = {                               \
        .tv_sec  = (us) / 1000000,                        \
        .tv_nsec = ((us) % 1000000) * 1000                \
    };                                                    \
    nanosleep(&_ts, NULL);                                \
} while (0)

/* ═══════════════════════════════════════════════════════════════════
 *  ANSI Escape Codes
 * ═══════════════════════════════════════════════════════════════════ */
#define RST   "\033[0m"
#define BOLD  "\033[1m"
#define DIM   "\033[2m"
#define RED   "\033[91m"
#define GRN   "\033[92m"
#define YEL   "\033[93m"
#define BLU   "\033[94m"
#define MAG   "\033[95m"
#define CYN   "\033[96m"
#define WHT   "\033[97m"

/* ═══════════════════════════════════════════════════════════════════
 *  Global State
 * ═══════════════════════════════════════════════════════════════════ */

/* --- Bounded buffer ------------------------------------------------ */
static bounded_buffer_t g_buffer;

/* --- Monitor primitives -------------------------------------------- */
static pthread_mutex_t monitor_mtx    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond_not_full  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  cond_not_empty = PTHREAD_COND_INITIALIZER;

/* --- Trace-file logging -------------------------------------------- */
static FILE            *g_trace_fp   = NULL;
static pthread_mutex_t  g_trace_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- Run-time statistics (protected by monitor_mtx) ---------------- */
static int g_total_produced = 0;
static int g_total_consumed = 0;

/* --- Graceful termination (protected by monitor_mtx) --------------- */
static int g_done             = 0;
static int g_producers_active = 0;   /* decremented as producers exit */

/* --- Configuration ------------------------------------------------- */
static int g_buf_size   = DEFAULT_BUFFER_SIZE;
static int g_num_prod   = DEFAULT_NUM_PRODUCERS;
static int g_num_cons   = DEFAULT_NUM_CONSUMERS;
static int g_items_each = DEFAULT_ITEMS_PER_PROD;
static int g_quiet      = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  Trace Logging  (thread-safe via g_trace_lock)
 * ═══════════════════════════════════════════════════════════════════ */
static void trace_log(const char *role, int id,
                      const char *action, buffer_item item, int count)
{
    char ts[32];
    get_timestamp(ts, sizeof(ts));

    pthread_mutex_lock(&g_trace_lock);
    if (g_trace_fp) {
        fprintf(g_trace_fp,
                "[%s]  %-10s #%-2d  %-10s  item=%-6d  buf=[%d/%d]\n",
                ts, role, id, action, item, count, g_buffer.size);
        fflush(g_trace_fp);
    }
    pthread_mutex_unlock(&g_trace_lock);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Real-Time ASCII Terminal Visualization
 *
 *  Called INSIDE the mutex, so buffer state is consistent.
 * ═══════════════════════════════════════════════════════════════════ */
static void render_buffer(const char *role, int id,
                          const char *action, buffer_item item)
{
    if (g_quiet) return;

    char ts[32];
    get_timestamp(ts, sizeof(ts));

    printf("\033[H\033[2J");

    printf("\n");
    printf(BOLD CYN
           "  ╔══════════════════════════════════════════════════════════╗\n"
           "  ║    PRODUCER-CONSUMER BOUNDED BUFFER · MONITOR VAR.     ║\n"
           "  ╚══════════════════════════════════════════════════════════╝\n"
           RST);
    printf("\n");

    printf("  " DIM "Timestamp :" RST "  %s\n", ts);

    const char *clr = (role[0] == 'P') ? GRN : MAG;
    printf("  " DIM "Action    :" RST "  %s%s%s #%d" RST "  %-10s  item=" BOLD "%d" RST "\n",
           BOLD, clr, role, id, action, item);
    printf("\n");

    printf(BOLD "  Buffer [%d/%d]:\n" RST, g_buffer.count, g_buffer.size);
    printf("\n");

    /* Top border */
    printf("    ");
    for (int i = 0; i < g_buffer.size; i++)
        printf(DIM "%s──────" RST, (i == 0) ? "┌" : "┬");
    printf(DIM "┐" RST "\n");

    /* Slot values */
    printf("    ");
    for (int i = 0; i < g_buffer.size; i++) {
        int occupied = (g_buffer.data[i] != SLOT_EMPTY);
        if (occupied)
            printf(DIM "│" RST GRN BOLD " %4d " RST, g_buffer.data[i]);
        else
            printf(DIM "│" RST DIM "      " RST);
    }
    printf(DIM "│" RST "\n");

    /* Bottom border */
    printf("    ");
    for (int i = 0; i < g_buffer.size; i++)
        printf(DIM "%s──────" RST, (i == 0) ? "└" : "┴");
    printf(DIM "┘" RST "\n");

    /* IN/OUT pointers */
    printf("    ");
    for (int i = 0; i < g_buffer.size; i++) {
        if (i == g_buffer.in && i == g_buffer.out && g_buffer.count == 0)
            printf(YEL " IN" CYN "/O " RST);
        else if (i == g_buffer.in && i == g_buffer.out && g_buffer.count == g_buffer.size)
            printf(YEL " IN" CYN "/O " RST);
        else if (i == g_buffer.in)
            printf(YEL "  IN  " RST);
        else if (i == g_buffer.out)
            printf(CYN " OUT  " RST);
        else
            printf("      ");
        printf(" ");
    }
    printf("\n\n");

    /* Fill bar */
    int bar_w  = 30;
    int filled = (g_buffer.size > 0)
                     ? (g_buffer.count * bar_w) / g_buffer.size
                     : 0;
    int pct    = (g_buffer.size > 0)
                     ? (g_buffer.count * 100) / g_buffer.size
                     : 0;

    printf("    " DIM "Fill:" RST " ");
    for (int i = 0; i < bar_w; i++) {
        if (i < filled) printf(GRN "█" RST);
        else            printf(DIM "░" RST);
    }
    printf("  %d%%\n\n", pct);

    printf("    " GRN "Produced:" RST " %-6d    " MAG "Consumed:" RST " %-6d    "
           BLU "In buffer:" RST " %d\n",
           g_total_produced, g_total_consumed, g_buffer.count);
    printf("\n");

    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Producer Thread  (Monitor Style)
 *
 *  Protocol:
 *    lock(monitor_mtx)
 *      while (buffer FULL)  ← while-loop: guards against spurious wakeups
 *          cond_wait(cond_not_full, monitor_mtx)
 *      insert item
 *      cond_signal(cond_not_empty)
 *    unlock(monitor_mtx)
 *
 *  ┌─────────────────────────────────────────────────────────────┐
 *  │  EXTENSION POINT: Token-Rotation Starvation Prevention     │
 *  │                                                             │
 *  │  Unlike semaphores, the monitor style can implement token   │
 *  │  rotation INSIDE the critical section for simplicity:       │
 *  │                                                             │
 *  │  1. Add fields:                                             │
 *  │       static int g_prod_turn = 0;                           │
 *  │       static pthread_cond_t cond_prod_turn;                 │
 *  │                                                             │
 *  │  2. After locking monitor_mtx, BEFORE the while-loop:      │
 *  │       while (g_prod_turn != my_id)                          │
 *  │           pthread_cond_wait(&cond_prod_turn, &monitor_mtx); │
 *  │                                                             │
 *  │  3. After inserting and signaling, rotate the turn:         │
 *  │       g_prod_turn = (g_prod_turn + 1) % g_num_prod;        │
 *  │       pthread_cond_broadcast(&cond_prod_turn);              │
 *  │                                                             │
 *  │  This enforces strict round-robin ordering.                 │
 *  └─────────────────────────────────────────────────────────────┘
 *
 *  ┌─────────────────────────────────────────────────────────────┐
 *  │  EXTENSION POINT: FIFO Queuing (Monitor Variant)           │
 *  │                                                             │
 *  │  pthread_cond_wait does NOT guarantee FIFO wakeup order.   │
 *  │  To enforce strict FIFO scheduling among producers:        │
 *  │                                                             │
 *  │  1. Maintain a per-thread condition variable array:         │
 *  │       pthread_cond_t prod_queue[MAX_PRODUCERS];             │
 *  │       int prod_queue_head, prod_queue_tail;                 │
 *  │                                                             │
 *  │  2. On entry, enqueue own cond and wait on it:             │
 *  │       pthread_cond_wait(&prod_queue[my_slot], &monitor_mtx);│
 *  │                                                             │
 *  │  3. On exit, signal the head of the queue:                 │
 *  │       pthread_cond_signal(&prod_queue[head]);               │
 *  │       head = (head + 1) % MAX_PRODUCERS;                   │
 *  │                                                             │
 *  │  This guarantees FIFO ordering regardless of scheduler.    │
 *  └─────────────────────────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════ */
static void *producer_thread(void *arg)
{
    thread_arg_t *targ = (thread_arg_t *)arg;
    int id         = targ->id;
    int num_items  = targ->num_items;
    unsigned int seed = (unsigned int)(id * 31 + time(NULL));

    for (int seq = 0; seq < num_items; seq++) {

        buffer_item item = (id + 1) * 100 + seq;

        /* Simulate variable production time */
        usleep_portable(rand_r(&seed) % PRODUCER_DELAY_US);

        /* ── Monitor entry ─────────────────────────────────────── */
        pthread_mutex_lock(&monitor_mtx);

        /*  EXTENSION POINT: insert token-check HERE (see above)   */

        /* Wait while buffer is FULL — while-loop prevents
         * acting on spurious wakeups (POSIX requirement).         */
        while (g_buffer.count == g_buffer.size) {
            pthread_cond_wait(&cond_not_full, &monitor_mtx);
        }

        /* ── Critical Section ──────────────────────────────────── */
        buffer_insert(&g_buffer, item);
        g_total_produced++;

        trace_log("Producer", id, "PRODUCED", item, g_buffer.count);
        render_buffer("Producer", id, "PRODUCED", item);

        /*  EXTENSION POINT: insert token-rotate HERE (see above)  */

        /* Signal ONE waiting consumer that data is available */
        pthread_cond_signal(&cond_not_empty);

        /* ── Monitor exit ──────────────────────────────────────── */
        pthread_mutex_unlock(&monitor_mtx);
    }

    /* ── Producer done: notify if we're the last one ───────────── */
    pthread_mutex_lock(&monitor_mtx);
    g_producers_active--;
    if (g_producers_active == 0) {
        g_done = 1;
        /* Broadcast to ALL consumers so they can check g_done
         * and exit instead of waiting indefinitely.               */
        pthread_cond_broadcast(&cond_not_empty);
    }
    trace_log("Producer", id, "FINISHED", -1, g_buffer.count);
    pthread_mutex_unlock(&monitor_mtx);

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Consumer Thread  (Monitor Style)
 *
 *  Protocol:
 *    lock(monitor_mtx)
 *      while (buffer EMPTY && !done)
 *          cond_wait(cond_not_empty, monitor_mtx)
 *      if (buffer EMPTY && done)  → exit
 *      remove item
 *      cond_signal(cond_not_full)
 *    unlock(monitor_mtx)
 *
 *  ┌─────────────────────────────────────────────────────────────┐
 *  │  EXTENSION POINT: Token-Rotation (Consumers)               │
 *  │                                                             │
 *  │  Mirror the producer token approach with:                   │
 *  │    static int g_cons_turn = 0;                              │
 *  │    static pthread_cond_t cond_cons_turn;                    │
 *  │  Add a while (g_cons_turn != my_id) wait loop after lock,  │
 *  │  and rotate + broadcast after consuming.                    │
 *  └─────────────────────────────────────────────────────────────┘
 *
 *  ┌─────────────────────────────────────────────────────────────┐
 *  │  EXTENSION POINT: FIFO Queuing (Consumers)                 │
 *  │                                                             │
 *  │  Same per-thread condvar queue as producers; maintain a     │
 *  │  separate queue for consumer threads.                       │
 *  └─────────────────────────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════ */
static void *consumer_thread(void *arg)
{
    thread_arg_t *targ = (thread_arg_t *)arg;
    int id = targ->id;
    unsigned int seed = (unsigned int)(id * 37 + time(NULL));

    while (1) {
        /* ── Monitor entry ─────────────────────────────────────── */
        pthread_mutex_lock(&monitor_mtx);

        /*  EXTENSION POINT: insert token-check HERE (see above)   */

        /* Wait while buffer is EMPTY — but also check g_done
         * so we don't wait forever after all producers exit.      */
        while (g_buffer.count == 0 && !g_done) {
            pthread_cond_wait(&cond_not_empty, &monitor_mtx);
        }

        /* ── Termination check ─────────────────────────────────── *
         *  If the buffer is empty AND all producers are done,
         *  there will never be new items.  Exit gracefully.       */
        if (g_buffer.count == 0 && g_done) {
            trace_log("Consumer", id, "FINISHED", -1, g_buffer.count);
            pthread_mutex_unlock(&monitor_mtx);
            break;
        }

        /* ── Critical Section ──────────────────────────────────── */
        buffer_item item = buffer_remove(&g_buffer);
        g_total_consumed++;

        trace_log("Consumer", id, "CONSUMED", item, g_buffer.count);
        render_buffer("Consumer", id, "CONSUMED", item);

        /*  EXTENSION POINT: insert token-rotate HERE (see above)  */

        /* Signal ONE waiting producer that a slot is available */
        pthread_cond_signal(&cond_not_full);

        /* ── Monitor exit ──────────────────────────────────────── */
        pthread_mutex_unlock(&monitor_mtx);

        /* Simulate variable consumption time */
        usleep_portable(rand_r(&seed) % CONSUMER_DELAY_US);
    }

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Usage
 * ═══════════════════════════════════════════════════════════════════ */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -b N   Buffer size          (default %d)\n"
        "  -p P   Number of producers  (default %d)\n"
        "  -c C   Number of consumers  (default %d)\n"
        "  -n I   Items per producer   (default %d)\n"
        "  -q     Quiet mode (no terminal visualization)\n"
        "  -h     Show this help\n",
        prog,
        DEFAULT_BUFFER_SIZE,
        DEFAULT_NUM_PRODUCERS,
        DEFAULT_NUM_CONSUMERS,
        DEFAULT_ITEMS_PER_PROD);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    /* ── Parse command-line arguments ─────────────────────────── */
    int opt;
    while ((opt = getopt(argc, argv, "b:p:c:n:qh")) != -1) {
        switch (opt) {
            case 'b': g_buf_size   = atoi(optarg); break;
            case 'p': g_num_prod   = atoi(optarg); break;
            case 'c': g_num_cons   = atoi(optarg); break;
            case 'n': g_items_each = atoi(optarg); break;
            case 'q': g_quiet      = 1;            break;
            case 'h': /* fall through */
            default:  print_usage(argv[0]); return EXIT_FAILURE;
        }
    }

    if (g_buf_size < 1 || g_num_prod < 1 || g_num_cons < 1 || g_items_each < 1) {
        fprintf(stderr, "Error: all numeric parameters must be >= 1.\n");
        return EXIT_FAILURE;
    }

    /* ── Print configuration ──────────────────────────────────── */
    printf(BOLD CYN "\n  ── Monitor Variant ───────────────────────────────\n" RST);
    printf("  Buffer size       : %d\n", g_buf_size);
    printf("  Producers         : %d\n", g_num_prod);
    printf("  Consumers         : %d\n", g_num_cons);
    printf("  Items per producer: %d\n", g_items_each);
    printf("  Total items       : %d\n", g_num_prod * g_items_each);
    printf(BOLD CYN "  ──────────────────────────────────────────────────\n" RST);
    printf("  Starting in 2 seconds...\n\n");
    sleep(2);

    /* ── Initialize buffer ────────────────────────────────────── */
    buffer_init(&g_buffer, g_buf_size);
    g_producers_active = g_num_prod;

    /* ── Open trace log ───────────────────────────────────────── */
    g_trace_fp = fopen("trace_monitor.log", "w");
    if (!g_trace_fp) {
        perror("fopen(trace_monitor.log)");
        return EXIT_FAILURE;
    }
    fprintf(g_trace_fp, "=== Monitor Variant Trace ===\n");
    fprintf(g_trace_fp, "Buffer=%d  Producers=%d  Consumers=%d  Items/P=%d\n\n",
            g_buf_size, g_num_prod, g_num_cons, g_items_each);
    fflush(g_trace_fp);

    /* ── Allocate thread handles & arguments ──────────────────── */
    pthread_t    *prod_tids = malloc(g_num_prod * sizeof(pthread_t));
    pthread_t    *cons_tids = malloc(g_num_cons * sizeof(pthread_t));
    thread_arg_t *prod_args = malloc(g_num_prod * sizeof(thread_arg_t));
    thread_arg_t *cons_args = malloc(g_num_cons * sizeof(thread_arg_t));

    if (!prod_tids || !cons_tids || !prod_args || !cons_args) {
        perror("malloc"); return EXIT_FAILURE;
    }

    /* ── Launch consumer threads ──────────────────────────────── */
    for (int i = 0; i < g_num_cons; i++) {
        cons_args[i].id        = i;
        cons_args[i].num_items = 0;
        if (pthread_create(&cons_tids[i], NULL, consumer_thread, &cons_args[i]) != 0) {
            perror("pthread_create(consumer)"); return EXIT_FAILURE;
        }
    }

    /* ── Launch producer threads ──────────────────────────────── */
    for (int i = 0; i < g_num_prod; i++) {
        prod_args[i].id        = i;
        prod_args[i].num_items = g_items_each;
        if (pthread_create(&prod_tids[i], NULL, producer_thread, &prod_args[i]) != 0) {
            perror("pthread_create(producer)"); return EXIT_FAILURE;
        }
    }

    /* ── Wait for all producers to finish ─────────────────────── */
    for (int i = 0; i < g_num_prod; i++)
        pthread_join(prod_tids[i], NULL);

    /* ── Wait for all consumers to finish ─────────────────────── *
     *  Consumers will self-terminate once they see g_done==1 and
     *  the buffer is empty.  The broadcast in the last producer's
     *  exit path ensures no consumer sleeps forever.              */
    for (int i = 0; i < g_num_cons; i++)
        pthread_join(cons_tids[i], NULL);

    /* ── Final summary ────────────────────────────────────────── */
    if (!g_quiet) {
        printf("\033[H\033[2J");
    }
    printf(BOLD CYN "\n  ╔══════════════════════════════════════════════════════════╗\n");
    printf(         "  ║                SIMULATION COMPLETE                      ║\n");
    printf(         "  ╚══════════════════════════════════════════════════════════╝\n" RST);
    printf("\n");
    printf("  " GRN "Total Produced :" RST " %d\n", g_total_produced);
    printf("  " MAG "Total Consumed :" RST " %d\n", g_total_consumed);
    printf("  " BLU "Items in buffer:" RST " %d\n", g_buffer.count);
    printf("  " DIM "Trace log      :" RST " trace_monitor.log\n");

    if (g_total_produced == g_total_consumed)
        printf("\n  " GRN BOLD "✓ Correctness check PASSED: produced == consumed" RST "\n\n");
    else
        printf("\n  " RED BOLD "✗ Correctness check FAILED: produced ≠ consumed" RST "\n\n");

    /* ── Cleanup ──────────────────────────────────────────────── */
    fclose(g_trace_fp);
    pthread_mutex_destroy(&monitor_mtx);
    pthread_cond_destroy(&cond_not_full);
    pthread_cond_destroy(&cond_not_empty);
    buffer_destroy(&g_buffer);
    free(prod_tids);
    free(cons_tids);
    free(prod_args);
    free(cons_args);

    return EXIT_SUCCESS;
}
