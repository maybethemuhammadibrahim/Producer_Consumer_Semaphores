# Work Division — Producer-Consumer Bounded Buffer Project

> 3 Team Members · 6 Files · 2 Synchronization Variants

---

## Overview Responsibility Matrix

| File | Member 1 | Member 2 | Member 3 |
|------|----------|----------|----------|
| `buffer.h` | ✅ **Owner** | | |
| `Makefile` | ✅ **Owner** | | |
| `README.md` | ✅ **Owner** | | |
| `semaphore_variant.c` | | ✅ **Owner** | |
| `monitor_variant.c` | | | ✅ **Owner** |
| `OS_Project_Proposal.pdf` | Collaborative | Collaborative | Collaborative |

---

## Member 1 — Shared Infrastructure & Documentation

**Files:** `buffer.h`, `Makefile`, `README.md`

### What they built:
1. **`buffer.h`** — The shared header used by both variants:
   - `bounded_buffer_t` struct (circular buffer with `data[]`, `in`, `out`, `count`)
   - Default configuration constants (`DEFAULT_BUFFER_SIZE = 5`, etc.)
   - Buffer operations: `buffer_init()`, `buffer_destroy()`, `buffer_insert()`, `buffer_remove()`
   - `get_timestamp()` utility for microsecond-precision logging
   - All functions are `static inline` (no separate `.c` file needed)

2. **`Makefile`** — Build system:
   - Compiles both variants with `-Wall -Wextra -Wpedantic -std=c11 -pthread`
   - `make`, `make clean`, `make test` targets
   - Automated correctness test that runs both variants in quiet mode and verifies output

3. **`README.md`** — Full project documentation:
   - Build/run instructions, CLI options
   - Synchronization design explanation for both variants
   - Architecture diagram (ASCII art)
   - Extension points documentation

### Talking points for viva:
- Explain the circular buffer data structure and the `in`/`out` pointer arithmetic
- Why `static inline` was chosen (header-only, no linker issues)
- Why `SLOT_EMPTY = -1` sentinel is used (for the ASCII visualizer)
- The `count` invariant: `0 <= count <= size` and `in = (out + count) % size`

---

## Member 2 — Semaphore Variant

**File:** `semaphore_variant.c` (~521 lines)

### What they built:
1. **Synchronization with 3 POSIX semaphores:**
   - `sem_empty` (init = N) — counts available empty slots
   - `sem_full` (init = 0) — counts filled slots
   - `sem_mutex` (init = 1) — binary semaphore for mutual exclusion

2. **Producer thread (`producer_thread`):**
   - Generates unique items (`(id+1)*100 + seq`)
   - Protocol: `wait(empty) → wait(mutex) → INSERT → post(mutex) → post(full)`
   - Random delay via `nanosleep` (no busy-waiting)

3. **Consumer thread (`consumer_thread`):**
   - Protocol: `wait(full) → wait(mutex) → REMOVE → post(mutex) → post(empty)`
   - Graceful termination via `g_done` flag + cascade `sem_post`

4. **Real-time ASCII visualization (`render_buffer`):**
   - Clears screen and redraws buffer state inside the critical section
   - Shows slot contents, IN/OUT pointers, fill bar, and statistics
   - Uses ANSI escape codes for colors

5. **Trace logging (`trace_log`):**
   - Thread-safe file logging with its own `pthread_mutex_t`
   - Writes to `trace_semaphore.log`

6. **`main()` function:**
   - CLI argument parsing with `getopt()`
   - Semaphore initialization, thread creation/joining
   - Graceful shutdown and correctness check (`produced == consumed`)

### Talking points for viva:
- **Deadlock prevention:** counting semaphores acquired *before* binary mutex (consistent lock ordering eliminates circular wait)
- **No busy-waiting:** `sem_wait()` is a kernel-level blocking call
- **Graceful termination:** why cascade `sem_post(&sem_full)` is needed to wake blocked consumers
- Extension points: token-rotation for starvation prevention, FIFO queuing

---

## Member 3 — Monitor Variant

**File:** `monitor_variant.c` (~similar structure, different sync)

### What they built:
1. **Synchronization with Monitor pattern (mutex + 2 condition variables):**
   - `monitor_mtx` — single mutex for all buffer access
   - `cond_not_full` — producers wait here when buffer is full
   - `cond_not_empty` — consumers wait here when buffer is empty

2. **Producer thread:**
   - Protocol: `lock → while(FULL) wait(not_full) → INSERT → signal(not_empty) → unlock`
   - `while`-loop guard protects against **spurious wakeups**

3. **Consumer thread:**
   - Protocol: `lock → while(EMPTY && !done) wait(not_empty) → REMOVE → signal(not_full) → unlock`
   - Graceful termination via `g_done` flag + `pthread_cond_broadcast`

4. **Real-time ASCII visualization** (same visual design, different title)

5. **Trace logging** to `trace_monitor.log`

6. **`main()` function:**
   - Same CLI interface for fair comparison
   - Uses `pthread_cond_broadcast()` for shutdown instead of cascade `sem_post`

### Talking points for viva:
- **Why `while` instead of `if`:** POSIX allows spurious wakeups — `pthread_cond_wait()` can return even without a `signal`
- **Single mutex vs 3 semaphores:** simpler to reason about (one lock = no ordering issues), but potentially less concurrent (producers and consumers can't overlap on the lock)
- **`broadcast` vs `signal`:** `pthread_cond_broadcast` ensures all blocked consumers wake up during shutdown
- **Comparison with semaphore variant:** monitor is simpler to write correctly, semaphore allows more fine-grained concurrency

---

## Shared / Collaborative Work

All 3 members collaborated on:
- **Project Proposal** (`OS_Project_Proposal.pdf`)
- **Design discussions** (choosing semaphore vs monitor, buffer structure, CLI interface)
- **Testing** (`make test`, manual verification)
- **Integration** (ensuring both variants use the same `buffer.h` and produce compatible output)

---

## Summary Table

| Aspect | Member 1 | Member 2 | Member 3 |
|--------|----------|----------|----------|
| **Focus** | Infrastructure | Semaphore Sync | Monitor Sync |
| **Lines of code** | ~280 | ~521 | ~530 |
| **Key concept** | Circular buffer, build system | `sem_wait`/`sem_post` | `pthread_cond_wait`/`signal` |
| **Sync primitives** | N/A | 3 semaphores | 1 mutex + 2 condvars |
| **Deadlock strategy** | N/A | Lock ordering | Single mutex |
| **Termination** | N/A | Cascade `sem_post` | `cond_broadcast` |
