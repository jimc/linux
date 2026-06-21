#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

# Standard kselftest exit codes
ksft_pass=0
ksft_fail=1
ksft_skip=4

ESC=$'\033'
RED="${ESC}[0;31m"
GREEN="${ESC}[0;32m"
YELLOW="${ESC}[0;33m"
BLUE="${ESC}[0;34m"
MAGENTA="${ESC}[0;35m"
CYAN="${ESC}[0;36m"
NC="${ESC}[0;0m"
CUMULATIVE_DDCMDS="init"
error_msg=""
V=${V:=0}  # invoke as V=1 $0  for global verbose

[ -e /proc/dynamic_debug/control ] || {
    echo -e "${RED}: this test requires CONFIG_DYNAMIC_DEBUG=y ${NC}"
    exit $ksft_skip # nothing to test here, no good reason to fail.
}

lsmod 2>/dev/null || {
    echo -e "${RED}: lsmod requires /proc/modules ${NC}"
    exit $ksft_skip # maybe later we can do more
}

# need info to avoid failures due to untestable configs

[ -f "$KCONFIG_CONFIG" ] || KCONFIG_CONFIG=".config"
if [ -f "$KCONFIG_CONFIG" ]; then
    echo "# consulting KCONFIG_CONFIG: $KCONFIG_CONFIG"
    grep -q "CONFIG_DYNAMIC_DEBUG=y" $KCONFIG_CONFIG ; LACK_DD_BUILTIN=$?
    grep -q "CONFIG_TEST_DYNAMIC_DEBUG=m" $KCONFIG_CONFIG ; LACK_TMOD=$?
else
    # if no config, try runtime probes
    modprobe -n test_dynamic_debug 2>/dev/null ; LACK_TMOD=$?
    # assume builtin dyndbg if control exists (checked above)
    LACK_DD_BUILTIN=0
fi

# ==============================================================================
# TESTING STRATEGY 1.
#   Change and observe control-file settings:
#     ddcmd: ie echo $dd_query_cmd > /proc/dynamic_debug/control
#     read back control, count changes due to query_cmd
# ==============================================================================

function ddcmd () {
    exp_exit_code=0
    num_args=$#
    if [ "${@:$#}" = "pass" ]; then
	num_args=$#-1
    elif [ "${@:$#}" = "fail" ]; then
        num_args=$#-1
	exp_exit_code=1
    fi
    args=${@:1:$num_args}
    
    # Update cumulative state-machine lineage
    if [[ "$args" == *"=_"* ]]; then
        CUMULATIVE_DDCMDS="$args"
    else
        CUMULATIVE_DDCMDS="${CUMULATIVE_DDCMDS} -> $args"
    fi

    output=$( (echo "$args" > /proc/dynamic_debug/control) 2>&1)
    exit_code=$?
    error_msg=$(echo "$output" | cut -d ":" -f 5 | sed -e 's/^[[:space:]]*//')
    handle_exit_code $BASH_LINENO $FUNCNAME $exit_code $exp_exit_code
}

function handle_exit_code() {
    local exp_exit_code=0
    [ $# == 4 ] && exp_exit_code=$4
    if [ "$3" -ne $exp_exit_code ]; then
        echo -e "${RED}: $BASH_SOURCE:$1 $2() expected to exit with code $exp_exit_code, got $3${NC}"
	[ "$3" == 1 ] && echo "Error: '$error_msg'"
        exit $ksft_fail
    fi
}

function count_pr_debugs {
    # count_pr_debugs <pattern_or_star> <expected_flags> <expected_count> [<label>]
    local pattern="$1"
    local expected_flags="$2"
    local expected_count="$3"
    local label="$4"

    # If pattern is '*', map it to '.' to match all lines in grep
    [ "$pattern" = "*" ] && pattern="."

    # Count how many callsites matching our target pattern actually have the expected flags exactly (surrounded by spaces)
    local cnt=$(grep "$pattern" /proc/dynamic_debug/control | grep -c " $expected_flags ")

    # Automatically print matches under verbose V=1 mode, or full state under V=2
    if [ "$V" -eq 1 ]; then
        grep "$pattern" /proc/dynamic_debug/control | grep " $expected_flags " | \
            sed -E "s/ $expected_flags / ${GREEN}${expected_flags}${NC} /g"
    elif [ "$V" -ge 2 ]; then
        grep "$pattern" /proc/dynamic_debug/control | \
            sed -E -e "s/ $expected_flags / ${GREEN}${expected_flags}${NC} /g" \
                   -e "s/ =([_a-z]*[a-z][_a-z]*) / ${RED}=\1${NC} /g"
    fi

    # 1. Assert on Count (Strategy 1)
    if [ "$cnt" -ne "$expected_count" ]; then
        echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO check failed expected $expected_count matches of '$pattern' with flags '$expected_flags', got $cnt${NC}"
        exit $ksft_fail
    else
        [ "$V" -ge 1 ] && echo -e "${GREEN}✔ Matches! [count:$cnt] pattern '$pattern' flags '$expected_flags'${NC}"
    fi

    # 2. Cryptographic Full-State Verification (Strategy 2 - Optional)
    if [ -n "$label" ]; then
        verify_ddctrl_state "$label" "1" "$pattern"
    elif [ "$FINGER" = "1" ] || [ "$FINGER" = "y" ]; then
        # Automatically generate a unique, self-documenting label on-the-fly!
        local safe_pattern=$(echo "$pattern" | tr -c 'a-zA-Z0-9_' '_' | tr -s '_' | sed 's/^_//;s/_$//')
        local safe_flags=$(echo "$expected_flags" | tr -c 'a-zA-Z0-9_' '_' | tr -s '_' | sed 's/^_//;s/_$//')
        local auto_label="auto_${safe_pattern}_flags_${safe_flags}_line${BASH_LINENO[0]}"
        verify_ddctrl_state "$auto_label" "1" "$pattern"
    fi
}

# ==============================================================================
# TESTING STRATEGY 2.
#   do 1 to setup test expectations.
#   run logging-workload
#   capture output
#   hash-validate it against GOLDEN_SAMPLE db (at file end)
#  
# ==============================================================================
# Source hash-based validation and state verification helper library
DIR="$(dirname "$(readlink -f "$0")")"
. "$DIR/syslog_hash_validation.sh"

# Define target validation file path
CONTROL_FILE="/proc/dynamic_debug/control"

# App-specific wrappers mapping to generic library helpers
function slice_and_hash_ddctrl {
    slice_and_hash_by_grep "$1" "$CONTROL_FILE"
}

function verify_ddctrl_state {
    # Map old client signature (label, extra_args, pattern) to generic verify_grep_state (label, pattern, file, extra_args)
    local label="$1"
    local extra_args="$2"
    local pattern="$3"
    verify_grep_state "$label" "$pattern" "$CONTROL_FILE" "$extra_args"
}

# ==============================================================================
function ifrmmod {
    lsmod | grep "$1" >/dev/null 2>&1 && rmmod $1
}

# ==============================================================================
# DYNAMIC-DEBUG BEHAVIOR TESTS.
# when these start doing response/content validation tests (STRATEGY
# 2), caller line numbers will matter for the GOLDEN_SAMPLES.  We may
# be able to improve this later.

# ==============================================================================
# FEATURE TESTS (FT_*)
# These functions define the specifications and behavioral compliance assertions
# of the dynamic-debug core query parser, classmaps, and parameter interfaces.
# ==============================================================================
function FT_basic_queries {
    echo -e "${GREEN}# BASIC_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	exit $ksft_skip
    fi
    ddcmd =_ # zero everything
    count_pr_debugs '*' '=p' 0

    # module params are builtin to handle boot args
    count_pr_debugs '\[kernel/params\]' '=_' 4
    ddcmd module params +mpf
    count_pr_debugs '\[kernel/params\]' '=pmf' 4 "FT_basic_queries_params_mpf"

    # multi-cmd input, newline separated, with embedded comments
    cat <<"EOF" > /proc/dynamic_debug/control
      module params =_				# clear params
      module params +mf				# set flags
      module params func parse_args +sl		# other flags
EOF
    count_pr_debugs '\[kernel/params\]' '=mf' 3
    count_pr_debugs '\[kernel/params\]' '=mfsl' 1 "FT_basic_queries_params_multicmd"

    ddcmd =_
}

function FT_path_module_queries {
    echo -e "${GREEN}# TEST_PATH_MODULE_QUERIES ${NC}"
    ddcmd =_

    # Find how many 'main' modules we have in total (by basename)
    # Use a precise OR pattern to match exactly [main] or [*/main] and avoid irqdomain
    local total_main=$(grep -c "\[main\]\|\[[^]]*/main\]" /proc/dynamic_debug/control)
    echo "# found $total_main total 'main' modules"

    if [ $total_main -eq 0 ]; then
        echo "SKIP - no 'main' modules found to test slashes"
        return
    fi

    echo "# testing 'module */main'"
    ddcmd module "*/main" +p
    # This should match modules that HAVE a slash and end in /main
    local slash_main=$(grep -c "\[[^]]*/main\]" /proc/dynamic_debug/control)
    count_pr_debugs "\[[^]]*/main\]" '=p' $slash_main
    local hash_star_main=$(slice_and_hash_ddctrl "\[main\]\|\[[^]]*/main\]")

    echo "# testing 'module init/main' (specific path)"
    ddcmd =_
    ddcmd module "init/main" +p
    local init_main=$(grep -c "\[init/main\]" /proc/dynamic_debug/control)
    count_pr_debugs "\[init/main\]" '=p' $init_main "FT_FT_path_module_queries_init_main"

    echo "# testing 'module main' (basename match)"
    ddcmd =_
    ddcmd module main +p
    # This should match ALL $total_main entries due to kbasename matching
    count_pr_debugs "\[main\]\|\[[^]]*/main\]" '=p' $total_main
    local hash_basename_main=$(slice_and_hash_ddctrl "\[main\]\|\[[^]]*/main\]")

    # Real-time mathematical proof of kbasename matching equivalence to wildcard matching!
    if [ "$hash_star_main" != "$hash_basename_main" ]; then
        echo -e "${RED}: kbasename matching check failed! Fingerprints do not match wildcard matching.${NC}"
        exit $ksft_fail
    else
        echo -e "${GREEN}: Proven: kbasename matching is equivalent to wildcard path matching!${NC}"
    fi
}

function FT_hyphen_underscore {
    echo -e "${GREEN}# TEST_HYPHEN_UNDERSCORE ${NC}"
    ddcmd =_

    # Find a module with a hyphen in its name (e.g., from the control file)
    local mod_with_hyphen=$(grep -m1 "\[[^]]*-[^]]*\]" /proc/dynamic_debug/control | sed -n 's/.*\[\(.*\)\].*/\1/p')

    if [ -z "$mod_with_hyphen" ]; then
        echo "SKIP - no module with hyphen found in /proc/dynamic_debug/control"
        return
    fi

    echo "# testing hyphen/underscore equivalence for module: $mod_with_hyphen"
    local mod_with_underscore=$(echo "$mod_with_hyphen" | tr '-' '_')
    local base_hyphen=$(basename "$mod_with_hyphen")
    local slice_pattern="\[[^]]*$base_hyphen\]"

    # 1. Enable using literal hyphen name, and record the state fingerprint
    echo "#   trying hyphen name: $mod_with_hyphen"
    ddcmd module "$mod_with_hyphen" +p
    local count_hyphen=$(grep -c "\[$mod_with_hyphen\]" /proc/dynamic_debug/control)
    count_pr_debugs "$slice_pattern" "=p" $count_hyphen
    local hash_hyphen=$(slice_and_hash_ddctrl "$slice_pattern")

    # 2. Disable and enable using underscore name, and record the state fingerprint
    ddcmd =_
    echo "#   trying underscore name: $mod_with_underscore"
    ddcmd module "$mod_with_underscore" +p
    count_pr_debugs "$slice_pattern" "=p" $count_hyphen
    local hash_underscore=$(slice_and_hash_ddctrl "$slice_pattern")

    # Real-time mathematical proof of hyphen/underscore name equivalence!
    if [ "$hash_hyphen" != "$hash_underscore" ]; then
        echo -e "${RED}: Hyphen/Underscore equivalence check failed! Fingerprints do not match.${NC}"
        echo -e "Hyphen name state hash:     $hash_hyphen"
        echo -e "Underscore name state hash: $hash_underscore"
        exit $ksft_fail
    else
        echo -e "${GREEN}: Proven: Hyphen/Underscore literal name equivalence matches!${NC}"
    fi

    # 3. Try kbasename with hyphen (if it has a path)
    if [ "$base_hyphen" != "$mod_with_hyphen" ]; then
        ddcmd =_
        echo "#   trying hyphen kbasename: $base_hyphen"
        ddcmd module "$base_hyphen" +pmf
        local count_base=$(grep -c "\[[^]]*$base_hyphen\]" /proc/dynamic_debug/control)
        count_pr_debugs "$slice_pattern" "=pmf" $count_base
        local hash_base_hyphen=$(slice_and_hash_ddctrl "$slice_pattern")

        # Prove kbasename hyphen name matches literal path hyphen name (with different flags)!
        echo "#   trying full path hyphen with pmf flags"
        ddcmd =_
        ddcmd module "$mod_with_hyphen" +pmf
        local hash_path_pmf=$(slice_and_hash_ddctrl "$slice_pattern")
        if [ "$hash_path_pmf" != "$hash_base_hyphen" ]; then
            echo -e "${RED}: Hyphen kbasename check failed! Fingerprints do not match full-path hyphen enablement.${NC}"
            exit $ksft_fail
        else
            echo -e "${GREEN}: Proven: Hyphen kbasename matches full-path hyphen enablement!${NC}"
        fi
    fi

    # 4. Try kbasename with underscore
    local base_underscore=$(echo "$base_hyphen" | tr '-' '_')
    ddcmd =_
    echo "#   trying underscore kbasename: $base_underscore"
    ddcmd module "$base_underscore" +pmf
    local count_base=$(grep -c "\[[^]]*$base_hyphen\]" /proc/dynamic_debug/control)
    count_pr_debugs "$slice_pattern" "=pmf" $count_base
    local hash_base_underscore=$(slice_and_hash_ddctrl "$slice_pattern")

    # Real-time mathematical proof of hyphen/underscore kbasename equivalence!
    if [ "$hash_base_hyphen" != "$hash_base_underscore" ] && [ -n "$hash_base_hyphen" ]; then
        echo -e "${RED}: Hyphen/Underscore kbasename equivalence check failed! Fingerprints do not match.${NC}"
        exit $ksft_fail
    elif [ -n "$hash_base_hyphen" ]; then
        echo -e "${GREEN}: Proven: Hyphen/Underscore kbasename equivalence matches!${NC}"
    fi

    ddcmd =_
}

# test parsing on spaces, commas. testing agains builtin [kernel/params]
function FT_comma_terminators {
    echo -e "${GREEN}# COMMA_TERMINATOR_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	return
    fi
    count_pr_debugs '\[kernel/params\]' '=_' 4

    ddcmd module,params,=_		# commas as spaces
    ddcmd module,params,+mpf		# turn on module's pr-debugs
    count_pr_debugs '\[kernel/params\]' '=pmf' 4

    # 1. Verify exact control file state after commas-as-spaces query
    verify_ddctrl_state "FT_comma_terminators_commas_as_spaces" "1" "kernel/params"

    # empty tokens in list are ignored
    ddcmd ,module ,, ,  params, -p
    count_pr_debugs '\[kernel/params\]' '=mf' 4

    # 2. Verify exact control file state after ignored-commas query
    verify_ddctrl_state "FT_comma_terminators_ignored_commas" "1" "kernel/params"

    ddcmd " , module ,,, ,  params, -m"
    count_pr_debugs '\[kernel/params\]' '=f' 4

    # 3. Verify exact control file state after quoted-commas query
    verify_ddctrl_state "FT_comma_terminators_quoted_commas" "1" "kernel/params"

    ddcmd =_
}

# more testing of multi-query command handling (newlines done in FT_basic_queries)
function FT_multiquery_splitting {
    echo -e "${GREEN}# TEST_MULTIQUERY_SPLITTING - multi-command splitting on @ ${NC}"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_
    modprobe test_dynamic_debug dyndbg=class,D2_CORE,+pf@class,D2_KMS,+ps@class,D2_ATOMIC,+pm
    count_pr_debugs '\[test_dynamic_debug\]' '=pf' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=ps' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=pm' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=_' 31 "FT_multiquery_splitting_ctrl_initial"
    # add flags to those callsites
    ddcmd class,D2_CORE,+mf@class,D2_KMS,+ls@class,D2_ATOMIC,+ml
    count_pr_debugs '\[test_dynamic_debug\]' '=pmf' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=psl' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=pml' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=_' 31 "FT_multiquery_splitting_ctrl_updated"

    # --- Live Content Fingerprinting Phase ---
    local label="FT_multiquery_splitting_prints_${BASH_LINENO[0]}"
    echo "DYNDBG_START_${label}" > /dev/kmsg
    echo 1 > /sys/module/test_dynamic_debug/parameters/do_bulk
    echo "DYNDBG_END_${label}" > /dev/kmsg
    local hash=$(slice_and_hash_dmesg "$label")
    verify_dmesg_fingerprint "$label" "1" "$hash"

    ifrmmod test_dynamic_debug
}

function FT_classmap_inheritance {
    echo -e "${GREEN}# TEST_MOD_SUBMOD ${NC}"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_

    # modprobe with class enablements
    modprobe test_dynamic_debug \
	dyndbg=class,D2_CORE,+pf@class,D2_KMS,+pt@class,D2_ATOMIC,+pm

    count_pr_debugs '\[test_dynamic_debug\]' '=_' 31 "FT_classmap_inheritance_ctrl_initial"
    count_pr_debugs '\[test_dynamic_debug\]' '=pf' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=pt' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=pm' 1

    modprobe test_dynamic_debug_submod
    count_pr_debugs '\[test_dynamic_debug_submod\]' '=_' 34 "FT_classmap_inheritance_sub_initial"
    count_pr_debugs '\[test_dynamic_debug\]' '=_' 31
    count_pr_debugs 'test_dynamic_debug' '=_' 65

    # no enablements propagate here
    count_pr_debugs '\[test_dynamic_debug\]' '=pf' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=pt' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=pm' 1

    # change classes again, this time submod too
    ddcmd class,D2_CORE,+mf@class,D2_KMS,+lt@class,D2_ATOMIC,+ml "# add some prefixes"
    count_pr_debugs '\[test_dynamic_debug\]' '=pmf' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=plt' 1
    count_pr_debugs '\[test_dynamic_debug\]' '=pml' 1
    #  submod changed too
    count_pr_debugs '\[test_dynamic_debug_submod\]' '=mf' 1
    count_pr_debugs '\[test_dynamic_debug_submod\]' '=lt' 1
    count_pr_debugs '\[test_dynamic_debug_submod\]' '=ml' 1
    count_pr_debugs 'test_dynamic_debug' '=_' 62 "FT_classmap_inheritance_ctrl_updated"

    # now work the classmap-params
    # fresh start, to clear all above flags (test-fn limits)
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    modprobe test_dynamic_debug_submod # get supermod too

    echo 1 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 4 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( V1-3 + D2_CORE )
    count_pr_debugs 'test_dynamic_debug' '=p' 8
    echo 3 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 0 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( D2_CORE, D2_DRIVER )
    count_pr_debugs 'test_dynamic_debug' '=p' 4
    echo 0x16 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 0 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( D2_DRIVER, D2_KMS, D2_ATOMIC )
    count_pr_debugs 'test_dynamic_debug' '=p' 6

    # recap DRM_USE_DYNAMIC_DEBUG regression
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    # set super-mod params
    modprobe test_dynamic_debug p_disjoint_bits=0x16 p_level_num=5
    count_pr_debugs '\[test_dynamic_debug\]' '=p' 7
    modprobe test_dynamic_debug_submod
    # see them picked up by submod
    count_pr_debugs 'test_dynamic_debug' '=p' 14

    # --- Live Content Fingerprinting Phase ---
    local label="FT_classmap_inheritance_regression_prints_${BASH_LINENO[0]}"
    echo "DYNDBG_START_${label}" > /dev/kmsg
    echo 1 > /sys/module/test_dynamic_debug/parameters/do_classes
    echo 1 > /sys/module/test_dynamic_debug_submod/parameters/do_classes
    echo "DYNDBG_END_${label}" > /dev/kmsg
    local hash=$(slice_and_hash_dmesg "$label")
    verify_dmesg_fingerprint "$label" "1" "$hash"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
}

function FT_modprobe_parameter_transitions {
    echo -e "${GREEN}# TEST_MODPROBES ${NC}"
    ddcmd =_
    local verbose
    
    # Enable logging on the builtin kernel parameter parsing engine
    ddcmd "file kernel/params.c +p"

    for verbose in 1 2 3 4 0; do
	echo $verbose > /sys/module/dynamic_debug/parameters/verbose

	# Verify each parameter load sequence with 100% DRY modularity
	verify_modprobe_param_logging "do_classes" "1" "classes_verb${verbose}"
	verify_modprobe_param_logging "do_bulk" "1" "bulk_verb${verbose}"

	# Sequence composite bitmasks to verify disjoint bit transitions
	for mask in "0x05" "0x12" "0x1f" "0x00"; do
            verify_modprobe_param_logging "p_disjoint_bits" "$mask" \
					  "disjoint_${mask}_verb${verbose}"
	done

	# Sequence levels to verify both growing and shrinking verbose transitions
	for lvl in "3" "5" "4" "0"; do
            verify_modprobe_param_logging "p_level_num" "$lvl" \
					  "level_${lvl}_verb${verbose}"
	done
    done # verbose loop
    ddcmd =_
}

function FT_ratelimiting {
    echo -e "${GREEN}# TEST_RATELIMITING - solo and shared rate-limiting tests on unclassed do_bulk sites ${NC}"
    if [ $LACK_TMOD -eq 1 ]; then
        echo "SKIP - test requires test-dynamic-debug.ko"
        return
    fi
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_

    modprobe test_dynamic_debug

    # 1. Enable standard printing and verify unsuppressed log content (5 repeats)
    ddcmd file test_dynamic_debug.c func do_bulk +p
    count_pr_debugs "bulk msg" "=p" 10
    do_logging_and_verify 5 "normal"

    # 2. Test SOLO rate-limiting (+r) unsuppressed (3 repeats < solo_burst=10)
    # Under solo (+r), each of the 10 callsites gets its own independent 10-burst budget.
    # Triggering 3 repeats is under the 10-burst limit per-site, so all 30 prints pass unsuppressed!
    ddcmd file test_dynamic_debug.c func do_bulk +r
    count_pr_debugs "bulk msg" "=pr" 10
    do_logging_and_verify 3 "solo_unsuppressed"

    # 3. Test SOLO rate-limiting (+r) suppression (12 repeats > solo_burst=10)
    # Triggering 12 repeats (total 120 executions). Since we already used 3 prints in step 2,
    # each site has exactly 7 left in its independent 10-burst budget.
    # Each site prints exactly 7 times and suppresses the remaining 5, yielding exactly 70 total lines!
    do_logging_and_verify 12 "solo_suppressed"

    # 4. Test SHARED rate-limiting (+R) suppression (8 repeats > shared_burst=50)
    # Under shared (+R), the entire group of 10 callsites shares a single 50-burst budget.
    # Triggering 8 repeats runs the group 80 times, which exceeds the shared 50-burst budget.
    # It allows exactly 50 total prints group-wide, suppressing the other 30, yielding exactly 50 total lines!
    ddcmd file test_dynamic_debug.c func do_bulk -r
    ddcmd file test_dynamic_debug.c func do_bulk +R
    count_pr_debugs "bulk msg" "=pR" 10
    do_logging_and_verify 8 "shared_suppressed"

    # 5. Test Overriding/Shadowing (+R and +r co-existing)
    # Override a single site (bulk msg 1 on line 226) with an individual solo limit
    ddcmd file test_dynamic_debug.c line 226 +r
    # "bulk msg 1" should now show both flags active (prR)
    count_pr_debugs "bulk msg" "=prR" 1
    # The remaining 9 sites should still have only 'pR'
    count_pr_debugs "bulk msg" "=pR" 9

    # 6. Fallback behavior (removing solo override puts it back into the shared group)
    ddcmd file test_dynamic_debug.c line 226 -r
    count_pr_debugs "bulk msg" "=pR" 10

    ifrmmod test_dynamic_debug
}

# Built-in Feature Tests (Can run on any CONFIG_DYNAMIC_DEBUG kernel, modular or monolithic)
builtin_tests=(
    FT_basic_queries
    FT_path_module_queries
    FT_hyphen_underscore
    FT_comma_terminators
)

# Modular Feature Tests (Require CONFIG_MODULES=y and test_dynamic_debug*.ko available)
modular_tests=(
    FT_multiquery_splitting
    FT_classmap_inheritance
    FT_modprobe_parameter_transitions
    FT_ratelimiting
)

# ==============================================================================
# GOLDEN_RECORDS (MD5 Fingerprint Verification Database)
#
# This database stores the expected invariant log content hashes for our tests.
# Since the key has the line-number of the callsite, we dont yet
# support looping over a test-call, maybe we'll need to address that
# later.
#
# NB: records have lineno of the test in code above. table at bottom
# means inserts dont shift test-lines.
#
# ==============================================================================
function GOLDEN_RECORDS {
    cat << 'EOF'
#K: <md5_hash>                       <label>               <args>
#K= 57e5fc9552bd6f0ff6ff18101df74565 FT_basic_queries_params_mpf   control:"\\[kernel/params\\]"
#K= 12aab629bdd655c99970dcb397c33402 FT_basic_queries_params_multicmd control:"\\[kernel/params\\]"
#K= 0c34da74906e3bd3bb482b7b7553a153 FT_FT_path_module_queries_init_main 1
#K= 57e5fc9552bd6f0ff6ff18101df74565 FT_comma_terminators_commas_as_spaces control:"kernel/params"
#K= f70473f6896b9f4f13a785f2234177a7 FT_comma_terminators_ignored_commas control:"kernel/params"
#K= d5961d67b122d14ce67559c71564efd1 FT_comma_terminators_quoted_commas control:"kernel/params"
#K= bd9aa64f2a576145e76b7545512bea73 FT_multiquery_splitting_ctrl_initial control:"\\[test_dynamic_debug\\]"
#K= 3cbd8ca2cb8ae863f91b9e1e2523c9e6 FT_multiquery_splitting_ctrl_updated control:"\\[test_dynamic_debug\\]"
#K= 68b329da9893e34099c7d8ad5cb9c940 FT_multiquery_splitting_prints_819 dmesg:1
#K= adad029f83d694b7d98b227af9fc800a FT_classmap_inheritance_ctrl_initial  control:"\\[test_dynamic_debug\\]"
#K= dffe8d7c914804645e0c71252a2b518a FT_classmap_inheritance_sub_initial   control:"\\[test_dynamic_debug_submod\\]"
#K= 390594542cc1fd3e16b6a4a3c9f5922c FT_classmap_inheritance_ctrl_updated  control:"test_dynamic_debug"
#K= 29d6c3d962400aa663d01a1ca665bc23 FT_classmap_inheritance_regression_prints_872 dmesg:1

# --- MULTI-DIMENSIONAL VERBOSITY AND STATE TRANSITIONS DATABASE (50 CHECKPOINTS) ---
# Verbosity = 0 (Pruned Silence)
#K= 18f050243a98fee40ead8cd0f0b4a092 modprobe_classes_verb0_line691_seq41 dmesg:1
#K= f5d6d4626f3744514db7f44db10f62f1 modprobe_bulk_verb0_line692_seq42 dmesg:1
#K= 3ca925e6bded5cc9cd91ab839be6ef89 modprobe_disjoint_0x05_verb0_line696_seq43 dmesg:1
#K= e02701932d7bcc8256b615fd9230e0a3 modprobe_disjoint_0x12_verb0_line696_seq44 dmesg:1
#K= bb5db0f2facb9f8641aff2688d1378df modprobe_disjoint_0x1f_verb0_line696_seq45 dmesg:1
#K= f75acea70622ac838300a4160a070f58 modprobe_disjoint_0x00_verb0_line696_seq46 dmesg:1
#K= f2056de0523055068adbd82c5a5362ac modprobe_level_3_verb0_line702_seq47 dmesg:1
#K= 8df4c82af2bf5ee0ae55df15fe479a51 modprobe_level_5_verb0_line702_seq48 dmesg:1
#K= 386199938d3608a49460556ec31ee755 modprobe_level_4_verb0_line702_seq49 dmesg:1
#K= 0c60b0b77f537e308c4aa86121388696 modprobe_level_0_verb0_line702_seq50 dmesg:1

# Verbosity = 1 (Basic Verbose)
#K= f30b35b60103fbe0c85a2a9752e6fd77 modprobe_classes_verb1_line691_seq1 dmesg:1
#K= 6cffa5fe4ca2a8cfd4ddf79925ab9c7e modprobe_bulk_verb1_line692_seq2 dmesg:1
#K= 7d970411668a1403a03f58e1b3b21e35 modprobe_disjoint_0x05_verb1_line696_seq3 dmesg:1
#K= 5d4c9031d9d85d11ccd43e88a189bec0 modprobe_disjoint_0x12_verb1_line696_seq4 dmesg:1
#K= a3d093f5b99ab0f87504359ab981bd26 modprobe_disjoint_0x1f_verb1_line696_seq5 dmesg:1
#K= 5f40b6f69acca4019e1b7914ca3785db modprobe_disjoint_0x00_verb1_line696_seq6 dmesg:1
#K= 4b96d415bd3c9b7f34079a6c8ace98a8 modprobe_level_3_verb1_line702_seq7 dmesg:1
#K= 45ac4a3203ec9aa87fc5cb7df8239a58 modprobe_level_5_verb1_line702_seq8 dmesg:1
#K= 4622dc4d2bd7539f3110abade501f40f modprobe_level_4_verb1_line702_seq9 dmesg:1
#K= cb7540a964856e830d514cc3f80cb89b modprobe_level_0_verb1_line702_seq10 dmesg:1

# Verbosity = 2 (High Verbose)
#K= f909855177e883a881395683f79be894 modprobe_classes_verb2_line691_seq11 dmesg:1
#K= 28f07d1490b058d0d0a924d04639b993 modprobe_bulk_verb2_line692_seq12 dmesg:1
#K= fe4e677264349ce232ab6fa98e188464 modprobe_disjoint_0x05_verb2_line696_seq13 dmesg:1
#K= 58fac3c5c9a955dabca609f65a424da8 modprobe_disjoint_0x12_verb2_line696_seq14 dmesg:1
#K= abb4eddc361e2a40904b42f904720368 modprobe_disjoint_0x1f_verb2_line696_seq15 dmesg:1
#K= 5797ced615a26bc508c247ff4b9b03fd modprobe_disjoint_0x00_verb2_line696_seq16 dmesg:1
#K= 20b016ecce50138343662ad19c2b154c modprobe_level_3_verb2_line702_seq17 dmesg:1
#K= d0507ffb1a46f467594b89a8fa6c47e9 modprobe_level_5_verb2_line702_seq18 dmesg:1
#K= befff040cee395becc4df3ac09ec2c3b modprobe_level_4_verb2_line702_seq19 dmesg:1
#K= 860b11fe198ec2a1ff5d09f438dd18b9 modprobe_level_0_verb2_line702_seq20 dmesg:1

# Verbosity = 3 (Very High Verbose)
#K= ea818358520568496b139670b8603d13 modprobe_classes_verb3_line691_seq21 dmesg:1
#K= 3c6e0285f3f1af0a6e96225eccc8066f modprobe_bulk_verb3_line692_seq22 dmesg:1
#K= 582859db904e2beef8112ff837a7e101 modprobe_disjoint_0x05_verb3_line483_seq23 dmesg:1
#K= b6b2eb815a1fb234337bc014f5ec84c8 modprobe_disjoint_0x12_verb3_line483_seq24 dmesg:1
#K= 649597abac7264116138f66894e16bf6 modprobe_disjoint_0x1f_verb3_line483_seq25 dmesg:1
#K= 784b411a5aaa10c03046c4777260d446 modprobe_disjoint_0x00_verb3_line696_seq26 dmesg:1
#K= 7e00e12459bb6c0db02c7fd887441665 modprobe_level_3_verb3_line489_seq27 dmesg:1
#K= 414546509bdfd883c47a312f719786dc modprobe_level_5_verb3_line489_seq28 dmesg:1
#K= 0994e15193674badf07a4e995b422a08 modprobe_level_4_verb3_line489_seq29 dmesg:1
#K= c97b46246763073b635fdbb3afeafcd4 modprobe_level_0_verb3_line702_seq30 dmesg:1

# Verbosity = 4 (Extremely High Verbose)
#K= ea818358520568496b139670b8603d13 modprobe_classes_verb4_line691_seq31 dmesg:1
#K= 3c6e0285f3f1af0a6e96225eccc8066f modprobe_bulk_verb4_line692_seq32 dmesg:1
#K= e6c0f53b0cefec7e55487739987cd7bf modprobe_disjoint_0x05_verb4_line483_seq33 dmesg:1
#K= 1e1a4aac01056da687107723b16987e0 modprobe_disjoint_0x12_verb4_line483_seq34 dmesg:1
#K= a8a8b6d2a93ddd65fd34d4b7792bed73 modprobe_disjoint_0x1f_verb4_line483_seq35 dmesg:1
#K= 784b411a5aaa10c03046c4777260d446 modprobe_disjoint_0x00_verb4_line696_seq36 dmesg:1
#K= e775d94a0655164782f7f80ce0e230e0 modprobe_level_3_verb4_line489_seq37 dmesg:1
#K= a02aee9b00e9ecf66d4f99b9227c4078 modprobe_level_5_verb4_line489_seq38 dmesg:1
#K= 1ef7be6b4f74969b9a325948c90fc8bf modprobe_level_4_verb4_line489_seq39 dmesg:1
#K= c97b46246763073b635fdbb3afeafcd4 modprobe_level_0_verb4_line702_seq40 dmesg:1
EOF
}

# ==============================================================================
# Utilites, test primitives

# ==============================================================================
# Run tests

# Clear any stale seen/new hashes from previous runs
rm -f "$SEEN_HASHES_FILE" "$NEW_HASHES_FILE"

ifrmmod test_dynamic_debug

# Check if loadable module support or our test modules are missing
LACK_TMOD=0
modprobe -q -n test_dynamic_debug || LACK_TMOD=1

# 1. Run all Built-in Feature Tests
echo -e "${GREEN}# RUNNING BUILT-IN FEATURE TESTS ${NC}"
for test_func in "${builtin_tests[@]}"; do
    $test_func
    echo ""
done

# 2. Run Modular Feature Tests only if test modules are available
if [ $LACK_TMOD -eq 0 ]; then
    echo -e "${GREEN}# RUNNING MODULAR FEATURE TESTS ${NC}"
    for test_func in "${modular_tests[@]}"; do
        $test_func
        echo ""
    done
else
    echo -e "${YELLOW}# SKIPPING MODULAR TESTS: test_dynamic_debug.ko not available ${NC}"
fi
echo -en "${GREEN}# Done on: "
date
echo -en "${NC}"

audit_golden_records

# Output consolidated block of mismatched or new fingerprints at the absolute end of the run
if [ -s "$NEW_HASHES_FILE" ]; then
    echo -e "${RED}\n=============================================================================="
    echo -e "THE FOLLOWING FINGERPRINTS MISMATCHED OR NEED TO BE ADDED TO GOLDEN_RECORDS():"
    echo -e "=============================================================================="
    cat "$NEW_HASHES_FILE"
    echo -e "==============================================================================${NC}"
    rm -f "$NEW_HASHES_FILE"
    exit $ksft_fail
fi

exit $ksft_pass

