#!/bin/bash
set -e

echo -e "\033[1;36m========================================================\033[0m"
echo -e "\033[1m UNTORN: END-TO-END DEMOSTRATION & BENCHMARK RUNNER\033[0m"
echo -e "\033[1;36m========================================================\033[0m\n"

cd "$(dirname "$0")"

echo -e "\033[1;33m[Step 1] Building Untorn Static Library and Executables...\033[0m"
make all

echo -e "\n\033[1;33m[Step 2] Probing NVMe / Kernel Hardware Atomic Capabilities...\033[0m"
./bin/untorn probe .

echo -e "\033[1;33m[Step 3] Running Invariant Unit & Extent Verification Tests...\033[0m"
./bin/test_suite

echo -e "\n\033[1;33m[Step 4] Running Storage Fault Injection & Crash Simulation...\033[0m"
python3 tests/fault_injection_harness.py

echo -e "\n\033[1;33m[Step 5] Running Transactional Benchmark (Rollback vs WAL vs Untorn)...\033[0m"
./bin/benchmark_suite ${1:-1000}

echo -e "\n\033[1;32m========================================================\033[0m"
echo -e "\033[1;32m ALL DEMO PHASES COMPLETED WITH 100% SUCCESS!\033[0m"
echo -e "\033[1;32m========================================================\033[0m\n"
