#!/usr/bin/env bash
# ==============================================================================
# UNTORN: TURNKEY LIVE DEFENSE DEMONSTRATION & REPRODUCIBILITY RUNNER
# Course: Operating Systems Capstone — Final Defense & Committee Review
# ==============================================================================

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ANSI Terminal Color Palette
C_RESET="\033[0m"
C_BOLD="\033[1m"
C_CYAN="\033[1;36m"
C_GREEN="\033[1;32m"
C_YELLOW="\033[1;33m"
C_RED="\033[1;31m"
C_MAGENTA="\033[1;35m"
C_BLUE="\033[1;34m"

INTERACTIVE=1
for arg in "$@"; do
    if [[ "$arg" == "--non-interactive" || "$arg" == "-y" || "$arg" == "--ci" ]]; then
        INTERACTIVE=0
    fi
done

if [[ -n "$CI" ]]; then
    INTERACTIVE=0
fi

prompt_phase() {
    local phase_num="$1"
    local phase_title="$2"
    echo -e "\n${C_CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${C_RESET}"
    echo -e "${C_BOLD} [PHASE ${phase_num}] ${phase_title}${C_RESET}"
    echo -e "${C_CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${C_RESET}"
    if [[ "$INTERACTIVE" -eq 1 ]]; then
        echo -e "${C_YELLOW}>>> Press ENTER to execute Phase ${phase_num} (or Ctrl+C to abort)...${C_RESET}"
        read -r
    fi
}

echo -e "\n${C_CYAN}================================================================================${C_RESET}"
echo -e "${C_BOLD}     UNTORN: KERNEL-STORAGE CO-DESIGN CAPSTONE LIVE DEMO RUNNER                ${C_RESET}"
echo -e "${C_BOLD}     Bridging VFS Extents & NVMe AWUN for Zero-Logging Storage Engines          ${C_RESET}"
echo -e "${C_CYAN}================================================================================${C_RESET}"

# Step 0: Build all binaries
echo -e "\n${C_YELLOW}[Step 0] Ensuring static library and binaries are up to date...${C_RESET}"
make all
echo -e "${C_GREEN}[✓] Engine static library (lib/libuntorn.a) and test/benchmark binaries built.${C_RESET}"

# Phase 1: Hardware & Kernel Capability Discovery
prompt_phase "1" "Hardware & Kernel Atomic Capability Probing (statx & sysfs)"
./bin/untorn probe .

# Phase 2: Extent Contiguity & Physical Pre-Allocation Inspection
prompt_phase "2" "Physical Extent Contiguity & Fallocate Pre-Allocation (FIEMAP)"
echo -e "${C_YELLOW}[Demo] Creating sample database file and checking extent layout...${C_RESET}"
touch demo_extent_test.db
./bin/untorn check-extent demo_extent_test.db 16 || true
echo -e "\n${C_YELLOW}[Demo] Pre-allocating contiguous 64 KB physical extent slab...${C_RESET}"
./bin/untorn prealloc demo_extent_test.db 64
./bin/untorn check-extent demo_extent_test.db 16
rm -f demo_extent_test.db

# Phase 3: Invariant Unit & Stress Test Suites
prompt_phase "3" "System Invariants: Extent Fragmentation & Multi-Threaded Concurrency (66 Tests)"
./bin/test_suite

# Phase 4: Storage Fault Injection & Mid-Transaction Crash Simulation
prompt_phase "4" "Crash Consistency & Fault Injection (Torn Write Failure vs SIGKILL Zero-Repair)"
python3 tests/fault_injection_harness.py

# Phase 5: Head-to-Head Empirical Benchmark
prompt_phase "5" "Empirical Benchmark: Rollback vs WAL vs Untorn (P50–P99.9 & Linux /proc/self/io WAF)"
BENCH_DIR="/tmp"
echo -e "${C_YELLOW}[Demo] Executing 500 synchronous transactions per engine on native ext4 storage...${C_RESET}"
cd "$BENCH_DIR"
"$SCRIPT_DIR/bin/benchmark_suite" 500
cp "$BENCH_DIR/benchmark_results.csv" "$BENCH_DIR/benchmark_raw_latencies.csv" "$SCRIPT_DIR/" 2>/dev/null || true
cd "$SCRIPT_DIR"

# Phase 6: Automated Figure Generation
prompt_phase "6" "Automated Publication Figure Generation (docs/figures/*.png)"
python3 benchmarks/generate_evaluation_charts.py

echo -e "\n${C_GREEN}================================================================================${C_RESET}"
echo -e "${C_GREEN}${C_BOLD} ★ ALL DEMONSTRATION & DEFENSE PHASES COMPLETED WITH 100% SUCCESS!             ${C_RESET}"
echo -e "${C_GREEN}================================================================================${C_RESET}"
echo -e "${C_CYAN}Deliverables Available for Examination:${C_RESET}"
echo -e "  • Academic Capstone Paper  : ${C_BOLD}docs/untorn_final_paper.md${C_RESET}"
echo -e "  • Defense Presentation Deck: ${C_BOLD}docs/final_defense_presentation.md${C_RESET}"
echo -e "  • Reproducibility Guide    : ${C_BOLD}ARTIFACT_EVALUATION.md${C_RESET}"
echo -e "  • Generated Figures        : ${C_BOLD}docs/figures/*.png${C_RESET} (4 publication charts)"
echo -e "  • Source Repository        : ${C_BOLD}https://github.com/AG0606/untorn (v1.0-final)${C_RESET}\n"
