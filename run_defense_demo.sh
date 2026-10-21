#!/usr/bin/env bash
# ==============================================================================
# UNTORN: TURNKEY LIVE DEFENSE DEMONSTRATION & REPRODUCIBILITY RUNNER
# Course: Operating Systems Capstone — Final Defense & Committee Review
# Repository: https://github.com/AG0606/untorn (Release: v1.0-final)
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

# Parse CLI execution mode
INTERACTIVE=1
for arg in "$@"; do
    case "$arg" in
        --non-interactive|-y|--batch|-n|--ci)
            INTERACTIVE=0
            ;;
    esac
done

if [[ -n "$CI" && "$CI" != "0" ]]; then
    INTERACTIVE=0
fi

prompt_phase() {
    local phase_num="$1"
    local phase_title="$2"
    local phase_desc="$3"

    echo -e "\n${C_CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${C_RESET}"
    echo -e "${C_BOLD}${C_MAGENTA} [PHASE ${phase_num}/5] ${C_CYAN}${phase_title}${C_RESET}"
    echo -e "${C_BLUE} ℹ Rationale: ${phase_desc}${C_RESET}"
    echo -e "${C_CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${C_RESET}"

    if [[ "$INTERACTIVE" -eq 1 ]]; then
        echo -e "${C_YELLOW}>>> Press [ENTER] to advance to Phase ${phase_num} (or Ctrl+C to abort)...${C_RESET}"
        read -r
    fi
}

echo -e "\n${C_CYAN}================================================================================${C_RESET}"
echo -e "${C_BOLD}${C_CYAN}         UNTORN: KERNEL-STORAGE CO-DESIGN CAPSTONE LIVE DEFENSE RUNNER          ${C_RESET}"
echo -e "${C_BOLD}     Bridging VFS Extents & NVMe AWUN for Zero-Logging Storage Engines          ${C_RESET}"
echo -e "${C_YELLOW}     Mode: $( [[ $INTERACTIVE -eq 1 ]] && echo 'Interactive Defense Presentation' || echo 'Automated Non-Interactive Verification' ) ${C_RESET}"
echo -e "${C_CYAN}================================================================================${C_RESET}"

# Step 0: Pre-Flight Compilation and System Invariant Verification
echo -e "\n${C_YELLOW}[Pre-Flight Step 0] Compiling static library and running all 66 invariant tests...${C_RESET}"
make all
./bin/test_suite
echo -e "${C_GREEN}[✓] Pre-flight verification passed: Engine library built and 66/66 invariant tests clean.${C_RESET}"

# Phase 1: Hardware & Kernel Capability Discovery Probe
prompt_phase "1" "Hardware & Kernel Capability Discovery Probe (statx & sysfs)" \
    "Discovers NVMe AWUN boundaries via statx(STATX_WRITE_ATOMIC) and Linux block queue attributes."
./bin/untorn probe .

# Phase 2: Extent Contiguity & FIEMAP Inspection
prompt_phase "2" "Extent Contiguity & Allocation Invariant Inspection (FS_IOC_FIEMAP)" \
    "Inspects physical block extents before and after fallocate() to guarantee N_bio_split = 0."
DEMO_DB="demo_extent_test.db"
rm -f "$DEMO_DB"
echo -e "${C_YELLOW}[Step 2.1] Inspecting unallocated file (demonstrating why naive atomic writes fail)...${C_RESET}"
touch "$DEMO_DB"
./bin/untorn check-extent "$DEMO_DB" 16 || true

echo -e "\n${C_YELLOW}[Step 2.2] Invoking untorn_ensure_contiguous() via fallocate() to pre-allocate 1 MB...${C_RESET}"
./bin/untorn prealloc "$DEMO_DB" 1

echo -e "\n${C_YELLOW}[Step 2.3] Re-inspecting physical extent mapping via FIEMAP ioctl...${C_RESET}"
./bin/untorn check-extent "$DEMO_DB" 16
rm -f "$DEMO_DB"

# Phase 3: Storage Fault Injection & Crash Recovery
prompt_phase "3" "Storage Fault Injection & Mid-Transaction Crash Simulation" \
    "Rebello et al. crash model: torn write induces fatal corruption vs Untorn SIGKILL zero-repair."
python3 tests/fault_injection_harness.py

# Phase 4: Head-to-Head Empirical Benchmark Execution
prompt_phase "4" "Head-to-Head Empirical Benchmark (Rollback vs WAL vs Untorn)" \
    "Executes 500 synchronous transactions per engine, measuring Linux /proc/self/io WAF and P50-P99.9 latencies."
./bin/benchmark_suite 500

# Phase 5: Publication Graph Generation
prompt_phase "5" "Automated Publication Figure Generation (Matplotlib Pipeline)" \
    "Generates 4 publication-quality 300 DPI figures in docs/figures/ from benchmark CSV traces."
python3 benchmarks/generate_evaluation_charts.py

echo -e "\n${C_GREEN}================================================================================${C_RESET}"
echo -e "${C_GREEN}${C_BOLD} ★ ALL 5 DEFENSE PHASES COMPLETED WITH 100% SUCCESS & ZERO INVARIANT BREACHES!  ${C_RESET}"
echo -e "${C_GREEN}================================================================================${C_RESET}"
echo -e "${C_CYAN}Capstone Deliverables & Publication Artifacts:${C_RESET}"
echo -e "  • Academic Capstone Paper  : ${C_BOLD}docs/untorn_final_paper.md${C_RESET}"
echo -e "  • Defense Presentation Deck: ${C_BOLD}docs/final_defense_presentation.md${C_RESET}"
echo -e "  • Reproducibility Guide    : ${C_BOLD}ARTIFACT_EVALUATION.md${C_RESET}"
echo -e "  • Publication Figures      : ${C_BOLD}docs/figures/*.png${C_RESET} (4 charts)"
echo -e "  • Benchmark CSV Traces     : ${C_BOLD}benchmark_results.csv, benchmark_raw_latencies.csv${C_RESET}"
echo -e "  • GitHub Release Tag       : ${C_BOLD}v1.0-final (https://github.com/AG0606/untorn)${C_RESET}\n"
