# ============================================================================
# Makefile — Producer-Consumer Bounded Buffer Simulation
#
# Targets:
#   all                Build both variants
#   semaphore_variant  Build the POSIX semaphore variant
#   monitor_variant    Build the POSIX monitor (mutex+condvar) variant
#   clean              Remove binaries and trace logs
#   test               Run both variants in quiet mode and verify correctness
#
# Usage:
#   make                       # build everything
#   make semaphore_variant     # build only the semaphore version
#   make monitor_variant       # build only the monitor version
#   make test                  # run automated correctness test
#   make clean                 # remove build artifacts
# ============================================================================

CC       = gcc
CFLAGS   = -Wall -Wextra -Wpedantic -std=c11 -g -pthread \
           -D_POSIX_C_SOURCE=200809L
LDFLAGS  = -pthread

# On older Linux (glibc < 2.17), uncomment the next line for clock_gettime:
# LDFLAGS += -lrt

TARGETS  = semaphore_variant monitor_variant

# ── Default target ──────────────────────────────────────────────────
.PHONY: all clean test

all: $(TARGETS)

# ── Semaphore variant ───────────────────────────────────────────────
semaphore_variant: semaphore_variant.c buffer.h
	$(CC) $(CFLAGS) -o $@ semaphore_variant.c $(LDFLAGS)

# ── Monitor variant ─────────────────────────────────────────────────
monitor_variant: monitor_variant.c buffer.h
	$(CC) $(CFLAGS) -o $@ monitor_variant.c $(LDFLAGS)

# ── Clean ───────────────────────────────────────────────────────────
clean:
	rm -f $(TARGETS) *.o trace_*.log

# ── Automated correctness test ──────────────────────────────────────
# Runs both variants in quiet mode (-q) and checks that the final
# "PASSED" message appears in the output.
test: all
	@echo "=== Testing Semaphore Variant ==="
	@./semaphore_variant -b 3 -p 3 -c 2 -n 5 -q | grep -q "PASSED" \
		&& echo "  ✓ Semaphore variant: PASSED" \
		|| echo "  ✗ Semaphore variant: FAILED"
	@echo "=== Testing Monitor Variant ==="
	@./monitor_variant -b 3 -p 3 -c 2 -n 5 -q | grep -q "PASSED" \
		&& echo "  ✓ Monitor variant:   PASSED" \
		|| echo "  ✗ Monitor variant:   FAILED"
	@echo "=== Comparing trace logs ==="
	@echo "  Semaphore produced: $$(grep -c PRODUCED trace_semaphore.log) items"
	@echo "  Semaphore consumed: $$(grep -c CONSUMED trace_semaphore.log) items"
	@echo "  Monitor   produced: $$(grep -c PRODUCED trace_monitor.log) items"
	@echo "  Monitor   consumed: $$(grep -c CONSUMED trace_monitor.log) items"
	@echo "=== All tests complete ==="
