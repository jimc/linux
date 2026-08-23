#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# scratchpad_bench.sh - Unified A/B Performance Testing Harness for Simple-Slab
# Compares standard SLUB/kmem_cache baseline vs Simple-Slab / Scratchpad
#

SANITY_MODE=0
TESTS_RUN=0
TESTS_SKIPPED=0

# ==============================================================================
# 0. io_uring: Alloc-cache (SLUB array vs Scratchrec compound pool)
# ==============================================================================
bench_io_uring() {
    local param="/sys/module/io_uring/parameters/cache_scratch"
    local base_out="/tmp/fio_baseline.txt"
    local cand_out="/tmp/fio_scratchrec.txt"

    if ! command -v fio >/dev/null 2>&1; then
        echo "SKIP: fio not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    if [ "$SANITY_MODE" -eq 1 ]; then
        echo ">>> Running io_uring Sanity / Baseline Single Pass (2s random read)..."
        fio --name=iouring --ioengine=io_uring --iodepth=128 \
            --rw=randread --bs=4k --size=64M --time_based --runtime=2 \
            --filename=/tmp/test_io --direct=1
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: io_uring parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running io_uring A/B Benchmark (5s random read, iodepth=128)..."

    # 0. Baseline (SLUB array cache)
    echo 0 > "$param"
    fio --name=iouring --ioengine=io_uring --iodepth=128 \
        --rw=randread --bs=4k --size=64M --time_based --runtime=5 \
        --filename=/tmp/test_io --direct=1 > "$base_out"

    # 1. Candidate (Scratchrec intrusive pool)
    echo 1 > "$param"
    fio --name=iouring --ioengine=io_uring --iodepth=128 \
        --rw=randread --bs=4k --size=64M --time_based --runtime=5 \
        --filename=/tmp/test_io --direct=1 > "$cand_out"

    ((TESTS_RUN++))

    echo ""
    echo "=== io_uring: Baseline A (<) vs Scratchrec B (>) ==="
    diff -w -I "^iouring: (groupid=" "$base_out" "$cand_out" || true
    echo ""
}

# ==============================================================================
# 1. netfilter: Transaction objects (kmem_cache vs Scratchpad)
# ==============================================================================
bench_nftables() {
    local param="/sys/module/nf_tables/parameters/trans_scratch"
    local base_out="/tmp/nft_baseline.txt"
    local cand_out="/tmp/nft_scratchpad.txt"
    local nft_batch="/tmp/bench_batch.nft"
    local table="perf_cp_table"
    local set="perf_cp_set"
    local num_elems=1000
    local repeats=5

    if ! command -v nft >/dev/null 2>&1 || ! command -v perf >/dev/null 2>&1; then
        echo "SKIP: nft or perf not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Generating 1000-element nftables batch..."
    cat <<NFT_EOF > "$nft_batch"
table ip ${table} {
    set ${set} {
        type ipv4_addr
        flags interval
        elements = {
NFT_EOF
    for i in $(seq 1 ${num_elems}); do
        o1=$(( (i >> 16) & 255 ))
        o2=$(( (i >> 8) & 255 ))
        o3=$(( i & 255 ))
        echo "            10.${o1}.${o2}.${o3}," >> "$nft_batch"
    done
    cat <<NFT_EOF >> "$nft_batch"
        }
    }
}
NFT_EOF

    local cmd="
        for pass in \$(seq 1 5); do
            nft -f $nft_batch >/dev/null 2>&1 || true
            nft delete table ip $table >/dev/null 2>&1 || true
        done
    "

    if [ "$SANITY_MODE" -eq 1 ]; then
        echo ">>> Running nf_tables Sanity / Baseline Single Pass (1000 elems)..."
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd"
        rm -f "$nft_batch"
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: nf_tables parameter ($param) not found"
        rm -f "$nft_batch"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running nf_tables A/B Benchmark (5 repeats, 1000 elems)..."

    # 0. Baseline (kmem_cache SLUB)
    echo 0 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        bash -c "$cmd" > "$base_out" 2>&1

    # 1. Candidate (Scratchpad transactional arena)
    echo 1 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        bash -c "$cmd" > "$cand_out" 2>&1

    rm -f "$nft_batch"
    ((TESTS_RUN++))

    echo ""
    echo "=== nf_tables: Baseline A (<) vs Scratchpad B (>) ==="
    diff -w "$base_out" "$cand_out" || true
    echo ""
}

# ==============================================================================
# 2. bpf_verifier: Verifier stack states (kmem_cache vs Scratchrec)
# ==============================================================================
bench_bpf_verifier() {
    local param="/sys/module/bpf/parameters/verifier_scratch"
    local base_out="/tmp/verifier_baseline.txt"
    local cand_out="/tmp/verifier_scratchrec.txt"
    local verifier_bin=""
    local candidate_paths=(
        "./test_verifier"
        "tools/testing/selftests/bpf/test_verifier"
        "../tools/testing/selftests/bpf/test_verifier"
        "../../tools/testing/selftests/bpf/test_verifier"
        "b0-dd/tools/testing/selftests/bpf/test_verifier"
        "b0-dd/kselftest/bpf/test_verifier"
    )

    if command -v test_verifier >/dev/null 2>&1; then
        verifier_bin="test_verifier"
    else
        for p in "${candidate_paths[@]}"; do
            if [ -x "$p" ]; then
                verifier_bin="$p"
                break
            fi
        done
    fi

    if [ -z "$verifier_bin" ]; then
        echo "SKIP: test_verifier binary not found"
        echo "      Build with: make -C tools/testing/selftests/bpf test_verifier"
        ((TESTS_SKIPPED++))
        return 0
    fi
    if ! command -v perf >/dev/null 2>&1; then
        echo "SKIP: perf not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    if [ "$SANITY_MODE" -eq 1 ]; then
        echo ">>> Running BPF Verifier Sanity / Baseline Single Pass..."
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            "$verifier_bin" >/dev/null 2>&1 || true
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: bpf verifier parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    local repeats=3

    echo ">>> Running BPF Verifier A/B Benchmark..."

    # 0. Baseline (kmem_cache)
    echo 0 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        "$verifier_bin" > "$base_out" 2>&1 || true

    # 1. Candidate (Scratchrec pool)
    echo 1 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        "$verifier_bin" > "$cand_out" 2>&1 || true

    ((TESTS_RUN++))

    echo ""
    echo "=== BPF Verifier: Baseline A (<) vs Scratchrec B (>) ==="
    diff -w "$base_out" "$cand_out" || true
    echo ""
}

# ==============================================================================
# 3. bpf_jit: JIT compilation jump tables & trampoline images
# ==============================================================================
bench_bpf_jit() {
    local param="/sys/module/bpf/parameters/jit_scratch"
    local base_out="/tmp/jit_baseline.txt"
    local cand_out="/tmp/jit_scratchpad.txt"
    local repeats=3

    if ! command -v perf >/dev/null 2>&1; then
        echo "SKIP: perf not found"
        ((TESTS_SKIPPED++))
        return 0
    fi
    if ! command -v bpftool >/dev/null 2>&1 && [ ! -x "./test_progs" ]; then
        echo "SKIP: bpftool or test_progs not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    local cmd="bpftool prog load /tmp/test_prog.o /sys/fs/bpf/test 2>/dev/null || true"
    if [ -x "./test_progs" ]; then
        cmd="./test_progs -t bpf_cookie 2>/dev/null || true"
    fi

    if [ "$SANITY_MODE" -eq 1 ]; then
        echo ">>> Running BPF JIT Sanity / Baseline Single Pass..."
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd"
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: bpf JIT parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running BPF JIT A/B Benchmark..."

    # 0. Baseline (kvmalloc)
    echo 0 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        bash -c "$cmd" > "$base_out" 2>&1

    # 1. Candidate (Scratchpad arena)
    echo 1 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        bash -c "$cmd" > "$cand_out" 2>&1

    ((TESTS_RUN++))

    echo ""
    echo "=== BPF JIT: Baseline A (<) vs Scratchpad B (>) ==="
    diff -w "$base_out" "$cand_out" || true
    echo ""
}

# ==============================================================================
# 4. modload: Module loader decompression workspaces & page tracking
# ==============================================================================
bench_modload() {
    local param="/sys/module/module/parameters/modload_scratch"
    local base_out="/tmp/modload_baseline.txt"
    local cand_out="/tmp/modload_scratchpad.txt"
    local repeats=5
    local mod_target=""

    if ! command -v perf >/dev/null 2>&1; then
        echo "SKIP: perf not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    # Find an unloadable/reloadable test module
    for m in dummy loop null_blk brd; do
        if modprobe "$m" 2>/dev/null; then
            rmmod "$m" 2>/dev/null
            mod_target="$m"
            break
        fi
    done

    if [ -z "$mod_target" ]; then
        echo "SKIP: no suitable test module found (dummy/loop/null_blk/brd)"
        ((TESTS_SKIPPED++))
        return 0
    fi

    local cmd="for i in \$(seq 1 20); do \
modprobe $mod_target >/dev/null 2>&1; \
rmmod $mod_target >/dev/null 2>&1; done"

    if [ "$SANITY_MODE" -eq 1 ]; then
        echo ">>> Running Module Loader Sanity Pass (module: $mod_target, 20 loads)..."
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd"
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: module loader parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running Module Loader A/B Benchmark (module: $mod_target, 20 loads)..."

    # 0. Baseline (kvmalloc)
    echo 0 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        bash -c "$cmd" > "$base_out" 2>&1

    # 1. Candidate (Scratchpad arena)
    echo 1 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        bash -c "$cmd" > "$cand_out" 2>&1

    ((TESTS_RUN++))

    echo ""
    echo "=== Module Loader: Baseline A (<) vs Scratchpad B (>) ==="
    diff -w "$base_out" "$cand_out" || true
    echo ""
}

# ==============================================================================
# 5. maple_tree: Per-CPU node allocation & recycling (SLUB vs Scratchrec)
# ==============================================================================
bench_maple_tree() {
    local param="/sys/module/maple_tree/parameters/node_scratch"
    local base_out="/tmp/maple_baseline.txt"
    local cand_out="/tmp/maple_scratchrec.txt"
    local repeats=3

    if ! command -v perf >/dev/null 2>&1; then
        echo "SKIP: perf not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    # Test via test_maple_tree if available, or hackbench / mmap loop
    local cmd="modprobe -r test_maple_tree 2>/dev/null; \
modprobe test_maple_tree; modprobe -r test_maple_tree"
    if ! modprobe test_maple_tree 2>/dev/null; then
        if command -v hackbench >/dev/null 2>&1; then
            cmd="hackbench -l 500"
        else
            cmd="python3 -c 'import mmap; [mmap.mmap(-1, 4096*100) for _ in range(1000)]'"
        fi
    else
        modprobe -r test_maple_tree 2>/dev/null || true
    fi

    if [ "$SANITY_MODE" -eq 1 ]; then
        echo ">>> Running Maple Tree Sanity / Baseline Single Pass..."
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd"
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: maple tree parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running Maple Tree A/B Benchmark..."

    # 0. Baseline (kmem_cache SLUB)
    echo 0 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        bash -c "$cmd" > "$base_out" 2>&1

    # 1. Candidate (Scratchrec per-CPU pool)
    echo 1 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        bash -c "$cmd" > "$cand_out" 2>&1

    ((TESTS_RUN++))

    echo ""
    echo "=== Maple Tree: Baseline A (<) vs Scratchrec B (>) ==="
    diff -w "$base_out" "$cand_out" || true
    echo ""
}

# ==============================================================================
# 6. genetlink: Generic Netlink attribute parsing
# ==============================================================================
bench_genetlink() {
    local param="/sys/module/genetlink/parameters/attr_scratch"
    local base_out="/tmp/genl_baseline.txt"
    local cand_out="/tmp/genl_scratchpad.txt"
    local repeats=5

    if ! command -v perf >/dev/null 2>&1; then
        echo "SKIP: perf not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    local cmd="for i in \$(seq 1 300); do \
ethtool -k lo >/dev/null 2>&1 || devlink dev show >/dev/null 2>&1 || \
genl ctrl list >/dev/null 2>&1; done"

    if [ "$SANITY_MODE" -eq 1 ]; then
        echo ">>> Running Generic Netlink Sanity / Baseline Single Pass (300 queries)..."
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd"
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: genetlink parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running Generic Netlink A/B Benchmark..."

    # 0. Baseline (kmem_cache SLUB)
    echo 0 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        bash -c "$cmd" > "$base_out" 2>&1

    # 1. Candidate (Per-CPU Scratchpad)
    echo 1 > "$param"
    perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
        bash -c "$cmd" > "$cand_out" 2>&1

    ((TESTS_RUN++))

    echo ""
    echo "=== Generic Netlink: Baseline A (<) vs Scratchpad B (>) ==="
    diff -w "$base_out" "$cand_out" || true
    echo ""
}

# ==============================================================================
# Main Dispatch
# ==============================================================================
dispatch_target() {
    local target="$1"
    case "$target" in
        all)
            bench_bpf_verifier
            bench_nftables
            bench_drm_gpuvm
            bench_io_uring
            bench_module_loader
            bench_maple_tree
            bench_genetlink
            ;;
        verifier|bpf)
            bench_bpf_verifier
            ;;
        nftables|netfilter)
            bench_nftables
            ;;
        gpuvm|drm)
            bench_drm_gpuvm
            ;;
        io_uring|uring)
            bench_io_uring
            ;;
        module|kmod)
            bench_module_loader
            ;;
        maple|maple_tree)
            bench_maple_tree
            ;;
        genl|genetlink)
            bench_genetlink
            ;;
        *)
            echo "Unknown benchmark target: $target"
            echo "Available: all, verifier, nftables, gpuvm, io_uring, module, maple_tree, genetlink"
            ;;
    esac
}

# CLI Argument parsing
TARGETS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -b|--baseline|sanity)
            SANITY_MODE=1
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [-b|--baseline|sanity] [target...]"
            echo "Targets: all (default), verifier, nftables, gpuvm, io_uring, module, maple_tree, genetlink"
            exit 0
            ;;
        *)
            TARGETS+=("$1")
            shift
            ;;
    esac
done

if [ ${#TARGETS[@]} -eq 0 ]; then
    TARGETS=("all")
fi

for t in "${TARGETS[@]}"; do
    dispatch_target "$t"
done

if [ "$SANITY_MODE" -eq 0 ] && [ "$TESTS_RUN" -eq 0 ]; then
    echo "=============================================================================="
    echo "NOTICE: All $TESTS_SKIPPED benchmark(s) skipped (subsystem *_scratch knobs missing)."
    echo "To run single-pass baseline measurements on this kernel, rerun with -b / sanity:"
    echo "    $0 -b"
    echo "    $0 sanity ${TARGETS[*]}"
    echo "=============================================================================="
fi
