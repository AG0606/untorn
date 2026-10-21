# Untorn: USENIX-Style Artifact Evaluation & Reproducibility Appendix

> **Paper Title**: Untorn: Kernel-Storage Co-Design for Hardware Atomic Writes and Physical Extent-Contiguity Guarantees in Linux  
> **Course**: Operating Systems Capstone  
> **Repository**: [https://github.com/AG0606/untorn](https://github.com/AG0606/untorn) (Tag: `v1.0-final`)  
> **Badges Claimed**: **Artifact Available**, **Artifact Functional**, **Results Reproduced**

---

## 1. Artifact Identification

- **Core Technologies**: Linux 6.11 / 6.12+ LTS, NVMe AWUN, `pwritev2(RWF_ATOMIC)`, `statx(STATX_WRITE_ATOMIC)`, ext4 `FS_IOC_FIEMAP`, Direct-I/O (`O_DIRECT`), SQLite 3.35+ Custom VFS Shim.
- **Artifact Components**:
  - `lib/libuntorn.a`: Static C runtime library containing hardware prober, extent validator, and atomic I/O engine.
  - `src/untorn_vfs.c`: Production-grade SQLite Virtual File System shim (`"untorn"`).
  - `bin/untorn`: Command-line inspection and extent management tool.
  - `bin/test_suite`: System invariant regression suite (66 tests).
  - `tests/fault_injection_harness.py`: Torn-write B-tree corruptor and mid-transaction `SIGKILL` crash simulation.
  - `bin/benchmark_suite`: Head-to-head empirical benchmark with microsecond latency percentiles and Linux `/proc/self/io` physical accounting.
  - `benchmarks/generate_evaluation_charts.py`: Automated publication chart generator (Matplotlib).
  - `run_defense_demo.sh` / `run_defense_demo.ps1`: Staged turnkey presentation runners.

---

## 2. Environment & Hardware Prerequisites

### 2.1 Hardware Requirements
- **Processor**: Any x86_64 or ARM64 processor (tested on AMD Ryzen 7 / 9 and Intel Core i7 / i9).
- **RAM**: Minimum 4 GB (recommended 8 GB+).
- **Storage**: Minimum 500 MB free non-volatile storage.
- **NVMe / SSD**: Native NVMe SSD exposing AWUN, or commodity virtualized disk with Untorn automated emulation fallback.

### 2.2 Software & Toolchain Requirements
- **Operating System**: Linux kernel 6.11+ (or Linux 5.15+ under WSL2 / Ubuntu 22.04+ with Untorn automated emulation).
- **Compiler**: GCC 11+ or Clang 14+ (`-Wall -Wextra -O2 -D_GNU_SOURCE`).
- **Libraries**: `libsqlite3-dev` (SQLite 3.35+ headers and library), `libpthread`.
- **Scripting & Plotting**: Python 3.8+ with `matplotlib` (`pip install matplotlib` or `sudo apt-get install python3-matplotlib`).

---

## 3. One-Command Quick Reproduction

To execute all 6 staged demonstration phases back-to-back without interactive pauses:

### Under Linux / WSL:
```bash
git clone https://github.com/AG0606/untorn.git
cd untorn
chmod +x ./run_defense_demo.sh
./run_defense_demo.sh --non-interactive
```

### Under Windows PowerShell:
```powershell
.\run_defense_demo.ps1 -NonInteractive
```

*Expected Execution Time*: Approximately 20–30 seconds.  
*Expected Outcome*: All 66 invariant tests pass, fault injection verifies zero torn pages, benchmark outputs latency and WAF tables, and 4 PNG figures are generated in `docs/figures/`.

---

## 4. Step-by-Step Modular Reproduction Recipes

### Experiment 1: Build System Invariants
```bash
make clean && make all
```
*Verification*: Compiles `lib/libuntorn.a`, `bin/untorn`, `bin/test_suite`, and `bin/benchmark_suite` with zero warnings (`-Wall -Wextra`).

---

### Experiment 2: Hardware & Kernel Capability Probing
```bash
./bin/untorn probe .
```
*Verification*: Outputs discovered NVMe AWUN boundaries (`min_bytes = 4096`, `max_bytes = 16384` or `65536`, `max_segments = 1`). Under virtualization or non-AWUN drives, confirms testbed emulation activation (`emulated: true`).

---

### Experiment 3: Physical Extent Contiguity & Fallocate Pre-Allocation
```bash
touch test_repro.db
./bin/untorn check-extent test_repro.db 16
./bin/untorn prealloc test_repro.db 64
./bin/untorn check-extent test_repro.db 16
rm -f test_repro.db
```
*Verification*: Initial file reports unallocated state; `prealloc` reserves a contiguous 64 KB slab via `fallocate()`; subsequent inspection confirms single physical extent with 4KB sector alignment.

---

### Experiment 4: Invariant Regression Suite (66 Tests)
```bash
./bin/test_suite
```
*Verification*: Executes all 7 test suites:
1. Hardware & Kernel Capability Discovery (5 tests)
2. Extent Contiguity & Pre-allocation Manager (5 tests)
3. Direct-I/O Hardware Atomic Write Engine (7 tests)
4. SQLite Integration under 'untorn' VFS (9 tests)
5. VFS Concurrency Locking & Direct-I/O Bounce Buffer (10 tests)
6. Synthetic Extent Fragmentation & Contiguity Restoration (15 tests)
7. Multi-Threaded Concurrency Invariant & Advisory Locking (15 tests)  
*Status: `TOTAL TESTS RUN: 66, TESTS PASSED: 66`*.

---

### Experiment 5: Storage Fault Injection & Crash Simulation
```bash
python3 tests/fault_injection_harness.py
```
*Verification*:
- **Phase 1**: Confirms that non-atomic torn writes corrupt B-Tree headers, failing with `database disk image is malformed`.
- **Phase 2**: Spawns active Untorn atomic writer, injects sudden `SIGKILL -9` mid-transaction, and reopens database to confirm `PRAGMA integrity_check = 'ok'` with zero torn pages and 0 ms recovery time.

---

### Experiment 6: Head-to-Head Empirical Benchmark (Table 1 in Paper)
```bash
cd /tmp && /path/to/untorn/bin/benchmark_suite 500
```
*Verification*:
- Outputs real-time comparative metrics table for Rollback, WAL, and Untorn.
- Confirms:
  - Untorn throughput is **$4\times\text{--}6\times$ faster** than Rollback.
  - Physical flash storage bytes written are **$50.2\%$ lower** than Rollback.
  - Untorn P99 tail latency is **lower than WAL mode** due to zero checkpointing stalls.
  - Emits `benchmark_results.csv` and `benchmark_raw_latencies.csv`.

---

### Experiment 7: Automated Publication Figure Generation
```bash
python3 benchmarks/generate_evaluation_charts.py
```
*Verification*: Generates 4 publication-quality 300 DPI figures in `docs/figures/`:
1. `waf_comparison.png`
2. `throughput_comparison.png`
3. `tail_latency_distribution.png`
4. `latency_cdf_curve.png`

---

## 5. Result Verification & Tolerance Bounds

| Paper Claim / Table 1 | Metric | Grounded Range / Acceptance Threshold |
| :--- | :--- | :--- |
| **Throughput vs Rollback** | Speedup | $\ge 4.0\times$ speedup (typically $4.5\times\text{--}5.8\times$) |
| **Flash Write Reduction** | Bytes Saved | $\ge 45.0\%$ fewer bytes written vs Rollback (typically $50.2\%$) |
| **Tail Latency Stability** | P99 Latency | Untorn P99 latency $\le$ WAL P99 latency (zero checkpoint jitter) |
| **Crash Consistency** | Integrity Check | `PRAGMA integrity_check` returns `ok` after mid-transaction `SIGKILL` |
| **Extent Fallback** | Contiguity | `untorn_ensure_contiguous()` resolves hole-punch discontiguity |

---

## 6. Directory Layout & Artifact Tree

```
untorn/
├── include/
│   ├── untorn.h                          # Public API declarations
│   └── untorn_internal.h                 # Internal macros, statx structs, timers
├── src/
│   ├── probe.c                           # Capability probing (statx & sysfs)
│   ├── extent_validator.c                # FIEMAP contiguity validation & fallocate
│   ├── atomic_io.c                       # pwritev2(RWF_ATOMIC) engine & aligned buffer pool
│   ├── untorn_vfs.c                      # SQLite custom VFS shim ("untorn")
│   └── main_cli.c                        # CLI inspection utility
├── tests/
│   ├── test_suite.c                      # 66-test invariant regression suite
│   └── fault_injection_harness.py        # Torn write & SIGKILL crash simulation
├── benchmarks/
│   ├── benchmark_suite.c                 # Microsecond-precision benchmark & CSV exporter
│   └── generate_evaluation_charts.py     # Automated Matplotlib publication chart generator
├── docs/
│   ├── figures/                          # 4 publication figures (PNG)
│   ├── untorn_final_paper.md             # 10-page formal academic capstone report
│   └── final_defense_presentation.md     # 14-slide defense deck & Professor Dossier
├── run_defense_demo.sh                   # Staged live demonstration runner (Linux/WSL)
├── run_defense_demo.ps1                  # Native Windows PowerShell launcher
├── ARTIFACT_EVALUATION.md                # This reproducibility guide
├── Makefile                              # Production build system
└── README.md                             # Project overview and executive summary
```
