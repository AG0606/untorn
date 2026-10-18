CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -g -Iinclude -D_GNU_SOURCE
LDFLAGS ?= -lsqlite3 -lpthread

SRCDIR = src
INCDIR = include
BINDIR = bin
LIBDIR = lib
TESTDIR = tests
BENCHDIR = benchmarks

OBJS = $(SRCDIR)/probe.o $(SRCDIR)/extent_validator.o $(SRCDIR)/atomic_io.o $(SRCDIR)/untorn_vfs.o

TARGET_LIB = $(LIBDIR)/libuntorn.a
TARGET_CLI = $(BINDIR)/untorn
TARGET_TEST = $(BINDIR)/test_suite
TARGET_BENCH = $(BINDIR)/benchmark_suite

all: $(TARGET_LIB) $(TARGET_CLI) $(TARGET_TEST) $(TARGET_BENCH)

$(LIBDIR):
	mkdir -p $(LIBDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(INCDIR)/untorn.h $(INCDIR)/untorn_internal.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET_LIB): $(OBJS) | $(LIBDIR)
	ar rcs $@ $(OBJS)

$(TARGET_CLI): $(SRCDIR)/main_cli.c $(TARGET_LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -L$(LIBDIR) -luntorn $(LDFLAGS) -o $@

$(TARGET_TEST): $(TESTDIR)/test_suite.c $(TARGET_LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -L$(LIBDIR) -luntorn $(LDFLAGS) -o $@

$(TARGET_BENCH): $(BENCHDIR)/benchmark_suite.c $(TARGET_LIB) | $(BINDIR)
	$(CC) $(CFLAGS) $< -L$(LIBDIR) -luntorn $(LDFLAGS) -o $@

test: $(TARGET_TEST) $(TARGET_CLI)
	@echo "=== Running Untorn Invariant Tests ==="
	@$(TARGET_TEST)
	@echo "=== Running Fault Injection Crash Harness ==="
	@python3 $(TESTDIR)/fault_injection_harness.py

bench: $(TARGET_BENCH)
	@echo "=== Running Untorn Benchmark Suite ==="
	@$(TARGET_BENCH) 2000

clean:
	rm -rf $(SRCDIR)/*.o $(LIBDIR) $(BINDIR) *.db *.db-* *.bin

.PHONY: all test bench clean
