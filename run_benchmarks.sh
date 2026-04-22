#!/bin/bash
# ============================================================================
# run_benchmarks.sh
# Automated latency benchmark suite for the CNF Security Layer
#
# Runs multiple benchmark configurations and produces a comparison summary.
#
# Usage:
#   chmod +x run_benchmarks.sh
#   sudo ./run_benchmarks.sh          # Run with defaults (100, 500, 1000 iterations)
#   sudo ./run_benchmarks.sh 50 200   # Custom iteration counts
# ============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCHMARK="$SCRIPT_DIR/latency_benchmark"
RESULTS_DIR="$SCRIPT_DIR/benchmark_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Default iteration counts
ITERATIONS=("${@:-100 500 1000}")
if [ $# -eq 0 ]; then
    ITERATIONS=(100 500 1000)
fi

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

echo ""
echo -e "${BOLD}╔══════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║     CNF Security Layer — Automated Benchmark Suite             ║${NC}"
echo -e "${BOLD}║     $(date)                           ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Check prerequisites
if [ ! -f "$BENCHMARK" ]; then
    echo -e "${YELLOW}[INFO] Building latency_benchmark...${NC}"
    cd "$SCRIPT_DIR"

    # Try with Makefile first
    if make benchmark 2>/dev/null; then
        echo -e "${GREEN}[OK] Built via Makefile${NC}"
    else
        # Direct compile
        echo -e "${YELLOW}[INFO] Direct compilation...${NC}"
        gcc -O3 -Wall -Wextra -o latency_benchmark \
            latency_benchmark.c cnf_security_layer.c \
            -lseccomp -lm
        echo -e "${GREEN}[OK] Built latency_benchmark${NC}"
    fi
fi

if [ ! -x "$BENCHMARK" ]; then
    echo -e "${RED}[ERROR] Cannot find or execute: $BENCHMARK${NC}"
    echo "Build it with: gcc -O3 -o latency_benchmark latency_benchmark.c cnf_security_layer.c -lseccomp -lm"
    exit 1
fi

# Create results directory
mkdir -p "$RESULTS_DIR"

echo -e "${CYAN}[CONFIG]${NC} Iteration counts: ${ITERATIONS[*]}"
echo -e "${CYAN}[CONFIG]${NC} Results directory: $RESULTS_DIR"
echo ""

# Run benchmarks
SUMMARY_FILE="$RESULTS_DIR/summary_${TIMESTAMP}.txt"
echo "CNF Security Layer Benchmark Summary" > "$SUMMARY_FILE"
echo "Date: $(date)" >> "$SUMMARY_FILE"
echo "Hostname: $(hostname)" >> "$SUMMARY_FILE"
echo "Kernel: $(uname -r)" >> "$SUMMARY_FILE"
echo "CPU: $(grep 'model name' /proc/cpuinfo 2>/dev/null | head -1 | cut -d: -f2 | xargs || echo 'unknown')" >> "$SUMMARY_FILE"
echo "========================================" >> "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"

for N in "${ITERATIONS[@]}"; do
    CSV_FILE="$RESULTS_DIR/latency_${N}iter_${TIMESTAMP}.csv"
    echo -e "${BOLD}────────────────────────────────────────────────────────────────${NC}"
    echo -e "${CYAN}[RUN]${NC} Benchmark with ${BOLD}$N${NC} iterations → $CSV_FILE"
    echo -e "${BOLD}────────────────────────────────────────────────────────────────${NC}"

    "$BENCHMARK" "$N" "$CSV_FILE" 2>&1 | tee -a "$SUMMARY_FILE"
    echo "" >> "$SUMMARY_FILE"
    echo ""
done

# Final summary
echo -e "${BOLD}╔══════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║                   ALL BENCHMARKS COMPLETE                      ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${GREEN}Results saved to:${NC}"
for N in "${ITERATIONS[@]}"; do
    echo "  → $RESULTS_DIR/latency_${N}iter_${TIMESTAMP}.csv"
done
echo "  → $SUMMARY_FILE (full summary)"
echo ""

# Also run the security tests if they exist
if [ -x "$SCRIPT_DIR/test_seccmp_graceful" ]; then
    echo -e "${CYAN}[BONUS]${NC} Running security validation tests..."
    echo ""
    "$SCRIPT_DIR/test_seccmp_graceful" && echo -e "${GREEN}✓ Graceful shutdown test PASSED${NC}" || echo -e "${RED}✗ Graceful shutdown test FAILED${NC}"
    echo ""
fi

echo -e "${BOLD}Done! Import CSV files into Excel/matplotlib for thesis charts.${NC}"
echo ""
