## Roadmap: Producer-Consumer Simulation

### 1. Prerequisites and Skill Requirements
* **Language & Environment:** C (GNU GCC), Linux command-line interface, Makefile construction.
* **Multithreading:** `pthread_create`, `pthread_join`, thread attributes (`pthread.h`).
* **Synchronization:**
    * Semaphores (`semaphore.h`): `sem_init`, `sem_wait`, `sem_post`, `sem_destroy`.
    * Mutexes (`pthread.h`): `pthread_mutex_init`, `pthread_mutex_lock`, `pthread_mutex_unlock`.
    * Condition Variables: `pthread_cond_wait`, `pthread_cond_signal`.
* **Benchmarking:** Standard C library timing routines (`clock_gettime`, `time.h`).

### 2. Implementation Milestones

#### Phase 1: Core Functionality (Weeks 1-2)
* **Repository Setup:** Initialize Git repository and standard C project structure (src, include, bin, Makefile).
* **Buffer Definition:** Implement a circular bounded buffer structure (array or linked list) of size N.
* **Semaphore Initialization:** Initialize `empty` to N, `full` to 0, and `mutex` (binary semaphore) to 1.
* **Thread Execution Logic:**
    * **Producer:** Loop -> Generate Item -> Wait on `empty` -> Wait on `mutex` -> Insert -> Post `mutex` -> Post `full`.
    * **Consumer:** Loop -> Wait on `full` -> Wait on `mutex` -> Remove -> Post `mutex` -> Post `empty`.
* **Integration:** Spawn multiple producer and consumer threads. Run single-threaded and multi-threaded stress tests to verify the absence of race conditions and deadlocks.
* **Logging:** Implement thread-safe logging writing timestamp, thread ID, and action (produce/consume) to a trace file.

#### Phase 2: Advanced Features (Weeks 3-4)
* **Monitor Variant:** Implement a parallel solution using `pthread_mutex` and `pthread_cond` for direct performance comparison.
* **Starvation Prevention:** Implement token-based rotation to guarantee CPU time across threads under high load.
* **Fair Semaphores:** Enforce FIFO thread queuing to prevent priority inversion.
* **Terminal Visualization:** Output real-time buffer state (e.g., `[X][ ][X][X]`) and thread counts to standard output.
* **Performance Benchmarking:** Implement timing logic. Measure throughput (items processed per second) and latency across the semaphore and monitor variants.

#### Phase 3: Final Delivery (Week 5)
* **Testing:** Run regression tests across edge cases (zero-item bursts, high thread contention, single producer/consumer).
* **Documentation:** Finalize inline C comments. Write the performance comparison report using collected metrics.
* **Packaging:** Ensure `make all` builds all targets correctly. Package source, Makefiles, trace logs, and reports.

---

### 3. Task Distribution Matrix

| Member | Mandatory Tasks | Additional Features |
| :--- | :--- | :--- |
| **Muhammad Hasnain** | Semaphore logic (`sem_wait`/`sem_post`), Mutex for buffer | Monitor implementation, Performance benchmarking |
| **Obaid Ullah** | Multi-thread spawning, Empty/full handling (no busy waiting) | Starvation prevention, Fair semaphore queuing |
| **Muhammad Ibrahim** | Lock ordering (deadlock-free), Formal reasoning | Thread-safe trace logging, Real-time terminal visualization |

---

### 4. LLM Generation Prompt

Copy the block below and paste it into Claude or Gemini to generate the initial codebase and project structure.

```text
Act as an expert C systems programmer and operating systems instructor. I need you to generate the complete codebase, build configuration, and testing framework for a Producer-Consumer bounded-buffer simulation in C using POSIX threads and semaphores, as defined by my project proposal.

The project requires a modular structure. Please generate the following components:

1. Makefile: Targets to build the standard semaphore variant, the monitor variant, and a clean target.
2. Header File (buffer.h): Define the circular bounded buffer structure, constants, and function prototypes.
3. Core Implementation (semaphore_variant.c):
   - Configurable number of producers, consumers, and buffer size N.
   - 3 semaphores: 'empty' (N), 'full' (0), and 'mutex' (1) implemented as a binary semaphore.
   - Producer logic: wait(empty) -> wait(mutex) -> insert -> post(mutex) -> post(full).
   - Consumer logic: wait(full) -> wait(mutex) -> remove -> post(mutex) -> post(empty).
   - Thread-safe logging to a trace file containing timestamp, Thread ID, and the action.
   - Real-time ASCII terminal visualization of the buffer state.
4. Monitor Implementation (monitor_variant.c):
   - A parallel version of the same logic using pthread_mutex_t and pthread_cond_t instead of semaphores for correctness comparison.
5. Extensions: Provide instructions or code comments indicating exactly where to inject token-rotation starvation prevention and FIFO queuing logic.

Ensure strict absence of busy-waiting, no race conditions, and consistent lock ordering to prevent deadlocks. Output the files sequentially in clearly labeled code blocks.
```

[Introduction to POSIX Semaphores in C](https://www.youtube.com/watch?v=eMAOZ1XaSCo)

This tutorial provides a conceptual breakdown of limiting concurrent access to a shared resource using semaphores in POSIX multi-threading.


http://googleusercontent.com/youtube_content/0
