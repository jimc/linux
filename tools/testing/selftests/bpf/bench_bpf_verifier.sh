#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Standalone eBPF Verifier Microbenchmarking Helper
# Measures CPU instruction count, cycles, and teardown latency of BPF verification.

set -e

BPF_OBJ="${1:-pyperf600.o}"
REPEATS="${2:-20}"

echo "=========================================================="
echo " eBPF Verifier Performance Microbenchmark (folio_arena)   "
echo " Target BPF Object: ${BPF_OBJ}"
echo " Repeats: ${REPEATS}"
echo "=========================================================="

if ! command -v bpftool >/dev/null 2>&1; then
    if [ -x /usr/sbin/bpftool ]; then
        BPFTOOL=/usr/sbin/bpftool
    else
        BPFTOOL=bpftool
    fi
else
    BPFTOOL=bpftool
fi

PIN_PATH="/sys/fs/bpf/bench_verifier_tmp"

echo "# Running perf stat across ${REPEATS} verifier load/unload passes..."

perf stat -r "${REPEATS}" -e cycles,instructions,branches,branch-misses \
    bash -c "
        for i in \$(seq 1 10); do
            ${BPFTOOL} prog load ${BPF_OBJ} ${PIN_PATH} >/dev/null 2>&1 || true
            rm -f ${PIN_PATH} >/dev/null 2>&1 || true
        done
    "

echo "=========================================================="
echo " Microbenchmark Complete."
echo "=========================================================="
