#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# scratch-perf.sh - Unified Performance & Locking Stress Testing Harness
# Compares SLUB/kmem_cache baseline vs Simple-Slab / Scratchpad
# Samples live lockdep slab reservoir allocation deltas after each pass.
#

RUN_MODE="AB"
SANITY_MODE=0
TESTS_RUN=0
TESTS_SKIPPED=0

# ==============================================================================
# Lockdep Live Slab Telemetry & Delta Sampler
# ==============================================================================
sample_lockdep_snap() {
    [ -f /proc/lockdep_stats ] || return 0
    awk '
        /used slabs:/                  { used=$3 }
        /total slabs:/                 { total=$3 }
        /lock-classes:/                { classes=$2 }
        /direct dependencies:/         { deps=$3 }
        /dependency chains:/           { chains=$3 }
        /dependency chain hlocks:/     { hlocks=$4 }
        /stack-trace entries:/         { trace=$3 }
        END {
            if (total != "")
                print used, total, classes, deps, chains, hlocks, trace;
            else
                print "none"
        }
    ' /proc/lockdep_stats
}

report_lockdep_delta() {
    local label="$1"
    local before=($2)
    local after=($(sample_lockdep_snap))

    if [ "${#before[@]}" -ge 7 ] && [ "${#after[@]}" -ge 7 ] && [ "${before[0]}" != "none" ]; then
        local d_slabs=$(( after[0] - before[0] ))
        local d_classes=$(( after[2] - before[2] ))
        local d_deps=$(( after[3] - before[3] ))
        local d_chains=$(( after[4] - before[4] ))
        local d_hlocks=$(( after[5] - before[5] ))
        local d_trace=$(( after[6] - before[6] ))

        printf "  [lockdep %s] slabs: %d/%d (%u kB) | delta: %+d slabs, %+d classes, %+d deps, %+d chains, %+d traces\n" \
            "$label" "${after[0]}" "${after[1]}" "$(( after[0] * 64 ))" \
            "$d_slabs" "$d_classes" "$d_deps" "$d_chains" "$d_trace"
    fi
}

# ==============================================================================
# Generic A/B Test Execution Engine
# ==============================================================================
execute_ab_test() {
    local title="$1"
    local param="$2"
    local base_out="$3"
    local cand_out="$4"
    local run_fn="$5"
    local -a diff_filter=()
    [ -n "${6:-}" ] && diff_filter=(-I "$6")

    run_single_ab_pass() {
        local order="$1"
        local b_out="$2"
        local c_out="$3"

        # Warmup pass to prime buffer caches and eliminate VM cold-start skew
        $run_fn "/dev/null" >/dev/null 2>&1 || true

        if [ "$order" = "AB" ]; then
            local snap0=$(sample_lockdep_snap)
            echo 0 > "$param"
            $run_fn "$b_out"
            report_lockdep_delta "Pass A (Baseline)" "$snap0"

            local snap1=$(sample_lockdep_snap)
            echo 1 > "$param"
            $run_fn "$c_out"
            report_lockdep_delta "Pass B (Candidate)" "$snap1"

            echo ""
            echo "=== $title: [A -> B] Baseline A (<) vs Candidate B (>) ==="
        else
            local snap0=$(sample_lockdep_snap)
            echo 1 > "$param"
            $run_fn "$c_out"
            report_lockdep_delta "Pass B (Candidate)" "$snap0"

            local snap1=$(sample_lockdep_snap)
            echo 0 > "$param"
            $run_fn "$b_out"
            report_lockdep_delta "Pass A (Baseline)" "$snap1"

            echo ""
            echo "=== $title: [B -> A (Reversed)] Baseline A (<) vs Candidate B (>) ==="
        fi

        # Check if the outputs are perf stat files to render a structured table
        if grep -q "cycles" "$b_out" 2>/dev/null && grep -q "cycles" "$c_out" 2>/dev/null; then
            awk '
            function clean_val(v) {
                gsub(/,/, "", v);
                return v + 0;
            }
            function format_num(val) {
                sign = (val < 0) ? "-" : "";
                aval = (val < 0) ? -val : val;
                if (aval >= 1e9) return sprintf("%s%.2fB", sign, aval / 1e9);
                if (aval >= 1e6) return sprintf("%s%.2fM", sign, aval / 1e6);
                if (aval >= 1e3) return sprintf("%s%.2fK", sign, aval / 1e3);
                return sprintf("%s%.0f", sign, aval);
            }
            NR==FNR {
                if ($0 ~ /cycles|instructions|branches|branch-misses|dTLB-load-misses/) {
                    metric = $2;
                    val = clean_val($1);
                    b_val[metric] = val;
                    order[++m_count] = metric;
                } else if ($0 ~ /seconds time elapsed/) {
                    metric = "time_elapsed";
                    b_val[metric] = clean_val($1);
                    order[++m_count] = metric;
                }
                next;
            }
            {
                if ($0 ~ /cycles|instructions|branches|branch-misses|dTLB-load-misses/) {
                    metric = $2;
                    val = clean_val($1);
                    c_val[metric] = val;
                } else if ($0 ~ /seconds time elapsed/) {
                    metric = "time_elapsed";
                    c_val[metric] = clean_val($1);
                }
            }
            END {
                if (m_count > 0) {
                    printf "%-18s | %-12s | %-12s | %-12s | %-8s\n", "Metric", "Baseline (A)", "Cand (B)", "Delta", "% Delta";
                    printf "%s\n", "-------------------+--------------+--------------+--------------+---------";
                    for (i = 1; i <= m_count; i++) {
                        m = order[i];
                        bv = b_val[m];
                        cv = c_val[m];
                        if (bv == 0 && cv == 0) continue;
                        diff = cv - bv;
                        pct = (bv > 0) ? ((diff * 100.0) / bv) : 0;
                        if (m == "time_elapsed") {
                            printf "%-18s | %10.3fs | %10.3fs | %+10.3fs | %+6.2f%%\n", m, bv, cv, diff, pct;
                        } else {
                            printf "%-18s | %12s | %12s | %+12s | %+6.2f%%\n", m, format_num(bv), format_num(cv), format_num(diff), pct;
                        }
                    }
                    print "";
                }
            }' "$b_out" "$c_out"
        else
            diff -w "${diff_filter[@]}" "$b_out" "$c_out" || true
        fi
        echo ""
    }

    if [ "$RUN_MODE" = "AB" ]; then
        run_single_ab_pass "AB" "$base_out" "$cand_out"
    elif [ "$RUN_MODE" = "BA" ]; then
        run_single_ab_pass "BA" "$base_out" "$cand_out"
    elif [ "$RUN_MODE" = "BOTH" ]; then
        local b_ab="${base_out}.ab"
        local c_ab="${cand_out}.ab"
        local b_ba="${base_out}.ba"
        local c_ba="${cand_out}.ba"
        run_single_ab_pass "AB" "$b_ab" "$c_ab"
        run_single_ab_pass "BA" "$b_ba" "$c_ba"
    fi
}

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
        local snap=$(sample_lockdep_snap)
        fio --name=iouring --ioengine=io_uring --iodepth=128 \
            --rw=randread --bs=4k --size=64M --time_based --runtime=2 \
            --filename=/tmp/test_io --direct=1
        report_lockdep_delta "Single Pass" "$snap"
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: io_uring parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running io_uring Benchmark (5s random read, iodepth=128, mode: $RUN_MODE)..."

    do_fio() {
        local out="$1"
        fio --name=iouring --ioengine=io_uring --iodepth=128 \
            --rw=randread --bs=4k --size=64M --time_based --runtime=5 \
            --filename=/tmp/test_io --direct=1 > "$out"
    }

    execute_ab_test "io_uring" "$param" "$base_out" "$cand_out" do_fio "^iouring: (groupid="
    ((TESTS_RUN++))
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
        local snap=$(sample_lockdep_snap)
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd"
        report_lockdep_delta "Single Pass" "$snap"
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

    echo ">>> Running nf_tables Benchmark (5 repeats, 1000 elems, mode: $RUN_MODE)..."

    do_nft() {
        local out="$1"
        perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd" > "$out" 2>&1
    }

    execute_ab_test "nf_tables" "$param" "$base_out" "$cand_out" do_nft
    rm -f "$nft_batch"
    ((TESTS_RUN++))
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
        local snap=$(sample_lockdep_snap)
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            "$verifier_bin" >/dev/null 2>&1 || true
        report_lockdep_delta "Single Pass" "$snap"
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
    echo ">>> Running BPF Verifier Benchmark (mode: $RUN_MODE)..."

    do_verifier() {
        local out="$1"
        perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
            "$verifier_bin" > "$out" 2>&1 || true
    }

    execute_ab_test "BPF Verifier" "$param" "$base_out" "$cand_out" do_verifier
    ((TESTS_RUN++))
}

# ==============================================================================
# 3. bpf_jit: JIT compilation jump tables & trampoline images
# ==============================================================================
bench_bpf_jit() {
    local param="/sys/module/bpf/parameters/jit_scratch"
    local base_out="/tmp/jit_baseline.txt"
    local cand_out="/tmp/jit_scratchpad.txt"
    local repeats=10

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
        local snap=$(sample_lockdep_snap)
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd"
        report_lockdep_delta "Single Pass" "$snap"
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: bpf JIT parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running BPF JIT Benchmark (mode: $RUN_MODE)..."

    do_jit() {
        local out="$1"
        perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd" > "$out" 2>&1
    }

    execute_ab_test "BPF JIT" "$param" "$base_out" "$cand_out" do_jit
    ((TESTS_RUN++))
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

    local cmd="for i in \$(seq 1 20); do modprobe $mod_target >/dev/null 2>&1; rmmod $mod_target >/dev/null 2>&1; done"

    if [ "$SANITY_MODE" -eq 1 ]; then
        echo ">>> Running Module Loader Sanity / Baseline Single Pass (module: $mod_target, 20 loads)..."
        local snap=$(sample_lockdep_snap)
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd"
        report_lockdep_delta "Single Pass" "$snap"
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: module loader parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running Module Loader Benchmark (module: $mod_target, 20 loads, mode: $RUN_MODE)..."

    do_modload() {
        local out="$1"
        perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd" > "$out" 2>&1
    }

    execute_ab_test "Module Loader" "$param" "$base_out" "$cand_out" do_modload
    ((TESTS_RUN++))
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

    # Benchmark real process address space VMA tree churn (hackbench -p) or test_maple_tree
    local cmd=""
    if command -v hackbench >/dev/null 2>&1; then
        cmd="hackbench -p -g 4 -l 500"
    else
        local ko=""
        for p in ./lib/test_maple_tree.ko lib/test_maple_tree.ko /lib/modules/$(uname -r)/kernel/lib/test_maple_tree.ko; do
            if [ -f "$p" ]; then
                ko="$p"
                break
            fi
        done
        if [ -n "$ko" ]; then
            cmd="rmmod test_maple_tree 2>/dev/null; insmod $ko; rmmod test_maple_tree 2>/dev/null"
        else
            cmd="python3 -c 'import mmap; [mmap.mmap(-1, 4096*100) for _ in range(1000)]'"
        fi
    fi

    local perf_events="cycles,instructions,branches,branch-misses,dTLB-load-misses"

    if [ "$SANITY_MODE" -eq 1 ]; then
        echo ">>> Running Maple Tree / VMA Sanity Single Pass..."
        local snap=$(sample_lockdep_snap)
        perf stat -r 1 -e "$perf_events" bash -c "$cmd"
        report_lockdep_delta "Single Pass" "$snap"
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: maple tree parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running Maple Tree VMA Process Churn Benchmark (hackbench -p, mode: $RUN_MODE)..."

    do_maple() {
        local out="$1"
        perf stat -r "$repeats" -e "$perf_events" bash -c "$cmd" > "$out" 2>&1
    }

    execute_ab_test "Maple Tree VMA" "$param" "$base_out" "$cand_out" do_maple
    ((TESTS_RUN++))
}

# ==============================================================================
# 5b. bonsai_tree: Potted 25-way compact range tree with repotting
# ==============================================================================
bench_bonsai() {
    local ko=""
    for p in ./lib/test_bonsai_tree.ko lib/test_bonsai_tree.ko /lib/modules/$(uname -r)/kernel/lib/test_bonsai_tree.ko; do
        if [ -f "$p" ]; then
            ko="$p"
            break
        fi
    done

    if [ -z "$ko" ] && ! modprobe -n test_bonsai_tree 2>/dev/null; then
        echo "SKIP: test_bonsai_tree module not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running Bonsai Tree Benchmark (insertion, 25-way splits, lookups, repotting)..."
    local cmd="rmmod test_bonsai_tree 2>/dev/null; insmod ${ko:-lib/test_bonsai_tree.ko}; rmmod test_bonsai_tree 2>/dev/null"

    local snap=$(sample_lockdep_snap)
    if command -v perf >/dev/null 2>&1; then
        perf stat -r 3 -e cycles,instructions,branches,branch-misses,dTLB-load-misses bash -c "$cmd" 2>&1
    else
        bash -c "$cmd"
    fi
    report_lockdep_delta "Bonsai Tree" "$snap"
    ((TESTS_RUN++))
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

    local cmd="for i in \$(seq 1 300); do ethtool -k lo >/dev/null 2>&1 || devlink dev show >/dev/null 2>&1 || genl ctrl list >/dev/null 2>&1; done"

    if [ "$SANITY_MODE" -eq 1 ]; then
        echo ">>> Running Generic Netlink Sanity / Baseline Single Pass (300 queries)..."
        local snap=$(sample_lockdep_snap)
        perf stat -r 1 -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd"
        report_lockdep_delta "Single Pass" "$snap"
        ((TESTS_RUN++))
        echo ""
        return 0
    fi

    if [ ! -f "$param" ]; then
        echo "SKIP: genetlink parameter ($param) not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running Generic Netlink (genetlink) Benchmark (300 queries, mode: $RUN_MODE)..."

    do_genl() {
        local out="$1"
        perf stat -r "$repeats" -e cycles,instructions,branches,branch-misses \
            bash -c "$cmd" > "$out" 2>&1
    }

    execute_ab_test "Generic Netlink" "$param" "$base_out" "$cand_out" do_genl
    ((TESTS_RUN++))
}

# ==============================================================================
# 7. Locking & System Stress Workloads (Lockdep Exploration)
# ==============================================================================
bench_hackbench() {
    if ! command -v hackbench >/dev/null 2>&1; then
        echo "SKIP: hackbench not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running hackbench Stress Test (8 groups, 1000 loops)..."
    local snap=$(sample_lockdep_snap)
    if command -v perf >/dev/null 2>&1; then
        perf stat -r 1 -e cycles,instructions,branches,branch-misses hackbench -p -g 8 -l 1000 2>&1
    else
        hackbench -p -g 8 -l 1000
    fi
    report_lockdep_delta "hackbench" "$snap"
    ((TESTS_RUN++))
    echo ""
}

bench_netns() {
    if ! command -v ip >/dev/null 2>&1; then
        echo "SKIP: ip command not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo ">>> Running Network Namespace & Veth Churn Storm (40 netns)..."
    mkdir -p /var/run/netns 2>/dev/null || true
    local snap=$(sample_lockdep_snap)

    do_netns_storm() {
        for i in $(seq 1 40); do
            ip netns add "pns_$i" 2>/dev/null || true
            ip link add "vA_$i" type veth peer name "vB_$i" 2>/dev/null || true
            ip link set "vB_$i" netns "pns_$i" 2>/dev/null || true
        done
        for i in $(seq 1 40); do
            ip link del "vA_$i" 2>/dev/null || true
            ip netns del "pns_$i" 2>/dev/null || true
        done
    }

    if command -v perf >/dev/null 2>&1; then
        perf stat -r 1 -e cycles,instructions,branches,branch-misses bash -c "$(declare -f do_netns_storm); do_netns_storm" 2>&1
    else
        do_netns_storm
    fi
    report_lockdep_delta "netns storm" "$snap"
    ((TESTS_RUN++))
    echo ""
}

bench_vfs() {
    echo ">>> Running VFS Inode/Dentry Directory Storm..."
    local testdir="/tmp/vfs_bench_$$"
    mkdir -p "$testdir"
    local snap=$(sample_lockdep_snap)

    do_vfs_storm() {
        for w in $(seq 1 8); do
            (
                mkdir -p "$testdir/w_$w"
                for i in $(seq 1 200); do
                    mkdir -p "$testdir/w_$w/d_$i"
                    touch "$testdir/w_$w/d_$i/f_$i"
                    ln -s "f_$i" "$testdir/w_$w/d_$i/s_$i" 2>/dev/null || true
                done
                rm -rf "$testdir/w_$w"
            ) &
        done
        wait
    }

    if command -v perf >/dev/null 2>&1; then
        perf stat -r 1 -e cycles,instructions,branches,branch-misses bash -c "$(declare -f do_vfs_storm); testdir='$testdir'; do_vfs_storm" 2>&1
    else
        do_vfs_storm
    fi
    rm -rf "$testdir"
    report_lockdep_delta "vfs storm" "$snap"
    ((TESTS_RUN++))
    echo ""
}

bench_modstorm() {
    echo ">>> Running Kernel Module Load/Unload Storm..."
    local avail_mods=()
    for m in dummy loop null_blk brd tun; do
        if modprobe "$m" 2>/dev/null; then
            rmmod "$m" 2>/dev/null
            avail_mods+=("$m")
        fi
    done

    if [ ${#avail_mods[@]} -eq 0 ]; then
        echo "SKIP: no loadable test modules found (dummy/loop/null_blk/brd/tun)"
        ((TESTS_SKIPPED++))
        return 0
    fi

    local snap=$(sample_lockdep_snap)
    do_mod_storm() {
        for round in $(seq 1 10); do
            for m in "${avail_mods[@]}"; do
                modprobe "$m" 2>/dev/null || true
            done
            for m in "${avail_mods[@]}"; do
                rmmod "$m" 2>/dev/null || true
            done
        done
    }

    if command -v perf >/dev/null 2>&1; then
        perf stat -r 1 -e cycles,instructions,branches,branch-misses bash -c "$(declare -f do_mod_storm); avail_mods=(${avail_mods[*]}); do_mod_storm" 2>&1
    else
        do_mod_storm
    fi
    report_lockdep_delta "modstorm" "$snap"
    ((TESTS_RUN++))
    echo ""
}

# ==============================================================================
# Main Runner Dispatcher
# ==============================================================================
usage() {
    echo "Usage: $0 [-r|--reverse] [-b|--both] [-s|--sanity|--baseline] [target...]"
    echo ""
    echo "Target Subsets:"
    echo "  all (default)              Run all substrate A/B and locking stress tests"
    echo "  ab, substrate, scratch     Run all substrate A/B tests (io_uring, nft, etc.)"
    echo "  stress, lockdep            Run locking/system stress tests (hackbench, netns, etc.)"
    echo ""
    echo "Individual Targets:"
    echo "  iouring, nft, verifier, jit, modload, maple, bonsai, genl"
    echo "  hackbench, netns, vfs, modstorm"
    echo ""
    echo "Options:"
    echo "  -r, --reverse              Run candidate B first, then baseline A (B primes caches)"
    echo "  -b, --both                 Run both passes: A -> B and B -> A (evaluates priming bias)"
    echo "  -s, --sanity, --baseline   Run single-pass sanity/baseline (ignores missing sysfs knobs)"
    exit 1
}

dispatch_target() {
    local target="$1"
    case "$target" in
        iouring)
            bench_io_uring
            ;;
        nft|nftables)
            bench_nftables
            ;;
        verifier)
            bench_bpf_verifier
            ;;
        jit)
            bench_bpf_jit
            ;;
        modload|module)
            bench_modload
            ;;
        maple|mapletree)
            bench_maple_tree
            ;;
        bonsai|bonsaitree)
            bench_bonsai
            ;;
        genl|genetlink)
            bench_genetlink
            ;;
        hackbench)
            bench_hackbench
            ;;
        netns)
            bench_netns
            ;;
        vfs)
            bench_vfs
            ;;
        modstorm)
            bench_modstorm
            ;;
        ab|substrate|scratch)
            bench_io_uring
            bench_nftables
            bench_bpf_verifier
            bench_bpf_jit
            bench_modload
            bench_maple_tree
            bench_genetlink
            ;;
        stress|lockdep)
            bench_hackbench
            bench_netns
            bench_vfs
            bench_modstorm
            ;;
        all)
            bench_io_uring
            bench_nftables
            bench_bpf_verifier
            bench_bpf_jit
            bench_modload
            bench_maple_tree
            bench_genetlink
            bench_hackbench
            bench_netns
            bench_vfs
            bench_modstorm
            ;;
        *)
            usage
            ;;
    esac
}

TARGETS=()
for arg in "$@"; do
    case "$arg" in
        -r|--reverse|reverse)
            RUN_MODE="BA"
            ;;
        -b|--both|both)
            RUN_MODE="BOTH"
            ;;
        -s|--sanity|--baseline|sanity|baseline)
            SANITY_MODE=1
            ;;
        -h|--help)
            usage
            ;;
        *)
            TARGETS+=("$arg")
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
    echo "NOTICE: All $TESTS_SKIPPED benchmark(s) were skipped (subsystem *_scratch knobs not found)."
    echo "To run single-pass baseline measurements on this kernel, rerun with -s / sanity:"
    echo "    $0 -s ab"
    echo "    $0 -s all"
    echo "=============================================================================="
fi
