#!/bin/bash
# merge_into_dpdk.sh — Copy security layer files into the DPDK 23.11 l3fwd tree

SRC="$(cd "$(dirname "$0")" && pwd)"
DST="/home/Saptarshi_1996/dpdk-23.11/examples/l3fwd"

echo "=============================================="
echo "Merging security layer into DPDK l3fwd tree"
echo "  From: $SRC"
echo "  To:   $DST"
echo "=============================================="
echo ""

# Core security layer
cp -v "$SRC/cnf_security_layer.c" "$DST/"
cp -v "$SRC/cnf_security_layer.h" "$DST/"

# Updated main.c with security hooks
cp -v "$SRC/main.c" "$DST/"

# Updated build files
cp -v "$SRC/Makefile" "$DST/"
cp -v "$SRC/meson.build" "$DST/"

# Benchmark and tests
cp -v "$SRC/latency_benchmark.c" "$DST/"
cp -v "$SRC/test_exploit.c" "$DST/"
cp -v "$SRC/test_seccmp_graceful.c" "$DST/"
cp -v "$SRC/test_seccmp_trap.c" "$DST/"
cp -v "$SRC/test_security_layer.sh" "$DST/"
cp -v "$SRC/run_benchmarks.sh" "$DST/"

# Benchmark results
cp -v "$SRC/latency_results.csv" "$DST/"
cp -v "$SRC/latency_results_500.csv" "$DST/"
cp -v "$SRC/results_1000.csv" "$DST/"

echo ""
echo "=============================================="
echo "DONE — All files merged into DPDK l3fwd tree"
echo "=============================================="
echo ""
echo "Next steps:"
echo "  cd $DST"
echo "  make standalone    # Build tests + benchmark (no DPDK needed)"
echo "  make -j\$(nproc)    # Build l3fwd with DPDK + security layer"
