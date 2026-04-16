/*============================================================================
 * semaphore_variant.c — Producer-Consumer Bounded Buffer (POSIX Semaphores)
 *
 * Synchronization primitives (3 semaphores):
 *   sem_t  sem_empty  — initialized to N  (counts EMPTY  slots)
 *   sem_t  sem_full   — initialized to 0  (counts FILLED slots)
 *   sem_t  sem_mutex  — initialized to 1  (binary semaphore / mutex)
 *
 * Lock ordering  (prevents deadlocks — outer acquired first):
 *   Producer:  wait(empty) → wait(mutex) → [CS] → post(mutex) → post(full)
 *   Consumer:  wait(full)  → wait(mutex) → [CS] → post(mutex) → post(empty)
 *
 * Key guarantees:
 *   ✓ No busy-waiting   — all waits are kernel-level sem_wait()
 *   ✓ No race conditions — sem_mutex serializes every buffer access
 *   ✓ No deadlocks       — counting sems acquired before binary mutex
 *
 * Build:   make semaphore_variant
 * Usage:   ./semaphore_variant [-b N] [-p P] [-c C] [-n I] [-q]
 *
 * Trace output is written to  trace_semaphore.log
 *============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
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
 *  ANSI Escape Codes  (terminal colors & cursor control)
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

/* --- Three POSIX semaphores ---------------------------------------- */
static sem_t sem_empty;       /* counts empty slots    (init = N)     */
static sem_t sem_full;        /* counts occupied slots (init = 0)     */
static sem_t sem_mutex;       /* binary semaphore      (init = 1)     */

/* --- Trace-file logging -------------------------------------------- */
static FILE            *g_trace_fp   = NULL;
static pthread_mutex_t  g_trace_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- Run-time statistics (read/written inside sem_mutex) ----------- */
static int g_total_produced = 0;
static int g_total_consumed = 0;

/* --- Graceful termination ------------------------------------------ */
static volatile int g_done = 0;

/* --- Configuration (set once in main, read-only thereafter) -------- */
static int g_buf_size   = DEFAULT_BUFFER_SIZE;
static int g_num_prod   = DEFAULT_NUM_PRODUCERS;
static int g_num_cons   = DEFAULT_NUM_CONSUMERS;
static int g_items_each = DEFAULT_ITEMS_PER_PROD;
static int g_quiet      = 0;   /* 1 = suppress terminal visualization */

/* ═══════════════════════════════════════════════════════════════════
 *  Trace Logging  (thread-safe; uses its own POSIX mutex)
 *
 *  Each log line:  [HH:MM:SS.µµµµµµ]  Role  ID  Action  Item  Buffer
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
 *  Called INSIDE the critical section (sem_mutex held) so the buffer
 *  state cannot change while we read it.  Only one thread at a time
 *  can execute this, eliminating interleaved output.
 *
 *  Layout:
 *    ┌──────┬──────┬──────┬──────┬──────┐
 *    │  102 │  203 │      │      │  100 │
 *    └──────┴──────┴──────┴──────┴──────┘
 *       OUT                         IN
 *    Fill: █████████████░░░░░░░░░  60%
 * ═══════════════════════════════════════════════════════════════════ */
static void render_buffer(const char *role, int id,
                          const char *action, buffer_item item)
{
    if (g_quiet) return;

    char ts[32];
    get_timestamp(ts, sizeof(ts));

    /* ── Move cursor to top-left & clear screen ── */
    printf("\033[H\033[2J");

    /* ── Title box ── */
    printf("\n");
    printf(BOLD CYN
           "  ╔══════════════════════════════════════════════════════════╗\n"
           "  ║    PRODUCER-CONSUMER BOUNDED BUFFER · SEMAPHORE VAR.   ║\n"
           "  ╚══════════════════════════════════════════════════════════╝\n"
           RST);
    printf("\n");

    /* ── Timestamp ── */
    printf("  " DIM "Timestamp :" RST "  %s\n", ts);

    /* ── Last action (green for producers, magenta for consumers) ── */
    const char *clr = (role[0] == 'P') ? GRN : MAG;
    printf("  " DIM "Action    :" RST "  %s%s%s #%d" RST "  %-10s  item=" BOLD "%d" RST "\n",
           BOLD, clr, role, id, action, item);
    printf("\n");

    /* ── Buffer occupancy label ── */
    printf(BOLD "  Buffer [%d/%d]:\n" RST, g_buffer.count, g_buffer.size);
    printf("\n");

    /* ── Top border ── */
    printf("    ");
    for (int i = 0; i < g_buffer.size; i++)
        printf(DIM "%s──────" RST, (i == 0) ? "┌" : "┬");
    printf(DIM "┐" RST "\n");

    /* ── Slot values ── */
    printf("    ");
    for (int i = 0; i < g_buffer.size; i++) {
        int occupied = (g_buffer.data[i] != SLOT_EMPTY);
        if (occupied)
            printf(DIM "│" RST GRN BOLD " %4d " RST, g_buffer.data[i]);
        else
            printf(DIM "│" RST DIM "      " RST);
    }
    printf(DIM "│" RST "\n");

    /* ── Bottom border ── */
    printf("    ");
    for (int i = 0; i < g_buffer.size; i++)
        printf(DIM "%s──────" RST, (i == 0) ? "└" : "┴");
    printf(DIM "┘" RST "\n");

    /* ── IN / OUT pointer indicators ── */
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
        /* account for the border character width */
        printf(" ");
    }
    printf("\n\n");

    /* ── Fill bar ── */
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

    /* ── Statistics ── */
    printf("    " GRN "Produced:" RST " %-6d    " MAG "Consumed:" RST " %-6d    "
           BLU "In buffer:" RST " %d\n",
           g_total_produced, g_total_consumed, g_buffer.count);
    printf("\n");

    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Producer Thread
 *
 *  Each producer generates `num_items` items (value = id*100 + seq)
 *  and inserts them into the bounded buffer using the classic
 *  semaphore protocol.
 *
 *  ┌─────────────────────────────────────────────────────────────┐
 *  │  EXTENSION POINT: Token-Rotation Starvation Prevention     │
 *  │                                                             │
 *  │  To prevent a single producer from monopolizing the buffer, │
 *  │  inject a token-check here, BEFORE sem_wait(&sem_empty):    │
 *  │                                                             │
 *  │    1. Declare a global:                                     │
 *  │         static int  g_prod_token = 0;                       │
 *  │         static sem_t sem_prod_turn[MAX_PRODUCERS];          │
 *  │    2. Before sem_wait(&sem_empty), add:                     │
 *  │         sem_wait(&sem_prod_turn[arg->id]);                  │
 *  │    3. After sem_post(&sem_full), rotate:                    │
 *  │         int next = (arg->id + 1) % g_num_prod;             │
 *  │         sem_post(&sem_prod_turn[next]);                     │
 *  │    4. In main(), init sem_prod_turn[0] = 1, rest = 0.      │
 *  │                                                             │
 *  │  This guarantees round-robin scheduling among producers.    │
 *  └─────────────────────────────────────────────────────────────┘
 *
 *  ┌─────────────────────────────────────────────────────────────┐
 *  │  EXTENSION POINT: FIFO Queuing (Semaphore Variant)         │
 *  │                                                             │
 *  │  POSIX sem_wait() does NOT guarantee FIFO wakeup ordering. │
 *  │  To enforce strict FIFO among competing producers:          │
 *  │                                                             │
 *  │    1. Maintain a FIFO queue of per-thread semaphores:       │
 *  │         typedef struct { sem_t gate; } fifo_node_t;        │
 *  │    2. On entry, enqueue a node and sem_wait(&node.gate).   │
 *  │    3. On exit,  dequeue the head and sem_post(&head.gate). │
 *  │                                                             │
 *  │  See: "The Little Book of Semaphores" §4.3 (FIFO Queue).  │
 *  └─────────────────────────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════ */
static void *producer_thread(void *arg)
{
    thread_arg_t *targ = (thread_arg_t *)arg;
    int id         = targ->id;
    int num_items  = targ->num_items;
    unsigned int seed = (unsigned int)(id * 31 + time(NULL));

    for (int seq = 0; seq < num_items; seq++) {

        buffer_item item = (id + 1) * 100 + seq;   /* unique per-producer */

        /* Simulate variable production time — NO busy-wait */
        usleep_portable(rand_r(&seed) % PRODUCER_DELAY_US);

        /* ── Semaphore protocol ────────────────────────────────── */
        /*  EXTENSION POINT: insert token-wait HERE (see above)    */

        sem_wait(&sem_empty);       /* ① wait for an empty slot   */
        sem_wait(&sem_mutex);       /* ② enter critical section   */

        /* ── Critical Section ──────────────────────────────────── */
        buffer_insert(&g_buffer, item);
        g_total_produced++;

        trace_log("Producer", id, "PRODUCED", item, g_buffer.count);
        render_buffer("Producer", id, "PRODUCED", item);

        /* ── End Critical Section ──────────────────────────────── */
        sem_post(&sem_mutex);       /* ③ leave critical section   */
        sem_post(&sem_full);        /* ④ signal a filled slot     */

        /*  EXTENSION POINT: insert token-rotate HERE (see above)  */
    }

    trace_log("Producer", id, "FINISHED", -1, g_buffer.count);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Consumer Thread
 *
 *  Consumers loop until all items have been produced AND the buffer
 *  is empty, then exit gracefully.
 *
 *  ┌─────────────────────────────────────────────────────────────┐
 *  │  EXTENSION POINT: Token-Rotation (Consumers)               │
 *  │                                                             │
 *  │  Mirror the producer token approach:                        │
 *  │    static sem_t sem_cons_turn[MAX_CONSUMERS];               │
 *  │  Insert sem_wait(&sem_cons_turn[id]) before sem_wait(full). │
 *  │  Insert sem_post(&sem_cons_turn[next]) after sem_post(empty)│
 *  └─────────────────────────────────────────────────────────────┘
 *
 *  ┌─────────────────────────────────────────────────────────────┐
 *  │  EXTENSION POINT: FIFO Queuing (Consumers)                 │
 *  │                                                             │
 *  │  Same FIFO-queue technique as producers; maintain a         │
 *  │  separate queue for consumer threads.                       │
 *  └─────────────────────────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════ */
static void *consumer_thread(void *arg)
{
    thread_arg_t *targ = (thread_arg_t *)arg;
    int id = targ->id;
    unsigned int seed = (unsigned int)(id * 37 + time(NULL));

    while (1) {
        /*  EXTENSION POINT: insert token-wait HERE (see above)    */

        sem_wait(&sem_full);        /* ① wait for an occupied slot */
        sem_wait(&sem_mutex);       /* ② enter critical section    */

        /* ── Check graceful termination ─────────────────────────
         *  If all producers are done and the buffer is drained,
         *  release the mutex, cascade-wake the next consumer,
         *  and exit.
         * ──────────────────────────────────────────────────────── */
        if (g_buffer.count == 0 && g_done) {
            sem_post(&sem_mutex);
            sem_post(&sem_full);    /* cascade: wake next consumer */
            break;
        }

        /* ── Critical Section ──────────────────────────────────── */
        buffer_item item = buffer_remove(&g_buffer);
        g_total_consumed++;

        trace_log("Consumer", id, "CONSUMED", item, g_buffer.count);
        render_buffer("Consumer", id, "CONSUMED", item);

        /* ── End Critical Section ──────────────────────────────── */
        sem_post(&sem_mutex);       /* ③ leave critical section    */
        sem_post(&sem_empty);       /* ④ signal an empty slot      */

        /*  EXTENSION POINT: insert token-rotate HERE (see above)  */

        /* Simulate variable consumption time — NO busy-wait */
        usleep_portable(rand_r(&seed) % CONSUMER_DELAY_US);
    }

    trace_log("Consumer", id, "FINISHED", -1, g_buffer.count);
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
    printf(BOLD CYN "\n  ── Semaphore Variant ──────────────────────────────\n" RST);
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

    /* ── Initialize semaphores ────────────────────────────────── */
    if (sem_init(&sem_empty, 0, g_buf_size) != 0) {
        perror("sem_init(empty)"); return EXIT_FAILURE;
    }
    if (sem_init(&sem_full, 0, 0) != 0) {
        perror("sem_init(full)");  return EXIT_FAILURE;
    }
    if (sem_init(&sem_mutex, 0, 1) != 0) {
        perror("sem_init(mutex)"); return EXIT_FAILURE;
    }

    /* ── Open trace log ───────────────────────────────────────── */
    g_trace_fp = fopen("trace_semaphore.log", "w");
    if (!g_trace_fp) {
        perror("fopen(trace_semaphore.log)");
        return EXIT_FAILURE;
    }
    fprintf(g_trace_fp, "=== Semaphore Variant Trace ===\n");
    fprintf(g_trace_fp, "Buffer=%d  Producers=%d  Consumers=%d  Items/P=%d\n\n",
            g_buf_size, g_num_prod, g_num_cons, g_items_each);
    fflush(g_trace_fp);

    /* ── Allocate thread handles & arguments ──────────────────── */
    pthread_t   *prod_tids = malloc(g_num_prod * sizeof(pthread_t));
    pthread_t   *cons_tids = malloc(g_num_cons * sizeof(pthread_t));
    thread_arg_t *prod_args = malloc(g_num_prod * sizeof(thread_arg_t));
    thread_arg_t *cons_args = malloc(g_num_cons * sizeof(thread_arg_t));

    if (!prod_tids || !cons_tids || !prod_args || !cons_args) {
        perror("malloc"); return EXIT_FAILURE;
    }

    /* ── Launch consumer threads ──────────────────────────────── */
    for (int i = 0; i < g_num_cons; i++) {
        cons_args[i].id        = i;
        cons_args[i].num_items = 0; /* unused by consumers */
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

    /* ── Signal termination to consumers ──────────────────────── *
     *  Set the done flag, then post sem_full once per consumer
     *  so blocked consumers wake up and see the flag.             */
    g_done = 1;
    for (int i = 0; i < g_num_cons; i++)
        sem_post(&sem_full);

    /* ── Wait for all consumers to finish ─────────────────────── */
    for (int i = 0; i < g_num_cons; i++)
        pthread_join(cons_tids[i], NULL);

    /* ── Final summary ────────────────────────────────────────── */
    if (!g_quiet) {
        printf("\033[H\033[2J");  /* clear screen one last time */
    }
    printf(BOLD CYN "\n  ╔══════════════════════════════════════════════════════════╗\n");
    printf(         "  ║                SIMULATION COMPLETE                      ║\n");
    printf(         "  ╚══════════════════════════════════════════════════════════╝\n" RST);
    printf("\n");
    printf("  " GRN "Total Produced :" RST " %d\n", g_total_produced);
    printf("  " MAG "Total Consumed :" RST " %d\n", g_total_consumed);
    printf("  " BLU "Items in buffer:" RST " %d\n", g_buffer.count);
    printf("  " DIM "Trace log      :" RST " trace_semaphore.log\n");

    if (g_total_produced == g_total_consumed)
        printf("\n  " GRN BOLD "✓ Correctness check PASSED: produced == consumed" RST "\n\n");
    else
        printf("\n  " RED BOLD "✗ Correctness check FAILED: produced ≠ consumed" RST "\n\n");

    /* ── Cleanup ──────────────────────────────────────────────── */
    fclose(g_trace_fp);
    sem_destroy(&sem_empty);
    sem_destroy(&sem_full);
    sem_destroy(&sem_mutex);
    buffer_destroy(&g_buffer);
    free(prod_tids);
    free(cons_tids);
    free(prod_args);
    free(cons_args);

    return EXIT_SUCCESS;
}
