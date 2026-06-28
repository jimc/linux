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
# Environment Controls:
#   V=0,1,2 : Verbosity (0=concise summary, 1=verified assertions, 2=full captured outputs)
#   K=0     : Strict mode (fails with exit 1 on checksum drift or stale records)
#   K=1     : Soft-pass mode (prints DRIFT/STALE diffs, exits 0 with 'fake success')
#   K=2     : Silent soft-pass mode (suppresses DRIFT/STALE diffs, exits 0 with 'fake success')
V=${V:=0}
K=${K:=0}

# Sanitize V and K to ensure they are valid integers
if [[ ! "$V" =~ ^[0-9]+$ ]]; then
    V=0
fi
if [[ ! "$K" =~ ^[0-9]+$ ]]; then
    K=0
fi

function v_echo {
    [ "${V:-0}" -ge 1 ] && echo -e "$@"
}

[ -e /proc/dynamic_debug/control ] || {
    echo -e "${RED}: this test requires CONFIG_DYNAMIC_DEBUG=y ${NC}"
    exit $ksft_skip # nothing to test here, no good reason to fail.
}

lsmod >/dev/null 2>&1 || {
    echo -e "${RED}: lsmod requires /proc/modules ${NC}"
    # exit $ksft_skip # maybe later we can do more
}

# need info to avoid failures due to untestable configs

[ -f "$KCONFIG_CONFIG" ] || KCONFIG_CONFIG=".config"
if [ -f "$KCONFIG_CONFIG" ]; then
    v_echo "# consulting KCONFIG_CONFIG: $KCONFIG_CONFIG"
    grep -q "CONFIG_DYNAMIC_DEBUG=y" $KCONFIG_CONFIG ; LACK_DD_BUILTIN=$?
    grep -q "CONFIG_TEST_DYNAMIC_DEBUG=m" $KCONFIG_CONFIG ; LACK_TMOD=$?
else
    # if no config, try runtime probes
    modprobe -n test_dynamic_debug 2>/dev/null ; LACK_TMOD=$?
    # assume builtin dyndbg if control exists (checked above)
    LACK_DD_BUILTIN=0
fi

function ifrmmod {
    [ "${LACK_TMOD:-0}" -eq 1 ] && return
    grep -q "^$1 " /proc/modules 2>/dev/null && rmmod $1
}

# Clean up any leftover loaded test modules at initialization
ifrmmod test_dynamic_debug_submod
ifrmmod test_dynamic_debug

# ===========================================================================
# TESTING STRATEGY 1.
#   Change and observe control-file settings:
#     ddcmd: ie echo $dd_query_cmd > /proc/dynamic_debug/control
#     read back control, count changes due to query_cmd
# ===========================================================================
DDCMD_LOG=""	# accumulate

function log_ddcmd {
    local cmd="$1"
    if [ "${IN_BOOKEND:-0}" -eq 1 ] && [ -n "$DDCMD_LOG" ]; then
        DDCMD_LOG="${DDCMD_LOG}; $cmd"
    else
        DDCMD_LOG="$cmd"
    fi
}

function my_modprobe {
    log_ddcmd "modprobe $*"
    modprobe "$@"
}

function set_param {
    local val="$1"
    local path="$2"
    log_ddcmd "echo $val > $path"
    echo "$val" > "$path"
}

function ddcmd () {
    # ddcmd <query_args> [range_pattern] [pass|fail|log]
    local args="$1"
    local range="$2"
    local action="${3:-pass}"
    local exp_exit=0

    [ "$action" = "fail" ] && exp_exit=1
    log_ddcmd "$args"

    # Update cumulative state-machine lineage
    if [[ "$args" == *"=_"* ]]; then
        CUMULATIVE_DDCMDS="$args"
    else
        CUMULATIVE_DDCMDS="${CUMULATIVE_DDCMDS}; $args"
    fi

    [ "$action" != "pass" ] && log_start
    [ -n "$range" ] && capture_before "$range"

    output=$( (echo "$args" > /proc/dynamic_debug/control) 2>&1 )
    handle_exit_code $BASH_LINENO $FUNCNAME $? $exp_exit

    [ "$action" != "pass" ] && log_stop
    [ -n "$range" ] && verify_after_change
}

function ddcmd_err () {
    # ddcmd_err <query_args>
    # Semantic wrapper for parser syntax & error validation
    ddcmd "$1" "" fail
}

function ddcmd_load () {
    # ddcmd_load <query_args> <range_pattern> <workload_param_path> <workload_val>
    # Semantic wrapper for end-to-end filter setup and live workload logging
    local query="$1"
    local range="$2"
    local param_path="$3"
    local val="$4"

    # 1. Setup the control filters (using positional ddcmd range-check)
    echo  "$query" "$range"
    ddcmd "$query" "$range"

    # 2. Execute the workload and capture syslog prints
    log_start
    echo "$val" > "$param_path"
    log_stop
}

function handle_exit_code() {
    local exp_exit_code=0
    [ $# == 4 ] && exp_exit_code=$4
    if [ "$3" -ne $exp_exit_code ]; then
        echo -e "${RED}: $BASH_SOURCE:$1 $2() " \
            "expected to exit with code $exp_exit_code, got $3${NC}"
	[ "$3" == 1 ] && echo "Error: '$output'"
        exit $ksft_fail
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
function verify_control_slice {
    # $1 - pattern to slice
    # $2 - optional extra args
    verify_file_slice "$1" $CONTROL_FILE "$2"
}

function slice_and_hash_ddctrl {
    local slice=$(slice_by_grep "$1" "$CONTROL_FILE" | strip_control_linenos)
    echo "$slice" | tr -d '\r' | md5sum | cut -d' ' -f1
}

# ==============================================================================

function verify_modprobe_param_logging {
    # $1 - parameter name (e.g. do_classes)
    # $2 - parameter value (e.g. 1)
    local param="$1"
    local val="$2"

    # Make sure both modules are completely unloaded to trigger a fresh load
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    # Capture and verify the load-time (modprobe) dmesg logs
    log_start
    my_modprobe test_dynamic_debug "${param}=${val}"
    my_modprobe test_dynamic_debug_submod

    # If it is a state-controlling parameter, trigger the
    # print-workload 'do_prints=1' inside the same syslog dmesg
    # capture bookends to verify their actual pr_debug logging!

    if [ "$param" = "p_disjoint_bits" ] || [ "$param" = "p_level_num" ]; then
        set_param 1 /sys/module/test_dynamic_debug/parameters/do_prints
    fi

    log_stop

    # Verify param write by direct readback
    if [ "$param" = "p_disjoint_bits" ] || [ "$param" = "p_level_num" ]; then
        local readback=$(cat "/sys/module/test_dynamic_debug/parameters/${param}")
        if (( readback != val )); then
            echo -e "${RED}: param readback failed: ${param} ${val} != ${readback}${NC}"
            exit $ksft_fail
        else
            [ "$V" -ge 1 ] && \
		echo -e "${GREEN}✔ Parameter Readback Verified: ${param}=${readback}${NC}"
        fi
    fi

    # verify runtime unsetting
    if [ "$param" = "p_disjoint_bits" ] || [ "$param" = "p_level_num" ]; then

        set_param 0 "/sys/module/test_dynamic_debug/parameters/${param}"
	verify_control_slice '\[test_dynamic_debug\]'

    fi
}

# ==============================================================================
# FEATURE TESTS (FT_*)
#
# test legal queries which should execute and return 0 (success)
# so we dont look for errors in dmesg
function FT_grammar_ok {
    v_echo "${GREEN}# GRAMMAR_OK_TESTS ${NC}"
    ddcmd "+_"
    ddcmd "-_"

    # use 4 keywords (max 9 words inc flags)
    ddcmd "module foo file bar.c func buz class D2_CORE +_"	# 4 keywords
    #ddcmd "module foo file bar.c func buz class D2 line 100 +_" # 5 keywords

    # 3. Dedicated lineno range grammar assertions (side-effect-free proofs)
    ddcmd "line 42 +_"		# test exact line syntax
    ddcmd "line 10- +_"		# test open-ended line range (starting at 10)
    ddcmd "line -100 +_"	# test open-ended line range (ending at 100)
    ddcmd "line 10-100 +_"	# test closed-interval line range

    # 4. Dedicated colon-delimited file:line and file:func assertions
    ddcmd "file a_file.c:1-100 +_"	# test file:linerange syntax
    ddcmd "file b_file.c:30 +_"		# test file:exact_line syntax
    ddcmd "file c_file.c:c_func +_"	# test file:function_name syntax
    ddcmd "file c_file.c:start_* +_"	# test file:wildcard_function syntax

    # 5. Advanced formatting and separator checks (side-effect-free proofs)
    ddcmd "format \"space\\040here\" +_"	# test format query with octal escape
    #ddcmd "module,foo +_"		# test comma token separator syntax
    ddcmd "func *my_func* +_"		# test wildcard func syntax
    ddcmd "file drivers/usb/* +_"	# test wildcard file path syntax
}

# test grammar, no actual sites chosen/changed
# use dyndbg's embedded comments in queries
function FT_grammar_errs {
    v_echo "${GREEN}# GRAMMAR_ERROR_TESTS ${NC}"
    ddcmd =_
    local verbose

    # Reset before loop
    echo 0 > /sys/module/dynamic_debug/parameters/verbose

    # Sequence verbose level 0..3 to verify error diagnostics across all verbosity states!
    for verbose in 1 2 3; do
	echo $verbose > /sys/module/dynamic_debug/parameters/verbose

	ddcmd_err 'module foo format "parse +p #! unclosed quote: parse +p'

	# comments in queries tell the error in the logs
	ddcmd_err "module foo unknown_keyword value	#! bad flag-op v, at start of value"
	ddcmd_err "module foo %pm	#! bad flag-op %, at start of %pm"
	ddcmd_err "module foo +pfmHKDD	#! unknown flag 'H'"

	ddcmd_err "w1 w2 w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 w13 w 14 w15 w16 #! too many words, legal max <=15"
	ddcmd_err "func w2 w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 +p #! unknown keyword \"w3\""
	ddcmd_err "module foo line =_ #! expecting pairs of match-spec <value>"

	# match-spec duplicate keywords
	ddcmd_err "func foo func bar =_	#! match-spec:func val:foo overridden by bar"
	ddcmd_err "file foo.c file bar.c =_	#! match-spec:file val:foo.c overridden by bar.c"
	ddcmd_err "module foo module baz =_	#! match-spec:module val:foo overridden by baz"
	ddcmd_err "format foo format bar =_	#! match-spec:format val:foo overridden by bar"
	ddcmd_err "class D2_CORE class D2_KMS +p #! match-spec:class val:D2_CORE overridden by D2_KMS"
	ddcmd_err "module foo +x	#! unknown flag 'x'"

	# line value errs
	ddcmd_err "line 10 line 20 +l	#! match-spec: line used 2x"
	ddcmd_err "line 10a +pl		#! bad line-number: 10a"
	ddcmd_err "line 100-10 +pf	#! last-line:10 < 1st-line:100"

	# colon-delimited syntax errs
	ddcmd_err "func foo file bar.c:func_bar =_	#! match-spec:func val:foo overridden by func_bar"
	ddcmd_err "file bar.c:100-10 +pf	#! last-line:10 < 1st-line:100"
	ddcmd_err "file bar.c:10a +pl		#! bad line-number: 10a"
	ddcmd_err "line 10-20 file bar.c:30 +pl	#! match-spec: line used 2x"
    done

    # Reset to default verbose level 0 at the end of basic errors
    echo 0 > /sys/module/dynamic_debug/parameters/verbose
    ddcmd =_
}

# these queries run against the builtin module: params, and change
# flags.  The control file state-of-interest is found by path,
# kernel/params.c, to avoid module keyword entirely
function FT_basic_queries {
    v_echo "${GREEN}# BASIC_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	return
    fi
    ddcmd =_ # zero everything

    ddcmd "module params +mf" 'kernel/params.c'
    ddcmd "module params +l"  'kernel/params.c'
    ddcmd "module params -m"  'kernel/params.c'
    ddcmd "module params =_"  'kernel/params.c'

    # multi-query commands split on ; on a single line
    ddcmd "module params +mf ; module params func parse_args +sl"  'kernel/params.c'

    # verify multi-cmd input, newline separated, with embedded comments
    ddcmd =_ # reset before multiline query to capture full transition
    ddcmd "module params =_		# clear params
      module params +ml			# set flags
      module params func parse_args +fs # set other flags" \
	  'kernel/params.c'

    # clear flags and verify
    ddcmd "module params =_"  'kernel/params.c'
}

function FT_path_module_queries {
    v_echo "${GREEN}# TEST_PATH_MODULE_QUERIES ${NC}"
    ddcmd =_

    # Find how many 'main' modules we have in total (by basename)
    # Use a precise OR pattern to match exactly [main] or [*/main] and avoid irqdomain
    local total_main=$(grep -c "\[main\]\|\[[^]]*/main\]" /proc/dynamic_debug/control)
    v_echo "# found $total_main total 'main' modules"

    if [ $total_main -eq 0 ]; then
        echo "SKIP - no 'main' modules found to test slashes"
        return
    fi

    # Verify a robust, cross-query state-interaction handshake between
    # narrow path and wide wildcard/basename queries. This dynamically
    # proves they interact with the exact same underlying callsites!

    # 1. Turn ON specific path, verified under '[init/main]' range
    ddcmd "module 'init/main' +p" "init/main.c"

    # 2. Turn OFF using wide wildcard query,
    ddcmd "module '*/main' =_" "init/main.c"

    # 3. Turn ON using wide unscoped basename,
    ddcmd "module 'main' +p" "init/main.c"

    # 4. Turn OFF using specific narrow path,
    ddcmd "module 'init/main' =_" "init/main.c"
}

function FT_hyphen_underscore {
    v_echo "${GREEN}# TEST_HYPHEN_UNDERSCORE ${NC}"
    ddcmd =_

    # Find a module with a hyphen in its name (e.g., from the control file)
    local mod_with_hyphen
    mod_with_hyphen=$(awk -F'[][]' \
        '/^[^#:]+:[0-9]+/ { if ($2 ~ /-/) { print $2; exit } }' \
        /proc/dynamic_debug/control)

    if [ -z "$mod_with_hyphen" ]; then
        echo "SKIP - no module with hyphen found in /proc/dynamic_debug/control"
        return
    fi

    v_echo "# testing hyphen/underscore equivalence for module: $mod_with_hyphen"
    local mod_with_underscore=$(echo "$mod_with_hyphen" | tr '-' '_')
    local base_hyphen=$(basename "$mod_with_hyphen")
    local slice_pattern="\[[^]]*$base_hyphen\]"

    # 1. Enable using literal hyphen name, and record the state fingerprint
    v_echo "#   trying hyphen name: $mod_with_hyphen"
    ddcmd "module $mod_with_hyphen +p"
    # verify_control_slice "$slice_pattern"
    local hash_hyphen=$(slice_and_hash_ddctrl "$slice_pattern")

    # 2. Disable and enable using underscore name, record the state fingerprint
    ddcmd =_
    v_echo "#   trying underscore name: $mod_with_underscore"
    ddcmd "module $mod_with_underscore +p"
    # verify_control_slice "$slice_pattern"
    local hash_underscore=$(slice_and_hash_ddctrl "$slice_pattern")

    # Real-time mathematical proof of hyphen/underscore name equivalence!
    if [ "$hash_hyphen" != "$hash_underscore" ]; then
        echo -e "${RED}: Hyphen/Underscore equivalence check failed! " \
            "Fingerprints do not match.${NC}"
        echo -e "Hyphen name state hash:     $hash_hyphen"
        echo -e "Underscore name state hash: $hash_underscore"
        exit $ksft_fail
    else
        v_echo "${GREEN}: Proven: Hyphen/Underscore literal name equivalence matches!${NC}"
    fi

        # Try kbasename with hyphen (if it has a path)
    if [ "$base_hyphen" != "$mod_with_hyphen" ]; then
        ddcmd =_
        v_echo "#   trying hyphen kbasename: $base_hyphen"
        ddcmd "module $base_hyphen +pmf"
        # verify_control_slice "$slice_pattern" # omitted: slice contains dynamic
        # module info which drifts across different targets
        local hash_base_hyphen=$(slice_and_hash_ddctrl "$slice_pattern")

        # Prove kbasename hyphen name matches literal path hyphen name (with different flags)!
        v_echo "#   trying full path hyphen with pmf flags"
        ddcmd =_
        ddcmd "module $mod_with_hyphen +pmf"
        local hash_path_pmf=$(slice_and_hash_ddctrl "$slice_pattern")
        if [ "$hash_path_pmf" != "$hash_base_hyphen" ]; then
            echo -e "${RED}: Hyphen kbasename check failed! " \
                "Fingerprints do not match full-path hyphen enablement.${NC}"
            exit $ksft_fail
        else
            v_echo "${GREEN}: Proven: Hyphen kbasename matches " \
                "full-path hyphen enablement!${NC}"
        fi
    fi

    # 4. Try kbasename with underscore
    local base_underscore=$(echo "$base_hyphen" | tr '-' '_')
    ddcmd =_
    v_echo "#   trying underscore kbasename: $base_underscore"
    ddcmd "module $base_underscore +pmf"
    # verify_control_slice "$slice_pattern" # omitted: slice contains dynamic
    # module info which drifts across different targets
    local hash_base_underscore=$(slice_and_hash_ddctrl "$slice_pattern")

    # Real-time mathematical proof of hyphen/underscore kbasename equivalence!
    if [ "$hash_base_hyphen" != "$hash_base_underscore" ] && \
       [ -n "$hash_base_hyphen" ]; then
        echo -e "${RED}: Hyphen/Underscore kbasename equivalence check " \
            "failed! Fingerprints do not match.${NC}"
        exit $ksft_fail
    elif [ -n "$hash_base_hyphen" ]; then
        v_echo "${GREEN}: Proven: Hyphen/Underscore kbasename " \
            "equivalence matches!${NC}"
    fi

    ddcmd =_
}


# testing classmap-based query enablers and class configurations
function FT_test_classes {
    v_echo "${GREEN}# TEST_CLASSES - classmap-based query enablers and class configs ${NC}"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_

    # 1. Verify initial multi-query enablement state via file slice
    my_modprobe test_dynamic_debug \
        dyndbg="class,D2_CORE,+pf;class,D2_KMS,+ps;class,D2_ATOMIC,+pm"
    verify_control_slice '\[test_dynamic_debug\]'

    # 2. Verify state transition and live-printing end-to-end via ddcmd_load!
    ddcmd_load "class,D2_CORE,+pmf@class,D2_KMS,+pls@class,D2_ATOMIC,+pml" \
        '\[test_dynamic_debug\]' \
        "/sys/module/test_dynamic_debug/parameters/do_classes" "1"

    ifrmmod test_dynamic_debug
}

function FT_classmap_inheritance {
    v_echo "${GREEN}# TEST_MOD_SUBMOD ${NC}"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    # modprobe with plain-old +p & 3 class enablements
    my_modprobe test_dynamic_debug \
	"dyndbg=+p;class D2_CORE +pf;class D2_KMS +pt;class D2_ATOMIC +pm"
    verify_control_slice '\[test_dynamic_debug\]'

    # fresh start, to clear all above flags (test-fn limits)
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    # act on submod, which loads supermod
    my_modprobe test_dynamic_debug_submod \
	"dyndbg=+p;class D2_CORE +pfs;class D2_KMS +pts;class D2_ATOMIC +pmf"

    set_param 0x57 /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    set_param 4 /sys/module/test_dynamic_debug/parameters/p_level_num
    verify_control_slice 'test_dynamic_debug'

    set_param 3 /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    set_param 0 /sys/module/test_dynamic_debug/parameters/p_level_num
    verify_control_slice 'test_dynamic_debug'

    set_param 0x16 /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    set_param 0 /sys/module/test_dynamic_debug/parameters/p_level_num
    verify_control_slice 'test_dynamic_debug'

    # recap DRM_USE_DYNAMIC_DEBUG regression
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    # set super-mod params at load-time
    my_modprobe test_dynamic_debug p_disjoint_bits=0x16 p_level_num=5
    verify_control_slice '\[test_dynamic_debug\]'

    # see them picked up by submod
    my_modprobe test_dynamic_debug_submod
    verify_control_slice 'test_dynamic_debug'

    # Real-time mathematical proof that load-time (modprobe) parameter parsing
    # and runtime (sysfs write) parameter configurations are perfectly equivalent!
    local hash_modprobe=$(slice_and_hash_ddctrl '\[test_dynamic_debug\]')

    # Fresh load with default parameters, then configure them dynamically at runtime
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    my_modprobe test_dynamic_debug
    my_modprobe test_dynamic_debug_submod
    echo 0x16 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 5 > /sys/module/test_dynamic_debug/parameters/p_level_num

    local hash_sysfs=$(slice_and_hash_ddctrl '\[test_dynamic_debug\]')
    if [ "$hash_modprobe" != "$hash_sysfs" ]; then
        echo -e "${RED}: Load-time vs runtime parameter equivalence check failed!${NC}"
        exit $ksft_fail
    else
        v_echo "${GREEN}: Proven: parameter load-time (modprobe) " \
            "and runtime (sysfs write) are equivalent!${NC}"
    fi

    # --- Live Content Fingerprinting Phase ---
    log_start
    echo 1 > /sys/module/test_dynamic_debug/parameters/do_classes
    echo 1 > /sys/module/test_dynamic_debug_submod/parameters/do_classes
    log_stop

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
}

function FT_modprobe_w_param {
    v_echo "${GREEN}# TEST_MODPROBES ${NC}"
    local verbose

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    for verbose in 1 2; do # 3 4 0; do
	echo $verbose > /sys/module/dynamic_debug/parameters/verbose

	# Verify each parameter load sequence with 100% DRY modularity
	verify_modprobe_param_logging "do_prints" "1"

	#verify_modprobe_param_logging "do_classes" "1"
	#verify_modprobe_param_logging "do_bulk" "1"

	# Sequence composite bitmasks to verify disjoint bit transitions
	for mask in "0x05" "0x12" "0x1f" "0x00"; do
            verify_modprobe_param_logging "p_disjoint_bits" "$mask"
	done

	# Sequence levels to verify both growing and shrinking verbose transitions
	for lvl in "3" "5" "4" "0"; do
            verify_modprobe_param_logging "p_level_num" "$lvl"
	done
    done
    ddcmd =_
}

# Built-in Feature Tests (Can run on any CONFIG_DYNAMIC_DEBUG kernel, modular or monolithic)
builtin_tests=(
    FT_grammar_ok
    FT_grammar_errs
    FT_basic_queries
    #FT_path_module_queries
    #FT_hyphen_underscore
)

# Modular Feature Tests (Require CONFIG_MODULES=y and test_dynamic_debug*.ko available)
modular_tests=(
    #FT_test_classes
    #FT_classmap_inheritance
    #FT_modprobe_w_param
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
    cat << 'EOF' | {
#K= b938440930e5f1297d524e6c0c1ec85c FT_grammar_errs.1
#K= b9a397e807148d13ba07da7db7e69817 FT_grammar_errs.2
#K= 5abefa504937a329f974504f1896e368 FT_grammar_errs.3
#K= 1d94d0f239bf40bf51280caeab499de0 FT_grammar_errs.4
#K= 9a5e1c1856f1ebbe57a4afdfc4c9703a FT_grammar_errs.5
#K= b3809a6f9f643f049f1ef30827d4b001 FT_grammar_errs.6
#K= 5d400aad1a7d71f548b6b81274d00a55 FT_grammar_errs.7
#K= 61cd359b10f9051862d3745eda07a295 FT_grammar_errs.8
#K= 567379f4f4d099f00f37d1469d9370bd FT_grammar_errs.9
#K= 31a026d10f85e8f4c9f8cf3e1cef3c61 FT_grammar_errs.10
#K= ba62952b671a553d4b14e344a61926f7 FT_grammar_errs.11
#K= 49aec6c2e99cc70a1e4137f86e94f6f4 FT_grammar_errs.12
#K= cc02cccf15988a98dfd5b1df1e492e31 FT_grammar_errs.13
#K= 2bbf3fac2b77b88b6cebe459ff9631d3 FT_grammar_errs.14
#K= 93b67411c254875a6bae5c01c0239729 FT_grammar_errs.15
#K= b22f2efe54a1637156b9521b2794806e FT_grammar_errs.16
#K= de410dba40d8b097524f91da8ae2b1c5 FT_grammar_errs.17
#K= 6c922048c21f6198720dfd68ae16fdad FT_grammar_errs.18
#K= 4ddd57e9cbc3da2613ac59eb285ad8c1 FT_grammar_errs.19
#K= ba65062f6be00a1d04907dd176d26c19 FT_grammar_errs.20
#K= 94cdf3b32afa4f12a00a704208aa2e53 FT_grammar_errs.21
#K= 1f6fedfe222af475211b3bfc6d08bc63 FT_grammar_errs.22
#K= c67dfeca97697b707acdd70672816dff FT_grammar_errs.23
#K= 9574dbfdac063409b41a44e6251fbcd9 FT_grammar_errs.24
#K= 3c694a0ee2cf3a1d39f2ca8a9f3b9094 FT_grammar_errs.25
#K= c23be05426c8727be833ff055944aa96 FT_grammar_errs.26
#K= cdddf4ffd7fde3c12eb0c73770b872bf FT_grammar_errs.27
#K= cdf7a2e2740ec007efbd29feda478420 FT_grammar_errs.28
#K= f6650d69963cef433c50e1f2acb899fc FT_grammar_errs.29
#K= e150a96693eb561a20eebebfd637e667 FT_grammar_errs.30
#K= b63514cf73a93962311d098284d7b491 FT_grammar_errs.31
#K= b253cde723889c42d2c944c1b14f5a45 FT_grammar_errs.32
#K= 7ad3dfb73e82f4d75b0770179e6dc264 FT_grammar_errs.33
#K= f4d905e4f72dbd535bb80d7fe9df84b0 FT_grammar_errs.34
#K= 5bf50b198d8e5fe300541adfce92e073 FT_grammar_errs.35
#K= 7c4d73d42cff377cd84a7683b0c3e0b9 FT_grammar_errs.36
#K= 19f1aa8d33d71888be74b19a3143938f FT_grammar_errs.37
#K= 84309c1322631ae74ecf2c729b6aa210 FT_grammar_errs.38
#K= 8cfe48871f91a90e6eaaef9123892394 FT_grammar_errs.39
#K= 8aaa5fdd50ead5cd429fbba4123b215c FT_grammar_errs.40
#K= 94cdf3b32afa4f12a00a704208aa2e53 FT_grammar_errs.41
#K= 1a402ada248d0d50cec13a1af150ea33 FT_grammar_errs.42
#K= 5b46a8eb0d74009462f74cf7553e3228 FT_grammar_errs.43
#K= 546e440044828d86135dd63f1eb9ed17 FT_grammar_errs.44
#K= 3c694a0ee2cf3a1d39f2ca8a9f3b9094 FT_grammar_errs.45
#K= 37b7ab754e579683a28ce086c470cdc9 FT_grammar_errs.46
#K= 3e3acfb800cfb1bcb0cc07f33b75830a FT_grammar_errs.47
#K= 42dc6cb4fe938bb82b64c82924ef28d8 FT_grammar_errs.48
#K= cf9320d2188f5b48d508d610d95298b2 FT_grammar_errs.49
#K= 1f33fb1fd2365e77c3cfbd61d1ae2a7c FT_grammar_errs.50
#K= 67588c71f92f319c6224d5917456ecd2 FT_grammar_errs.51
#K= 22e7795f7dbeb6dc2fce5e113312781c FT_grammar_errs.52
#K= 66d253407728c1f8a850519eadd66880 FT_grammar_errs.53
#K= 27e8345aaeed85c1de5516871e0bbd0c FT_grammar_errs.54
#K= 43cc8a154f28f1a668ab409a1af4e77a FT_grammar_errs.55
#K= eda21551fcdd76ef2f192e1d9950a05f FT_grammar_errs.56
#K= 0a8a986b5610630cb75c0568deff8315 FT_grammar_errs.57
#K= 29bfae73351c3157a5fe409febcef276 FT_grammar_errs.58
#K= 73a9e53428d1fb1a9813e3eab7d0ea18 FT_grammar_errs.59
#K= 800951095b297532e25785ab101a2ba9 FT_grammar_errs.60
#K= 6e8599556a312200fb6d484565b6c52f FT_basic_queries.1
#K= db17180b59444e5e34a8fc20c40e4530 FT_basic_queries.2
#K= f7bd56bd407ff255ef2e3b5fb093ae6a FT_basic_queries.3
#K= 4ce44468b3b5f80ae42ade1f261b952f FT_basic_queries.4
#K= 02e4fd94602e108cb89bfc70d47a5dad FT_basic_queries.5
#K= f03a7ca7316e8db4c0e16523dc41e75d FT_basic_queries.6
#K= c518a50ba30ba8099d0dc874a27ecf16 FT_basic_queries.7
EOF
        # Read the K-recs and skip those for tests that can't run
        while read -r line; do
            # Filter built-in if needed
            if [ "${LACK_DD_BUILTIN:-0}" -eq 1 ]; then
                # Extract label (3rd field) from #K= line
                local label=$(echo "$line" | awk '{print $3}')
                if [[ "$label" == FT_basic_queries* \
			  || "$label" == FT_path_module_queries* \
			  || "$label" == FT_comma_terminators* \
			  || "$label" == FT_multi_query* ]]; then
                    continue
                fi
            fi
            # Filter modular if needed
            if [ "${LACK_TMOD:-0}" -eq 1 ]; then
                # Extract label (3rd field) from #K= line
                local label=$(echo "$line" | awk '{print $3}')
                if [[ "$label" == FT_test_classes* \
			  || "$label" == FT_classmap_inheritance* \
			  || "$label" == FT_modprobe_w_param* ]]; then
                    continue
                fi
            fi
            echo "$line"
        done
    }
}

# ==============================================================================
# Run tests

# Clear any stale seen/unregistered/drifted hashes from previous runs
: > "$SEEN_HASHES_FILE"
: > "$UNREG_HASHES_FILE"
: > "$DRIFT_HASHES_FILE"

ifrmmod test_dynamic_debug

# Check if loadable module support or our test modules are missing/builtin
LACK_TMOD=0
if [ -d "/sys/module/test_dynamic_debug" ]; then
    # If module is present but not in /proc/modules,
    # it is a builtin module (cannot unload/reload)
    if ! grep -q "^test_dynamic_debug " /proc/modules 2>/dev/null; then
        LACK_TMOD=1
    fi
else
    # Check if we can modprobe it from disk
    modprobe -q -n test_dynamic_debug || LACK_TMOD=1
fi

# 1. Run all Built-in Feature Tests
v_echo "${GREEN}# RUNNING BUILT-IN FEATURE TESTS ${NC}"
for test_func in "${builtin_tests[@]}"; do
    $test_func
    v_echo ""
done

# 2. Run Modular Feature Tests only if test modules are available
if [ $LACK_TMOD -eq 0 ]; then
    v_echo "${GREEN}# RUNNING MODULAR FEATURE TESTS ${NC}"
    for test_func in "${modular_tests[@]}"; do
        $test_func
        v_echo ""
    done
else
    v_echo "${YELLOW}# SKIPPING MODULAR TESTS: test_dynamic_debug.ko not available ${NC}"
fi

if [ "$V" -ge 1 ]; then
    echo -en "${GREEN}# Done on: "
    date
    echo -en "${NC}"
fi

audit_golden_records

# Output consolidated blocks of unregistered and drifted fingerprints
failed=0

if [ -s "$UNREG_HASHES_FILE" ]; then
    if [ "$K" -ne 2 ]; then
        echo -e "${YELLOW}\n# --- Unregistered Baselines ---"
        cat "$UNREG_HASHES_FILE"
        echo -e "# ------------------------------${NC}"
    fi
    : > "$UNREG_HASHES_FILE"
    failed=1
fi

if [ -s "$DRIFT_HASHES_FILE" ]; then
    if [ "$K" -ne 2 ]; then
        echo -e "${RED}\n# --- Drifted Baselines ---"
        cat "$DRIFT_HASHES_FILE"
        echo -e "# -------------------------${NC}"
    fi
    : > "$DRIFT_HASHES_FILE"
    failed=1
fi

if [ $failed -eq 1 ]; then
    [ "$K" -eq 1 ] && echo "fake success" && exit $ksft_pass
    [ "$K" -eq 2 ] && exit $ksft_pass
    exit $ksft_fail
fi

exit $ksft_pass

