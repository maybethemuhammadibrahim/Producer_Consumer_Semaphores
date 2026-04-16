# Producer-Consumer Bounded Buffer Simulation

A multithreaded simulation of the classic **Producer-Consumer problem** using a circular bounded buffer, implemented in C with POSIX threads.

Two synchronization variants are provided for correctness comparison:

| Variant | Primitives | File |
|---------|-----------|------|
| **Semaphore** | `sem_t` × 3 (empty, full, mutex) | `semaphore_variant.c` |
| **Monitor** | `pthread_mutex_t` + `pthread_cond_t` × 2 | `monitor_variant.c` |

---

## Project Structure

```
project/
├── buffer.h              # Shared header: buffer struct, constants, inline ops
├── semaphore_variant.c   # Semaphore-based synchronization
├── monitor_variant.c     # Monitor-based synchronization (mutex + condvars)
├── Makefile              # Build system with test target
└── README.md             # This file
```

---

## Building

**Prerequisites:** GCC and POSIX threads support (any Linux distribution or WSL).

```bash
# Build both variants
make

# Build only one
make semaphore_variant
make monitor_variant

# Clean all artifacts
make clean
```

---

## Running

```bash
# Default configuration (buffer=5, 2 producers, 2 consumers, 10 items each)
./semaphore_variant
./monitor_variant

# Custom configuration
./semaphore_variant -b 8 -p 3 -c 4 -n 15

# Quiet mode (no terminal visualization, only trace log)
./monitor_variant -b 5 -p 2 -c 2 -n 20 -q
```

### Command-Line Options

| Flag | Description | Default |
|------|-------------|---------|
| `-b N` | Buffer size (capacity) | 5 |
| `-p P` | Number of producer threads | 2 |
| `-c C` | Number of consumer threads | 2 |
| `-n I` | Items each producer generates | 10 |
| `-q` | Quiet mode (disable ASCII visualization) | off |
| `-h` | Show help | — |

---

## Synchronization Design

### Semaphore Variant

Three POSIX semaphores coordinate access:

```
sem_empty (init = N)  ── counts available empty slots
sem_full  (init = 0)  ── counts available filled slots
sem_mutex (init = 1)  ── binary semaphore (mutual exclusion)
```

**Lock ordering** (prevents deadlocks):
```
Producer:  wait(empty) → wait(mutex) → [INSERT] → post(mutex) → post(full)
Consumer:  wait(full)  → wait(mutex) → [REMOVE] → post(mutex) → post(empty)
```

The counting semaphores are always acquired **before** the binary mutex, ensuring a consistent ordering that eliminates circular wait.

### Monitor Variant

A single mutex with two condition variables:

```
monitor_mtx    ── mutual exclusion for all buffer access
cond_not_full  ── producers wait here when buffer is full
cond_not_empty ── consumers wait here when buffer is empty
```

**Protocol:**
```
Producer:  lock → while(FULL) wait(not_full) → INSERT → signal(not_empty) → unlock
Consumer:  lock → while(EMPTY && !done) wait(not_empty) → REMOVE → signal(not_full) → unlock
```

The `while`-loop guards protect against **spurious wakeups** (POSIX requirement).

---

## Key Guarantees

| Property | How Achieved |
|----------|-------------|
| **No busy-waiting** | All waits use `sem_wait()` or `pthread_cond_wait()` — kernel-level blocking |
| **No race conditions** | All buffer reads/writes occur inside the mutex/binary-semaphore critical section |
| **No deadlocks** | Consistent lock ordering (counting sems before mutex); monitor uses single mutex |
| **Graceful termination** | Semaphore: cascade `sem_post` chain; Monitor: `pthread_cond_broadcast` |

---

## Output

### Terminal Visualization

Both variants display a real-time ASCII visualization showing:
- Current buffer contents with occupied/empty slots
- IN (write) and OUT (read) pointer positions
- Fill percentage with progress bar
- Running produced/consumed statistics

### Trace Log

Each run generates a detailed trace file (`trace_semaphore.log` or `trace_monitor.log`):

```
[14:23:45.123456]  Producer   #0   PRODUCED    item=100     buf=[1/5]
[14:23:45.234567]  Producer   #1   PRODUCED    item=200     buf=[2/5]
[14:23:45.345678]  Consumer   #0   CONSUMED    item=100     buf=[1/5]
```

---

## Automated Testing

```bash
make test
```

This runs both variants with `-q` (quiet) mode using a stress configuration (3 producers, 2 consumers, buffer size 3) and verifies:
1. The "PASSED" correctness message appears (produced == consumed)
2. Trace log item counts match

---

## Extension Points

Both source files contain clearly marked `EXTENSION POINT` comment blocks at the exact insertion locations. Two extensions are documented:

### 1. Token-Rotation Starvation Prevention

**Problem:** The OS scheduler may starve certain threads by always waking the same one.

**Solution:** Round-robin token passing ensures every thread gets fair access.

**Location:** Marked in both `producer_thread()` and `consumer_thread()` — look for:
```c
/*  EXTENSION POINT: insert token-wait HERE  */
/*  EXTENSION POINT: insert token-rotate HERE  */
```

### 2. FIFO Queuing

**Problem:** Neither `sem_wait()` nor `pthread_cond_wait()` guarantee FIFO wakeup ordering.

**Solution:** Per-thread semaphore/condvar queue enforces strict FIFO scheduling.

**Location:** Marked at the same locations, with detailed implementation instructions in the comment blocks.

---

## Architecture Diagram

```
    ┌──────────┐     ┌──────────┐
    │Producer 0│     │Producer 1│  ...
    └────┬─────┘     └────┬─────┘
         │                │
    wait(empty)      wait(empty)
    wait(mutex)      wait(mutex)
         │                │
         ▼                ▼
    ┌─────────────────────────────┐
    │   Bounded Circular Buffer   │
    │  ┌───┬───┬───┬───┬───┐     │
    │  │   │   │   │   │   │     │
    │  └───┴───┴───┴───┴───┘     │
    │    OUT ──────────▶ IN       │
    └─────────────────────────────┘
         │                │
    post(mutex)      post(mutex)
    post(full)       post(full)
         │                │
         ▼                ▼
    ┌──────────┐     ┌──────────┐
    │Consumer 0│     │Consumer 1│  ...
    └──────────┘     └──────────┘
```

---

## References

- Silberschatz, Galvin, Gagne — *Operating System Concepts*, Ch. 7 (Synchronization Examples)
- Downey — *The Little Book of Semaphores*, §4.1–4.3
- `man 3 sem_init`, `man 3 pthread_cond_wait`
