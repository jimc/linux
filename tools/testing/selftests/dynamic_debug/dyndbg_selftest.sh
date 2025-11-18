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
K=${K:=0}  # K=1,2 to tolerate/pass on checksum mismatches

# Sanitize V to ensure it is a valid integer
if [[ ! "$V" =~ ^[0-9]+$ ]]; then
    V=0
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

# Clean up any leftover loaded test modules at initialization
grep -q "^test_dynamic_debug_submod " /proc/modules 2>/dev/null \
    && rmmod test_dynamic_debug_submod
grep -q "^test_dynamic_debug " /proc/modules 2>/dev/null \
    && rmmod test_dynamic_debug

# ==============================================================================
# TESTING STRATEGY 1.
#   Change and observe control-file settings:
#     ddcmd: ie echo $dd_query_cmd > /proc/dynamic_debug/control
#     read back control, count changes due to query_cmd
# ==============================================================================
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
    IN_BOOKEND=1
    DDCMD_LOG="modprobe $*"
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
	[ "$3" == 1 ] && echo "Error: '$error_msg'"
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

function ifrmmod {
    [ "${LACK_TMOD:-0}" -eq 1 ] && return
    grep -q "^$1 " /proc/modules 2>/dev/null && rmmod $1
}

# ==============================================================================

function verify_modprobe_param_logging {
    # $1 - parameter name (e.g. do_prints)
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
        set_param 1 /sys/module/test_dynamic_debug/parameters/do_classes
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
    ddcmd "module foo file bar.c func buz class D2 line 100 +_" # 5 keywords

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

	ddcmd_err 'module foo format "parse +p'		# unclosed double quote

	# comments in queries tell the error in the logs
	ddcmd_err "module foo unknown_keyword value	# no flag err"
	ddcmd_err "module foo %pm	# bad flag-op "
	ddcmd_err "module foo +pfmHKDD	# bad flags after good "

	ddcmd_err "w1 w2 w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 w13 w 14 w15 w16 # too many tokens"
	ddcmd_err "func w2 w3 w4 w5 w6 w7 w8 w9 w10 w11 w12 +p # bad keyword w3"
	ddcmd_err "module foo line =_ # no line val"

	#
	ddcmd_err "func foo func bar =_		# func used 2x"
	ddcmd_err "module foo module baz =_	# module used 2x"
	ddcmd_err "class D2_CORE class D2_KMS +p # class used 2x"
	ddcmd_err "module foo +x		# unrecognized flag character"

	# line value errs
	ddcmd_err "line 10 line 20 +l		# line used 2x"
	ddcmd_err "line 10a +pl			# line value trailing garbage"
	ddcmd_err "line 100-10 +pf		# line range error (last < 1st)"
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

    # multi-query commands on a single line, split on ;/@ respectively
    ddcmd "module params +mf ; module params func parse_args +sl"  'kernel/params.c'
    ddcmd "module params -f @ module params func parse_args -l"  'kernel/params.c'

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

# test parsing on spaces, commas. testing against builtin [kernel/params]
# disabled pending feature patch
function FT_comma_terminators {
    v_echo "${GREEN}# COMMA_TERMINATOR_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	return
    fi
    ddcmd "module params =_"

    ddcmd "module,params,=_" 'kernel/params.c'
    ddcmd "module,params,+mf" 'kernel/params.c'
    # ignore empty tokens
    ddcmd ",module ,, ,  params, -p" 'kernel/params.c'
    ddcmd " , module ,,, ,  params, -m" 'kernel/params.c'

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

    set_param 5 /sys/module/test_dynamic_debug/parameters/p_level_num
    verify_control_slice '\[test_dynamic_debug\]'

    my_modprobe test_dynamic_debug_submod
    verify_control_slice 'test_dynamic_debug_submod'

    # fresh start, to clear all above flags (test-fn limits)
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    # load submod, which loads supermod
    my_modprobe test_dynamic_debug_submod \
	"dyndbg=+p;class D2_CORE +pfs;class D2_KMS +pts;class D2_ATOMIC +pmf"
    verify_control_slice 'test_dynamic_debug'

    # runtime changes to both
    set_param 0x57 /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    set_param 4 /sys/module/test_dynamic_debug/parameters/p_level_num
    verify_control_slice 'test_dynamic_debug'

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
	verify_modprobe_param_logging "do_classes" "1"
	verify_modprobe_param_logging "do_bulk" "1"

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
    FT_path_module_queries
    FT_hyphen_underscore
    FT_comma_terminators
)

# Modular Feature Tests (Require CONFIG_MODULES=y and test_dynamic_debug*.ko available)
modular_tests=(
    #FT_test_classes
    FT_classmap_inheritance
    FT_modprobe_w_param
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
#K= f3dbd5afb9aa1750f93275b634499e22,bb85d28739876b0fdf1eb426baae8ef7 FT_grammar_errs.1        dmesg
#K= 200c01632c52a63f6d186da1c6460740,9c405e06fcea50284df82d1ae9692403 FT_grammar_errs.2        dmesg
#K= 7d7141900ce6e32f15c99202309c63a4,0b0d9b0e6f4c45c7d75d699fd3b6aa93 FT_grammar_errs.3        dmesg
#K= 1bb798a5831d0119789d424ef6cb55c4,e008644e159c2a9d99a8d0806c06e71e FT_grammar_errs.4        dmesg
#K= 5edd66e308b2792d5694df86c07a3eaf,11833e6cf8fe8b03869aa4f669b63398,5750ed178633f9a68623f928c093d16e FT_grammar_errs.5        dmesg
#K= 6f87d92ffe0812550f43287127c6f2b9,e85a4b665f5ef141e3f4d4505dec1da1,2cd51e5e3c3e2501544524bed0fab620 FT_grammar_errs.6        dmesg
#K= c0eb05b58a008c722e091e1ae74440ec,3e1eba65bd936e1276c7e1d76b399595 FT_grammar_errs.7        dmesg
#K= 911929ec0e2ffc1f13822b479dec6805,e3ac86676e309d4e72ff3da287736c29 FT_grammar_errs.8        dmesg
#K= c1407512376369d2e591a4b25a4b607a,466d325c23519119f754fa7860bae7c6 FT_grammar_errs.9        dmesg
#K= 2046abda72725ea06fe339d5f364f1c9,51c489366cd86c16280858b95adf34ac FT_grammar_errs.10       dmesg
#K= b72f7fccf76f8a5bee47a05d7bb545fb,dfc632c6c0cd6206b6141dbc5fb989db FT_grammar_errs.11       dmesg
#K= 98e2bd3e4f3da58536496a38ec3e6238,69650fc26e97a78082bd851eb4a5c83b FT_grammar_errs.12       dmesg
#K= b371c6ba52503d037dbc43da788af8be,5cfe5665962a482f416836793067f2a2 FT_grammar_errs.13       dmesg
#K= cb8288d607b0c5282125852f3ab05107,661cc9a2d8d3c381bfeae41392432575 FT_grammar_errs.14       dmesg
#K= 9346a310c4ad57cc3746afbace702c3e,5dd0accf9be37ce2f4bf697e9bdebe30 FT_grammar_errs.15       dmesg
#K= 533d27af85eed3c0fd2eaec961982a36,bd8d9cf661c9aca54dac93c74cfc1501 FT_grammar_errs.16       dmesg
#K= 114e0632585e205a3347c82bac7d79f2,e991391048f2fdffaaed99ba36b787ed FT_grammar_errs.17       dmesg
#K= 73f5c173bafdfb9674b5ecce77db3354,82d99278f471ebfbf47eeb19898525cf FT_grammar_errs.18       dmesg
#K= 0fc110d078f60eacdd389e5975ba18d9,3c8d268c94b0bf97e64999654cd840c4,f3f088fa7d276981bf2898c048087f76 FT_grammar_errs.19       dmesg
#K= 815a1c52f365510c644450bb80c07e72,1a448ff3f1df9155cc3acf1e124f24f5,984ecc198e0fa9fc91bc31caf7da2a5d FT_grammar_errs.20       dmesg
#K= 621e3cd81b553973cb40a935bb9298f1,76c8a33c585d414fcc3f1c8b5ea5b4b3 FT_grammar_errs.21       dmesg
#K= 581222901232344ade18bbda58302c48,90cfc66e9d61f5c448353b97baa99145 FT_grammar_errs.22       dmesg
#K= ea0aae3e01b3bb22eb8ad7acd327b371,37a51dfd6e84cb64b69d3a0e23e68ceb FT_grammar_errs.23       dmesg
#K= 8f28189bff62a3d5ed16f537d41a725a,3aefa325b63742710b45e4531152c9d8 FT_grammar_errs.24       dmesg
#K= 19c425e5d3a645b5dc5e23758ba0f4a1,bc8d2c23282cd6834879bc1f1b303d05 FT_grammar_errs.25       dmesg
#K= 75415542333f2250f0e060a54dae50f8,092ec5530d875be1dc96ad4202fc7dc3 FT_grammar_errs.26       dmesg
#K= 0955815c0e595ab2206e25aa31fe1ef2,98caa2677e9070953a852b27ab57861e FT_grammar_errs.27       dmesg
#K= 3ff4c0b60db33e44c3cd6f0e14f81e3e,8d755baa707c7b3b67491bf84708ef08 FT_grammar_errs.28       dmesg
#K= 9346a310c4ad57cc3746afbace702c3e,5dd0accf9be37ce2f4bf697e9bdebe30 FT_grammar_errs.29       dmesg
#K= 7aaf0a16c287e66b62e11298ee160b34,2e37642dc8aee3d04b8a54a76b3b891c FT_grammar_errs.30       dmesg
#K= 06350c62105b537cdd0c67736b29727d,593c8b4c41932e3bde564a0008627ced FT_grammar_errs.31       dmesg
#K= 6ef0ec01805c8719d828553098f95377,85a2a0bee8d0cd10f883aa5477d62efb FT_grammar_errs.32       dmesg
#K= 0fc110d078f60eacdd389e5975ba18d9,3c8d268c94b0bf97e64999654cd840c4,f3f088fa7d276981bf2898c048087f76 FT_grammar_errs.33       dmesg
#K= 72203b2d88d0617cd5c659d3b80e26f9,1a448ff3f1df9155cc3acf1e124f24f5,629f6a7e379e02f38c543cdb48e3b84f FT_grammar_errs.34       dmesg
#K= 6ecb03736d5cddb7ab2aaff49d561be9,d9f0cffb0898c54690735a71b0d4f3bf FT_grammar_errs.35       dmesg
#K= eaa989336cb7c96c13ef4a3964fc6898,4cf728b6a32043ea5336a14d0c435312 FT_grammar_errs.36       dmesg
#K= 5b624d9c133d7bb4f5370c3ce06929ed,a11ae3f86f4e5f539fd6168d318f74b4 FT_grammar_errs.37       dmesg
#K= 127275739b1fe04c84eedad28ec154f6,c7d3830b0391f6a773205066b64e15f9 FT_grammar_errs.38       dmesg
#K= 3e2fd15a7e8c0583bc5066524bc50508,5c506ea2bfd050439b580b91543e9be7 FT_grammar_errs.39       dmesg
#K= 781995971d28f732a792522f3c56cdd3,a257660007408f7bb0cb43c99b1c7bb8 FT_grammar_errs.40       dmesg
#K= 6614a677d9f9ac09d9825b4e989d2c42,0082939f100300a7b327b143cf40cb43 FT_grammar_errs.41       dmesg
#K= 70de9afed457a6be9f9c3c81cbd6d4d5,657ac4360f978687ad25a705b2bb89f1 FT_grammar_errs.42       dmesg
#K= 99985cce918eb5108ecb3658249f6bc7,6e8599556a312200fb6d484565b6c52f FT_basic_queries.1       "kernel/params.c"
#K= eb3bd35439cc289ef59ee967aad4d540,db17180b59444e5e34a8fc20c40e4530 FT_basic_queries.2       "kernel/params.c"
#K= 00359a9a05d439ec3a850a55e437fcbd,f7bd56bd407ff255ef2e3b5fb093ae6a FT_basic_queries.3       "kernel/params.c"
#K= b24b1a8081d7514fa593cc28f6fb645b,4ce44468b3b5f80ae42ade1f261b952f FT_basic_queries.4       "kernel/params.c"
#K= de950a3e60669fdd58d0a8c2867a056d,02e4fd94602e108cb89bfc70d47a5dad FT_basic_queries.5       "kernel/params.c"
#K= 2ff49f0c4d18ec99bcb1c30840fe8afc,f03a7ca7316e8db4c0e16523dc41e75d FT_basic_queries.6       "kernel/params.c"
#K= 9a1b13c32a15363dcf93913308edeea5,c518a50ba30ba8099d0dc874a27ecf16 FT_basic_queries.7       "kernel/params.c"
#K= c3511fd3669af58188acf121c05914ca,fb294f02a4207b28b2a874524ef07afd,ccd518e373775f0c94cabe81d93584e4 FT_classmap_inheritance.1 "\[test_dynamic_debug\]"
#K= c3511fd3669af58188acf121c05914ca,7a0b87016fdc237077dfe96bbbb3661b,ac599497ac899d0f175673b67bf13725 FT_classmap_inheritance.2 "\[test_dynamic_debug\]"
#K= 29731f5b3707d4fb7e93888f95d6da02,2784d60f5056fc5cc03b3ceb854293f5,5a9479ff72e261b6c6cbd2a855a991d0 FT_classmap_inheritance.3 "test_dynamic_debug_submod"
#K= 62501908ed46fb83205bebd80f850d56,bf66aaf8ff612272c0cda29778ed2131,38e813e9025107ac3e24226b8d487a92 FT_classmap_inheritance.4 "test_dynamic_debug"
#K= 1d2ac19332c416e913d9fff8661db4b9,49fdd29d91a4c1d16f8b59bb431e741b,9b82b12a35ad98ef26183db15071f70e FT_classmap_inheritance.5 "test_dynamic_debug"
#K= 8dd8f7c4b3b7777d5279b6635ec83f7b,a8aa244285d048b5ebe33061fa99c424,d4937472530af6fdcb0a2440d4a366ea FT_classmap_inheritance.6 "\[test_dynamic_debug\]"
#K= a8dfa89c4daf89f13a015e87a36077c1,3060b86a0f553dd5a826bb7023284925,fea6f925b829f75a5b2d4e837738fa12 FT_classmap_inheritance.7 "test_dynamic_debug"
#K= 68b329da9893e34099c7d8ad5cb9c940,f43e0aff8a4b38435b73d90ed8100d1b,7e92245008439ee79fe2460aeaa16a9b FT_classmap_inheritance.8 dmesg
#K= d406a3a4cf51460a4975ed457c02a992,0f3a3a0814ef979d7d5b6ba42c6b5542,94610c57ac44bd7011002a654fd78f93 FT_modprobe_w_param.1    dmesg
#K= b209fb96c2f32981750ca5f71ccf92d1,ec0781a78e51590d8481c2551411ded6,94610c57ac44bd7011002a654fd78f93 FT_modprobe_w_param.2    dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,c1309e18dc9bf2f57184fa13164d917d FT_modprobe_w_param.3    "\[test_dynamic_debug\]"
#K= 3eecda71b85be31ea5bc848d713da91f,b7b913ba270695efd7a812f67ebda48c,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.4    dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,a1232e658d95fbca8b23a69e9a0db965 FT_modprobe_w_param.5    "\[test_dynamic_debug\]"
#K= 84ec18133715b6ff3204dd64414588ec,ea1267e206c3e3d062e829ef82fe68f1,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.6    dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,af7b3d532325b1c5ab990e4b32fed577 FT_modprobe_w_param.7    "\[test_dynamic_debug\]"
#K= 591411c42cf52d7c4c46d76bcc345a5f,5746de4ee0f35a779a4fad6f2186b708,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.8    dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,591411c42cf52d7c4c46d76bcc345a5f FT_modprobe_w_param.9    "\[test_dynamic_debug\]"
#K= f31e66ed431512b21d5ea779017ede0d,2f7acce16b80572d5e8eaee6bda4d227,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.10   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,b0435304108118e64529469e59332111 FT_modprobe_w_param.11   "\[test_dynamic_debug\]"
#K= 52e8d9bf30e67e2794618aaf15e78ef2,3f26cbf7e3f672f5f077a03d6ff86831,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.12   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,4d036833ce9f661057a4e13d97295c65 FT_modprobe_w_param.13   "\[test_dynamic_debug\]"
#K= 92e575a9402b53b8fd369c0e30a24247,1cde346e273ca9854cb947576b22b528,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.14   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,5c3c6ecf6a46f9ccebd60c5ca9ebdbb7 FT_modprobe_w_param.15   "\[test_dynamic_debug\]"
#K= 73a93377a823739e8aae44856a20fa7f,dafad094064271d2ac6a47859a63d67b,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.16   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,73a93377a823739e8aae44856a20fa7f FT_modprobe_w_param.17   "\[test_dynamic_debug\]"
#K= 3d9505b65ed7498f04b91827f489111a,27e436316b20a0189f6a46cdfe5a03b3,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.18   dmesg
#K= 5bad46c3c323bb274c6ccef2e5adc7f6,d39a9490cdcd4f69f400d207501b794a,10464b2c3e3972f05e93c609700f8fb2 FT_modprobe_w_param.19   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,10464b2c3e3972f05e93c609700f8fb2 FT_modprobe_w_param.20   "\[test_dynamic_debug\]"
#K= 9523c153c7bc4354a90c23690f43e479,943c6084b990afdff3dda65836be49be,07c1f81d5a58675a291dc77acd6938c4 FT_modprobe_w_param.21   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.22   "\[test_dynamic_debug\]"
#K= 729332fc241734565782b2fad5b1d648,8a4cbe52f1f102b360e7419b1de0145e,b070066b0eb13a033446bd05850b15e2 FT_modprobe_w_param.23   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.24   "\[test_dynamic_debug\]"
#K= 7b91db8e9f160aebb1ee87fab2232404,e220f2b25973e7e2c2e4e972796e27cf,32ca47823c27e629e03c21aebfc25095 FT_modprobe_w_param.25   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.26   "\[test_dynamic_debug\]"
#K= f1b772ca7b98a8d18eb34635a863f883,73564da4d991018d2e5f704f0425daa0,7b91db8e9f160aebb1ee87fab2232404 FT_modprobe_w_param.27   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.28   "\[test_dynamic_debug\]"
#K= 086650af00f66c73a051b6afbb71bf44,01276487a4aed602f729e513885755a7,e393499e02677de414e478f4e710eeb9,caa849a2817863d68a8d11ee415b049c FT_modprobe_w_param.29   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.30   "\[test_dynamic_debug\]"
#K= 72a67c4c9de15f08d1212b96ebc71213,1983b6dc4cafc7cb5bd16cf4135f3674,375e38613af3b49bb7c7689dfecf4177,e94cc54f62faa428a03f2a7dbca06f97 FT_modprobe_w_param.31   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.32   "\[test_dynamic_debug\]"
#K= 3d2538bf868e71bff17c768cf118c352,58dc8705e7070a8f4c6262ee6b29db7b,745a61d20e26a6a22db0b99420fea80a,8919dde0fee0cf42f9388e541b33aa01 FT_modprobe_w_param.33   dmesg
#K= ef22493a8baadddc5dd0291577e413c8,62e34fa11ce7e711bfe810d6452697a7,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.34   "\[test_dynamic_debug\]"
#K= 1ca895d63d34634ce1a72189aedb7be3,3d2538bf868e71bff17c768cf118c352,ff5bf6afec9642da83d3dcdb5e732ab9 FT_modprobe_w_param.35   dmesg
#K= d7ed6935730c356ac78d6a9e7184b5d6,030cda0a59aaae95750d5ec55acbcb8c FT_modprobe_w_param.36   "\[test_dynamic_debug\]"
EOF
        # Read the K-recs and skip those for tests that can't run
        while read -r line; do
            # Filter built-in if needed
            if [ "${LACK_DD_BUILTIN:-0}" -eq 1 ]; then
                # Extract pattern (4th field) from #K= line
                local pattern=$(echo "$line" | awk '{print $4}')
                if [[ "$pattern" == *params* || "$pattern" == *main* ]]; then
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
rm -f "$SEEN_HASHES_FILE" "$UNREG_HASHES_FILE" "$DRIFT_HASHES_FILE"

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
    echo -e "${YELLOW}\n# --- Unregistered Baselines ---"
    cat "$UNREG_HASHES_FILE"
    echo -e "# ------------------------------${NC}"
    rm -f "$UNREG_HASHES_FILE"
    failed=1
fi

if [ -s "$DRIFT_HASHES_FILE" ]; then
    echo -e "${RED}\n# --- Drifted Baselines ---"
    cat "$DRIFT_HASHES_FILE"
    echo -e "# -------------------------${NC}"
    rm -f "$DRIFT_HASHES_FILE"
    failed=1
fi

# Cleanup
rm -f "$UNREG_HASHES_FILE" "$DRIFT_HASHES_FILE"

if [ $failed -eq 1 ]; then
    [ "$K" -eq 1 ] && echo "fake success" && exit $ksft_pass
    exit $ksft_fail
fi

exit $ksft_pass

