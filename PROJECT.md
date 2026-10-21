# Project: Untorn — Linux Kernel-Storage Co-Design for Atomic DBMS Commits

## Architecture
Untorn co-designs user-space DBMS storage engines with Linux kernel block and filesystem layers to eliminate the traditional 2× write penalty (double-write buffering / WAL checkpoints) while guaranteeing zero torn writes.

```
+-----------------------------------------------------------------------+
| Application / SQLite DBMS ("untorn" VFS Shim)                        |
| - O_DIRECT Page Cache Bypass                                          |
| - 4096-byte Aligned Bounce Buffer for Unaligned Reads                 |
| - SQLITE_IOCAP_ATOMIC / SQLITE_IOCAP_ATOMIC16K                        |
| - POSIX Advisory Locking (F_RDLCK, F_WRLCK via F_SETLK)               |
+-----------------------------------------------------------------------+
                                  |
                                  v
+-----------------------------------------------------------------------+
| Untorn Runtime Core (libuntorn.a)                                     |
| 1. Capability Prober: statx(STATX_WRITE_ATOMIC) / sysfs / Emulation   |
| 2. Extent Validator: ioctl(FS_IOC_FIEMAP) + fallocate() pre-alloc     |
| 3. Atomic I/O Engine: pwritev2(RWF_ATOMIC | RWF_DSYNC)                |
+-----------------------------------------------------------------------+
                                  |
                                  v
+-----------------------------------------------------------------------+
| Linux Kernel 6.11 / 6.12+ Block Layer & Filesystem (ext4 / XFS)       |
| - Guaranteed Contiguity -> Zero bio_split() -> Failure Atomicity      |
| - NVMe Block Driver -> Hardware Atomic Write Unit Normal (AWUN)       |
+-----------------------------------------------------------------------+
```

## Feature Inventory
Every feature from the Survey phase and `ORIGINAL_REQUEST.md` is inventoried and mapped to an implementation milestone.

| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| F1 | Per-Transaction Microsecond Latency Tracking | High-precision `clock_gettime(CLOCK_MONOTONIC)` timestamping for each individual transaction in `benchmarks/benchmark_suite.c` | M1 | ORIGINAL_REQUEST §R1 |
| F2 | Sorted Latency Percentiles (P50, P90, P95, P99, P99.9) | Calculate percentiles using `qsort` and display in the benchmark summary table | M1 | ORIGINAL_REQUEST §R1 |
| F3 | Machine-Readable CSV Export | Export benchmark results and raw per-transaction latencies to `benchmark_results.csv` and `benchmark_raw_latencies.csv` | M1 | ORIGINAL_REQUEST §R1 |
| F4 | Synthetic Extent Fragmentation Invariant | Artificially fragment a test file layout using `fallocate(FALLOC_FL_PUNCH_HOLE)`, assert `untorn_validate_contiguity()` detects discontiguity, invoke `untorn_ensure_contiguous()`, and assert contiguity is restored | M2 | ORIGINAL_REQUEST §R2 |
| F5 | Multi-Threaded Concurrency Invariant | Concurrently execute multiple SQLite reader threads alongside an atomic writer thread under Untorn VFS, asserting zero data corruption and correct POSIX advisory locking behavior | M2 | ORIGINAL_REQUEST §R2 |
| F6 | Automated Data Visualization Pipeline | Standalone Python visualization script `benchmarks/generate_evaluation_charts.py` using `matplotlib` to parse benchmark results | M3 | ORIGINAL_REQUEST §R3 |
| F7 | Publication Figures: WAF & Throughput | Generate `docs/figures/waf_comparison.png` (bytes written & reduction %) and `docs/figures/throughput_comparison.png` | M3 | ORIGINAL_REQUEST §R3 |
| F8 | Publication Figures: Tail Latency & CDF | Generate `docs/figures/tail_latency_distribution.png` (P50-P99.9) and `docs/figures/latency_cdf_curve.png` (cumulative distribution) | M3 | ORIGINAL_REQUEST §R3 |
| F9 | Formal Academic Capstone Paper (10 Pages) | IEEE/ACM FAST/HotStorage format paper in `docs/untorn_final_paper.md` covering 2x write tax, NVMe AWUN, Linux 6.11/6.12 RWF_ATOMIC, VFS extent impedance mismatch, co-design, evaluation, KaTeX math, citations | M4 | ORIGINAL_REQUEST §R4 |
| F10 | Final Defense Presentation Deck (14 Slides) | Comprehensive 14-slide defense deck in `docs/final_defense_presentation.md` with slide markdown, speaker notes, and architectural flowcharts | M5 | ORIGINAL_REQUEST §R5 |
| F11 | Professor Defense Dossier | Exhaustive defense Q&A dossier in `docs/final_defense_presentation.md` addressing DBMS wrapper vs OS co-design, non-AWUN drives, io_uring, page-cache bypass | M5 | ORIGINAL_REQUEST §R5 |
| F12 | Turnkey Live Defense Demo Runners | Interactive and non-interactive presentation runners `run_defense_demo.sh` and `run_defense_demo.ps1` with 5 staged color-coded phases | M6 | ORIGINAL_REQUEST §R6 |
| F13 | USENIX-Style Artifact Evaluation Guide | `ARTIFACT_EVALUATION.md` detailing environment requirements, one-line reproduction commands, and verification criteria | M6 | ORIGINAL_REQUEST §R6 |
| F14 | Build System & Documentation Updates | Makefile targets (`make charts`, `make demo`, `make bench TX_COUNT=1000`) and comprehensive `README.md` updates | M6 | ORIGINAL_REQUEST §R6 |
| F15 | Repository Release Tagging | Clean git commit tracking of all deliverables and release tag `v1.0-final` | M6 | ORIGINAL_REQUEST §R6 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Benchmark Latency Percentiles & CSV Export | Instrument `benchmarks/benchmark_suite.c` with microsecond latency recording, compute P50/P90/P95/P99/P99.9 percentiles, print table, and output `benchmark_results.csv` and `benchmark_raw_latencies.csv` | Survey complete | PLANNED |
| M2 | Advanced System Invariant & Stress Test Suites | Extend `tests/test_suite.c` with synthetic extent fragmentation invariant (`fallocate(FALLOC_FL_PUNCH_HOLE)`, discontiguity detection, `untorn_ensure_contiguous()` restoration) and multi-threaded concurrency invariant (concurrent SQLite readers + atomic writer under Untorn VFS, POSIX locking, zero corruption) | Survey complete | PLANNED |
| M3 | Automated Data Visualization Pipeline | Author `benchmarks/generate_evaluation_charts.py` using matplotlib to generate 4 publication-quality PNG charts in `docs/figures/` (`waf_comparison.png`, `throughput_comparison.png`, `tail_latency_distribution.png`, `latency_cdf_curve.png`) | M1 | PLANNED |
| M4 | Academic Capstone Research Paper | Author 10-page formal academic capstone paper in `docs/untorn_final_paper.md` following IEEE/ACM FAST/HotStorage structure, KaTeX math equations, 14-paper bibliography, and threats to validity | M1, M2, M3 | PLANNED |
| M5 | Final Defense Presentation Deck & Professor Dossier | Author 14-slide defense deck with speaker notes, architectural diagrams, and exhaustive Professor Defense Dossier in `docs/final_defense_presentation.md` | M1, M2, M3 | PLANNED |
| M6 | Turnkey Demo Runners, Artifact Evaluation, & Release | Author `run_defense_demo.sh`, `run_defense_demo.ps1`, `ARTIFACT_EVALUATION.md`, update `Makefile` and `README.md`, verify end-to-end demo execution, git commit cleanly, and tag `v1.0-final` | M1, M2, M3, M4, M5 | PLANNED |

## Interface Contracts
### Benchmark ↔ Visualization & Evaluation (`benchmarks/benchmark_suite.c` -> `benchmark_results.csv`)
- Header format: `mode,transactions,total_time_ms,throughput_tps,avg_latency_us,p50_us,p90_us,p95_us,p99_us,p999_us,bytes_written,waf`
- Modes: `Rollback`, `WAL`, `Untorn`
- Secondary trace: `benchmark_raw_latencies.csv` (`tx_id,rollback_latency_us,wal_latency_us,untorn_latency_us`)

### Invariant Test Suite Contracts (`tests/test_suite.c`)
- Baseline tests: 36 assertions preserved and passing.
- Extent fragmentation suite: `test_synthetic_extent_fragmentation()`. Tests hole-punching via `FALLOC_FL_PUNCH_HOLE`, checks `untorn_validate_contiguity()` returns `UNTORN_ERR_DISCONTIGUOUS`, repairs via `untorn_ensure_contiguous()`, asserts contiguity restored and atomic write succeeds.
- Concurrency suite: `test_multithreaded_concurrency()`. Tests 1 writer thread + 3 reader threads under Untorn VFS, asserts POSIX advisory locking (`SQLITE_BUSY` handled), zero corruption, and `PRAGMA integrity_check = ok`.
- Output: prints summary table, asserts `STATUS: ALL INVARIANTS VERIFIED!`.

### Visualization Engine (`benchmarks/generate_evaluation_charts.py`)
- CLI: `python3 benchmarks/generate_evaluation_charts.py [--input CSV] [--output-dir DIR]`
- Default output directory: `docs/figures/`
- Generated files: `waf_comparison.png`, `throughput_comparison.png`, `tail_latency_distribution.png`, `latency_cdf_curve.png` (300 DPI, publication styling).

### Demo Runner Contract (`run_defense_demo.sh` & `run_defense_demo.ps1`)
- Staged phases:
  1. Capability Probe (`./bin/untorn_cli --probe`)
  2. Extent Contiguity Inspection (`./bin/untorn_cli --check-extent`)
  3. Fault Injection & Crash Recovery (`python3 tests/fault_injection_harness.py`)
  4. Head-to-Head Benchmark Execution (`./bin/benchmark_suite 500`)
  5. Publication Graph Generation (`python3 benchmarks/generate_evaluation_charts.py`)
- Flags: Supports interactive mode (prompts before each step) and `--non-interactive` / `-y` / `CI=1` mode.

## Code Layout
- `include/untorn.h`: Public API declarations.
- `include/untorn_internal.h`: Internal macros, statx structs, and timing helpers.
- `src/probe.c`: Capability probing (statx `STATX_WRITE_ATOMIC` and sysfs).
- `src/extent_validator.c`: `FS_IOC_FIEMAP` contiguity validation and `fallocate` pre-allocation.
- `src/atomic_io.c`: `pwritev2(..., RWF_ATOMIC | RWF_DSYNC)` atomic write engine.
- `src/untorn_vfs.c`: SQLite custom VFS shim (`"untorn"`).
- `benchmarks/benchmark_suite.c`: Transactional benchmarking, microsecond latency percentile calculation, CSV export.
- `benchmarks/generate_evaluation_charts.py`: Python matplotlib publication chart generation.
- `tests/test_suite.c`: Invariant test suite (baseline 36 + extent fragmentation + concurrency).
- `tests/fault_injection_harness.py`: Torn write corruption and mid-transaction SIGKILL crash recovery test.
- `docs/figures/`: Publication PNG figures.
- `docs/untorn_final_paper.md`: 10-page formal academic capstone paper.
- `docs/final_defense_presentation.md`: 14-slide presentation deck and Professor Defense Dossier.
- `ARTIFACT_EVALUATION.md`: USENIX-style artifact evaluation reproducibility guide.
- `run_defense_demo.sh`: Bash demo runner.
- `run_defense_demo.ps1`: PowerShell demo runner.
- `Makefile`: Build, test, benchmark, charts, and demo targets.
- `README.md`: Capstone project documentation.
