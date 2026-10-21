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
build: all

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
	@echo "=== Running Untorn Invariant Tests (66 Tests) ==="
	@$(TARGET_TEST)
	@echo "=== Running Fault Injection Crash Harness ==="
	@python3 $(TESTDIR)/fault_injection_harness.py

TX_COUNT ?= 1000
bench: $(TARGET_BENCH)
	@echo "=== Running Untorn Benchmark Suite ($(TX_COUNT) Transactions) ==="
	@$(TARGET_BENCH) $(TX_COUNT)
benchmarks: bench

charts:
	@echo "=== Generating Evaluation Visualizations ==="
	@mkdir -p docs/figures
	@python3 $(BENCHDIR)/generate_evaluation_charts.py 2>/dev/null || python $(BENCHDIR)/generate_evaluation_charts.py

demo: all
	@echo "=== Launching Turnkey Defense Demo ==="
	@chmod +x ./run_defense_demo.sh
	@bash ./run_defense_demo.sh --non-interactive

clean:
	rm -rf $(SRCDIR)/*.o $(LIBDIR) $(BINDIR) *.db *.db-* *.bin demo_*.db* test_*.db* bench_*.db* unsafe_*.db* untorn_*.db*

distclean: clean
	rm -f benchmark_results.csv benchmark_raw_latencies.csv

.PHONY: all build test bench benchmarks charts demo clean distclean
