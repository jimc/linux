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

function v_echo {
    [ "${V:-0}" -ge 1 ] && echo -e "$@"
}

[ -e /proc/dynamic_debug/control ] || {
    echo -e "${RED}: this test requires CONFIG_DYNAMIC_DEBUG=y ${NC}"
    exit $ksft_skip # nothing to test here, no good reason to fail.
}

lsmod >/dev/null 2>&1 || {
    echo -e "${RED}: lsmod requires /proc/modules ${NC}"
    exit $ksft_skip # maybe later we can do more
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
grep -q "^test_dynamic_debug_submod " /proc/modules 2>/dev/null && rmmod test_dynamic_debug_submod
grep -q "^test_dynamic_debug " /proc/modules 2>/dev/null && rmmod test_dynamic_debug

# ==============================================================================
# TESTING STRATEGY 1.
#   Change and observe control-file settings:
#     ddcmd: ie echo $dd_query_cmd > /proc/dynamic_debug/control
#     read back control, count changes due to query_cmd
# ==============================================================================

function ddcmd () {
    # ddcmd <query_args> [range_pattern] [pass|fail|log]
    local args="$1"
    local range_pattern="$2"
    local action="${3:-pass}"

    # Verify if range transition check is requested (non-empty pattern)
    local do_range=0
    if [ -n "$range_pattern" ]; then
        do_range=1
    fi

    local exp_exit_code=0
    local do_log=0

    if [ "$action" = "fail" ]; then
	exp_exit_code=1
        do_log=1
    elif [ "$action" = "log" ]; then
        exp_exit_code=0
        do_log=1
    fi

    # Update cumulative state-machine lineage
    if [[ "$args" == *"=_"* ]]; then
        CUMULATIVE_DDCMDS="$args"
    else
        CUMULATIVE_DDCMDS="${CUMULATIVE_DDCMDS}; $args"
    fi

    # Update local bookend command tracker
    if [ "$IN_BOOKEND" -eq 1 ]; then
        if [ -z "$LOCAL_DDCMDS" ]; then
            LOCAL_DDCMDS="$args"
        else
            LOCAL_DDCMDS="${LOCAL_DDCMDS}; $args"
        fi
    else
        LOCAL_DDCMDS="$args"
    fi

    # 1. Automatically start dmesg capture if requested (fail or log)
    [ "$do_log" -eq 1 ] && log_start

    # 2. Automatically take pre-state file snapshot if requested (non-empty range)
    [ "$do_range" -eq 1 ] && capture_before "$range_pattern"

    output=$( (echo "$args" > /proc/dynamic_debug/control) 2>&1)
    exit_code=$?
    error_msg=$(echo "$output" | cut -d ":" -f 5 | sed -e 's/^[[:space:]]*//')
    
    # Handle the exit code check
    handle_exit_code $BASH_LINENO $FUNCNAME $exit_code $exp_exit_code

    # Close and verify dmesg capture if we started one
    [ "$do_log" -eq 1 ] && log_stop

    # Close and verify range transition diff if we started one (non-empty range)
    [ "$do_range" -eq 1 ] && verify_after_change
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
        echo -e "${RED}: $BASH_SOURCE:$1 $2() expected to exit with code $exp_exit_code, got $3${NC}"
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
function slice_and_hash_ddctrl {
    local slice=$(slice_by_grep "$1" "$CONTROL_FILE")
    echo "$slice" | tr -d '\r' | md5sum | cut -d' ' -f1
}

function verify_modprobe_param_logging {
    # $1 - parameter name (e.g. do_classes)
    # $2 - parameter value (e.g. 1)
    # $3 - short descriptive tag (e.g. classes)
    local param="$1"
    local val="$2"
    local tag="$3"

    # Make sure both modules are completely unloaded to trigger a fresh load
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    # Explicitly register the modprobe command as the local stimulus!
    LOCAL_DDCMDS="modprobe test_dynamic_debug ${param}=${val}"

    # 1. Capture and verify the load-time (modprobe) dmesg logs (R2)
    log_start
    modprobe test_dynamic_debug "${param}=${val}"

    # If it is a state-controlling parameter, trigger the print-workload 'do_classes=1'
    # inside the same syslog dmesg capture bookends to verify their actual pr_debug logging!
    if [ "$param" = "p_disjoint_bits" ] || [ "$param" = "p_level_num" ]; then
        LOCAL_DDCMDS="${LOCAL_DDCMDS}; echo 1 > /sys/module/test_dynamic_debug/parameters/do_classes"
        echo 1 > /sys/module/test_dynamic_debug/parameters/do_classes
    fi

    log_stop

    # 2. If it is a state-controlling parameter, verify runtime unsetting (S2 ➔ R1)
    if [ "$param" = "p_disjoint_bits" ] || [ "$param" = "p_level_num" ]; then
        # Dynamically write 0 to unset the parameter at runtime via sysfs
        LOCAL_DDCMDS="echo 0 > /sys/module/test_dynamic_debug/parameters/${param}"
        echo 0 > "/sys/module/test_dynamic_debug/parameters/${param}"
        
        # Verify that the resulting control-file state is completely cleared (all disabled)
        local hash_unset=$(slice_and_hash_ddctrl '\[test_dynamic_debug\]')
        local default_hash="fd89900c8614f23c3a6e8a8d45aa3280" # Pristine fully-disabled slice hash

        if [ "$hash_unset" != "$default_hash" ]; then
            echo -e "${RED}: Runtime unsetting failed for ${param}! Expected default-disabled, got ${hash_unset}${NC}"
            exit $ksft_fail
        else
            [ "$V" -ge 2 ] && echo -e "${GREEN}✔ Proven Runtime Unset: echo 0 > ${param} successfully cleared callsites${NC}"
        fi
    fi
}

# ==============================================================================
function ifrmmod {
    [ "${LACK_TMOD:-0}" -eq 1 ] && return
    grep -q "^$1 " /proc/modules 2>/dev/null && rmmod $1
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
function FT_grammar_errs {
    v_echo "${GREEN}# GRAMMAR_ERROR_TESTS ${NC}"
    ddcmd =_
    local verbose

    # Reset before loop
    echo 0 > /sys/module/dynamic_debug/parameters/verbose

    # Sequence verbose level 0..3 to verify error diagnostics across all verbosity states!
    for verbose in 0 1 2 3; do
        echo $verbose > /sys/module/dynamic_debug/parameters/verbose
        ddcmd =_ # Reset cumulative query log and control file state before each run

        # Semantic, zero-boilerplate, declarative error assertions.
        # Because these expect failure, ddcmd_err automatically captures,
        # hashes, and verifies their dmesg error diagnostics!
        ddcmd_err 'module foobar format "parse'
        ddcmd_err "module foobar unknown_keyword value"
        ddcmd_err "module foobar %pm"
        ddcmd_err "word1 word2 word3 word4 word5 word6 word7 word8 word9 word10 word11 word12"
        ddcmd_err "module foobar line"

        # Advanced parser error path coverage (EINVAL returns)
        ddcmd_err "line 10 line 20"                  # overridden line keyword
        ddcmd_err "line 10a"                         # line syntax trailing garbage
        ddcmd_err "line 100-10"                      # line range error (last < 1st)
        ddcmd_err "module foobar module baz"         # overridden module keyword
        ddcmd_err "class D2_CORE class D2_KMS"       # overridden class keyword
        ddcmd_err "module foobar +x"                 # unrecognized flag character
    done

    # Reset to default verbose level 0 at the end of basic errors
    echo 0 > /sys/module/dynamic_debug/parameters/verbose
    ddcmd =_
}

function FT_grammar_ok {
    v_echo "${GREEN}# GRAMMAR_OK_TESTS ${NC}"
    # Verify successful query syntax compiles and executes with code 0 (success).
    # Since these are side-effect-free proofs carrying empty flags, we omit the
    # range argument to bypass redundant diff-verification, completely removing
    # golden records maintenance and target instability risks!
    ddcmd "+_"
    ddcmd "-_"

    # Verify a comprehensive query containing all valid grammar keywords and value-pairs.
    ddcmd "module foobar file foobar.c func foobar_func line 1-10 class D2_CORE +_"

    # 3. Dedicated lineno range grammar assertions (side-effect-free proofs)
    ddcmd "line 42 +_"       # test exact line syntax
    ddcmd "line 10- +_"      # test open-ended line range (starting at 10)
    ddcmd "line -100 +_"     # test open-ended line range (ending at 100)
    ddcmd "line 10-100 +_"   # test closed-interval line range

    # 4. Dedicated colon-delimited file:line and file:func assertions (side-effect-free proofs)
    ddcmd "file a_file.c:1-100 +_"  # test file:linerange syntax
    ddcmd "file b_file.c:30 +_"     # test file:exact_line syntax
    ddcmd "file c_file.c:c_func +_" # test file:function_name syntax
    ddcmd "file c_file.c:start_* +_" # test file:wildcard_function syntax

    # 5. Advanced formatting and separator checks (side-effect-free proofs)
    ddcmd "format \"space\\040here\" +_" # test format query with octal escape
    ddcmd "module,foobar +_"            # test comma token separator syntax
    ddcmd "func *my_func* +_"           # test wildcard func syntax
    ddcmd "file drivers/usb/* +_"       # test wildcard file path syntax
}

function FT_basic_queries {
    v_echo "${GREEN}# BASIC_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	exit $ksft_skip
    fi
    ddcmd =_ # zero everything

    # 1. Verify params enablement and state transition on a single, clean line!
    # Use non-printing decorator flags (+mf) to avoid print-enabling (+p) and prevent syslog pollution
    ddcmd "module params +mf" '\[kernel/params\]'

    # 2. Verify subsequent flag accumulation and stripping toggles in sequence
    ddcmd "module params +l" '\[kernel/params\]'
    ddcmd "module params -m" '\[kernel/params\]'
    ddcmd "module params =_" '\[kernel/params\]'

    # 3. Verify multi-query command splitting on @ on a single line
    ddcmd "module params +mf @ module params func parse_args +sl" 'kernel/params'

    # 4. Verify multi-cmd input, newline separated, with embedded comments transition
    ddcmd =_ # reset before multiline query to capture full transition
    ddcmd "module params =_                      # clear params
      module params +ml                             # set flags
      module params func parse_args +fs             # other flags" '\[kernel/params\]'

    ddcmd =_
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

    # Verify a robust, cross-query state-interaction handshake between narrow path
    # and wide wildcard/basename queries. This dynamically proves they interact
    # with the exact same underlying callsites!
    
    # 1. Turn ON specific path, verified under '[init/main]' range
    ddcmd "module 'init/main' +p" "\[init/main\]"

    # 2. Turn OFF using wide wildcard query, proving it covers the specific path site!
    ddcmd "module '*/main' =_" "\[init/main\]"

    # 3. Turn ON using wide unscoped basename, proving it reaches the deep path site!
    ddcmd "module 'main' +p" "\[init/main\]"

    # 4. Turn OFF using specific narrow path, proving it targets the basename-loaded site!
    ddcmd "module 'init/main' =_" "\[init/main\]"
}

function FT_hyphen_underscore {
    v_echo "${GREEN}# TEST_HYPHEN_UNDERSCORE ${NC}"
    ddcmd =_

    # Find a module with a hyphen in its name (e.g., from the control file)
    local mod_with_hyphen=$(grep -m1 "\[[^]]*-[^]]*\]" /proc/dynamic_debug/control | sed -n 's/.*\[\(.*\)\].*/\1/p')

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
    verify_file_slice "$slice_pattern"
    local hash_hyphen=$(slice_and_hash_ddctrl "$slice_pattern")

    # 2. Disable and enable using underscore name, and record the state fingerprint
    ddcmd =_
    v_echo "#   trying underscore name: $mod_with_underscore"
    ddcmd "module $mod_with_underscore +p"
    verify_file_slice "$slice_pattern"
    local hash_underscore=$(slice_and_hash_ddctrl "$slice_pattern")

    # Real-time mathematical proof of hyphen/underscore name equivalence!
    if [ "$hash_hyphen" != "$hash_underscore" ]; then
        echo -e "${RED}: Hyphen/Underscore equivalence check failed! Fingerprints do not match.${NC}"
        echo -e "Hyphen name state hash:     $hash_hyphen"
        echo -e "Underscore name state hash: $hash_underscore"
        exit $ksft_fail
    else
        v_echo "${GREEN}: Proven: Hyphen/Underscore literal name equivalence matches!${NC}"
    fi

    # 3. Try kbasename with hyphen (if it has a path)
    if [ "$base_hyphen" != "$mod_with_hyphen" ]; then
        ddcmd =_
        v_echo "#   trying hyphen kbasename: $base_hyphen"
        ddcmd "module $base_hyphen +pmf"
        verify_file_slice "$slice_pattern"
        local hash_base_hyphen=$(slice_and_hash_ddctrl "$slice_pattern")

        # Prove kbasename hyphen name matches literal path hyphen name (with different flags)!
        v_echo "#   trying full path hyphen with pmf flags"
        ddcmd =_
        ddcmd "module $mod_with_hyphen +pmf"
        local hash_path_pmf=$(slice_and_hash_ddctrl "$slice_pattern")
        if [ "$hash_path_pmf" != "$hash_base_hyphen" ]; then
            echo -e "${RED}: Hyphen kbasename check failed! Fingerprints do not match full-path hyphen enablement.${NC}"
            exit $ksft_fail
        else
            v_echo "${GREEN}: Proven: Hyphen kbasename matches full-path hyphen enablement!${NC}"
        fi
    fi

    # 4. Try kbasename with underscore
    local base_underscore=$(echo "$base_hyphen" | tr '-' '_')
    ddcmd =_
    v_echo "#   trying underscore kbasename: $base_underscore"
    ddcmd "module $base_underscore +pmf"
    verify_file_slice "$slice_pattern"
    local hash_base_underscore=$(slice_and_hash_ddctrl "$slice_pattern")

    # Real-time mathematical proof of hyphen/underscore kbasename equivalence!
    if [ "$hash_base_hyphen" != "$hash_base_underscore" ] && [ -n "$hash_base_hyphen" ]; then
        echo -e "${RED}: Hyphen/Underscore kbasename equivalence check failed! Fingerprints do not match.${NC}"
        exit $ksft_fail
    elif [ -n "$hash_base_hyphen" ]; then
        v_echo "${GREEN}: Proven: Hyphen/Underscore kbasename equivalence matches!${NC}"
    fi

    ddcmd =_
}

# test parsing on spaces, commas. testing agains builtin [kernel/params]
function FT_comma_terminators {
    v_echo "${GREEN}# COMMA_TERMINATOR_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	return
    fi
    ddcmd "module params =_"

    # 1. Verify transition after commas-as-spaces query and splitting on @
    # Use non-printing decorator flags (+mf) to avoid print-enabling (+p) and prevent syslog pollution
    ddcmd "module,params,=_ @ module,params,+mf" 'kernel/params'

    # 2. Verify transition after ignored-commas query
    ddcmd ",module ,, ,  params, -p" 'kernel/params'

    # 3. Verify transition after quoted-commas query
    ddcmd " , module ,,, ,  params, -m" 'kernel/params'

    ddcmd =_
}

# testing classmap-based query enablers and class configurations
function FT_test_classes {
    v_echo "${GREEN}# TEST_CLASSES - classmap-based query enablers and class configs ${NC}"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_

    # 1. Verify initial multi-query enablement state via file slice
    modprobe test_dynamic_debug \
        dyndbg=class,D2_CORE,+pf@class,D2_KMS,+ps@class,D2_ATOMIC,+pm
    verify_file_slice '\[test_dynamic_debug\]'

    # 2. Verify state transition and live-printing end-to-end via ddcmd_load!
    ddcmd_load "class,D2_CORE,+pmf@class,D2_KMS,+pls@class,D2_ATOMIC,+pml" '\[test_dynamic_debug\]' \
        "/sys/module/test_dynamic_debug/parameters/do_classes" "1"

    ifrmmod test_dynamic_debug
}

function FT_classmap_inheritance {
    v_echo "${GREEN}# TEST_MOD_SUBMOD ${NC}"

    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_

    # modprobe with class enablements
    modprobe test_dynamic_debug \
	dyndbg=class,D2_CORE,+pf@class,D2_KMS,+pt@class,D2_ATOMIC,+pm

    verify_file_slice '\[test_dynamic_debug\]'

    modprobe test_dynamic_debug_submod
    verify_file_slice '\[test_dynamic_debug_submod\]'

    # Verify multi-module class control propagation and transition diff on a single line!
    ddcmd "class,D2_CORE,+pmf @ class,D2_KMS,+plt @ class,D2_ATOMIC,+pml" 'test_dynamic_debug' \
        "# add some prefixes"

    # now work the classmap-params
    # fresh start, to clear all above flags (test-fn limits)
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    modprobe test_dynamic_debug_submod # get supermod too

    echo 1 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 4 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( V1-3 + D2_CORE )
    verify_file_slice 'test_dynamic_debug'
    echo 3 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 0 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( D2_CORE, D2_DRIVER )
    verify_file_slice 'test_dynamic_debug'
    echo 0x16 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 0 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( D2_DRIVER, D2_KMS, D2_ATOMIC )
    verify_file_slice 'test_dynamic_debug'

    # recap DRM_USE_DYNAMIC_DEBUG regression
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    # set super-mod params at load-time
    modprobe test_dynamic_debug p_disjoint_bits=0x16 p_level_num=5
    verify_file_slice '\[test_dynamic_debug\]'
    modprobe test_dynamic_debug_submod
    # see them picked up by submod
    verify_file_slice 'test_dynamic_debug'

    # Real-time mathematical proof that load-time (modprobe) parameter parsing
    # and runtime (sysfs write) parameter configurations are perfectly equivalent!
    local hash_modprobe=$(slice_and_hash_ddctrl '\[test_dynamic_debug\]')

    # Fresh load with default parameters, then configure them dynamically at runtime
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    modprobe test_dynamic_debug
    modprobe test_dynamic_debug_submod
    echo 0x16 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 5 > /sys/module/test_dynamic_debug/parameters/p_level_num

    local hash_sysfs=$(slice_and_hash_ddctrl '\[test_dynamic_debug\]')
    if [ "$hash_modprobe" != "$hash_sysfs" ]; then
        echo -e "${RED}: Load-time vs runtime parameter equivalence check failed!${NC}"
        exit $ksft_fail
    else
        v_echo "${GREEN}: Proven: parameter load-time (modprobe) and runtime (sysfs write) are equivalent!${NC}"
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
    ddcmd =_
    local verbose

    # Enable logging on the builtin kernel parameter parsing engine with rich decorator context (+pmf)
    ddcmd "file kernel/params.c +pmf"

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
    FT_test_classes
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
    cat << 'EOF'
#K= 05551d1cf10c15288454425a8c7beffe FT_grammar_errs.1         dmesg
#K= 50c26fbc18f1d01e9a9f062b41a47f06 FT_grammar_errs.2         dmesg
#K= 5e21172610d0e15ce6eb28f47867fa3a FT_grammar_errs.3         dmesg
#K= fa956043d7899968a576139abdcce647 FT_grammar_errs.4         dmesg
#K= 9b36d50cdd8d803ef8a92c3e5234b6ef FT_grammar_errs.5         dmesg
#K= 3ba5e9ab3644e56c2ef7ee7d04a4f245 FT_grammar_errs.6         dmesg
#K= e7a1301d69970955afba8c7676cc3764 FT_grammar_errs.7         dmesg
#K= a6a2f13dd9d20b325bfa87acca85ff5f FT_grammar_errs.8         dmesg
#K= a78ee3aee5ff6fdf0b1c88b38ceea7b1 FT_grammar_errs.9         dmesg
#K= 44ef27b1923c45ca7008a305bc051265 FT_grammar_errs.10        dmesg
#K= ac6b46fdadb1f42ba8d983a870828377 FT_grammar_errs.11        dmesg
#K= 05551d1cf10c15288454425a8c7beffe FT_grammar_errs.12        dmesg
#K= 50c26fbc18f1d01e9a9f062b41a47f06 FT_grammar_errs.13        dmesg
#K= 5e21172610d0e15ce6eb28f47867fa3a FT_grammar_errs.14        dmesg
#K= fa956043d7899968a576139abdcce647 FT_grammar_errs.15        dmesg
#K= 9b36d50cdd8d803ef8a92c3e5234b6ef FT_grammar_errs.16        dmesg
#K= 3ba5e9ab3644e56c2ef7ee7d04a4f245 FT_grammar_errs.17        dmesg
#K= e7a1301d69970955afba8c7676cc3764 FT_grammar_errs.18        dmesg
#K= a6a2f13dd9d20b325bfa87acca85ff5f FT_grammar_errs.19        dmesg
#K= a78ee3aee5ff6fdf0b1c88b38ceea7b1 FT_grammar_errs.20        dmesg
#K= 44ef27b1923c45ca7008a305bc051265 FT_grammar_errs.21        dmesg
#K= ac6b46fdadb1f42ba8d983a870828377 FT_grammar_errs.22        dmesg
#K= fcebac1b1bd7ce6a70cc389228b5dee3 FT_grammar_errs.23        dmesg
#K= 22bd610aa0504072633877f791242133 FT_grammar_errs.24        dmesg
#K= 736b4fd5925b4e8dba3146687d70e4c4 FT_grammar_errs.25        dmesg
#K= e91fd483241e37660ee2ed277201455d FT_grammar_errs.26        dmesg
#K= bab1559fa0cd58f8e5366e3608d22042 FT_grammar_errs.27        dmesg
#K= eecc711e008b10a6fdb309271c14e555 FT_grammar_errs.28        dmesg
#K= 3ba71571a91e9440754b465ce915e55a FT_grammar_errs.29        dmesg
#K= d9bc269765a46783e63e3e1b613e6841 FT_grammar_errs.30        dmesg
#K= 4198eb3efd410da804e3bb801aab43ee FT_grammar_errs.31        dmesg
#K= d4ad5a645e7ae6c4f2b34bec72a76bc1 FT_grammar_errs.32        dmesg
#K= 797cf3002f6ab7cc03e6bd77902723e2 FT_grammar_errs.33        dmesg
#K= fcebac1b1bd7ce6a70cc389228b5dee3 FT_grammar_errs.34        dmesg
#K= 4f4d0188fe4f31929c989acc2430dd82 FT_grammar_errs.35        dmesg
#K= 101f8d438680ccdbb8cee352697efd1b FT_grammar_errs.36        dmesg
#K= 1a519f868d18cd3949a19c0c48442781 FT_grammar_errs.37        dmesg
#K= 9f968258643f37878b6bf90669513c5a FT_grammar_errs.38        dmesg
#K= 629d7145c4486661d83b4d7116115e60 FT_grammar_errs.39        dmesg
#K= 8356a85c3a27f0c05db0650b534cee1e FT_grammar_errs.40        dmesg
#K= 4f28c16204126709710c87c768319f64 FT_grammar_errs.41        dmesg
#K= 2845c75e4fb0b6ed58d65657015d1a66 FT_grammar_errs.42        dmesg
#K= ad173eab4c6caf41a77c3f35ee656090 FT_grammar_errs.43        dmesg
#K= 2d3af67031a3a53e27bafe8fc0b1943b FT_grammar_errs.44        dmesg
#K= 24d85e3b86f3d5f7640995922d91e08c FT_basic_queries.1       "\[kernel/params\]"
#K= 958898bcd9736a94e1f8b293b230a96a FT_basic_queries.2       "\[kernel/params\]"
#K= 130118da5a296e4039865d167e06cacd FT_basic_queries.3       "\[kernel/params\]"
#K= da6bd1c6a299290150668186f8263b82 FT_basic_queries.4       "\[kernel/params\]"
#K= 82572e8d20c4b567afac783006d1a935 FT_basic_queries.5       "kernel/params"
#K= baea1247680e8151c121539f4b90a6d8 FT_basic_queries.6       "\[kernel/params\]"
#K= 0b5cae527b414cdcc31a92eb31a27682 FT_path_module_queries.1 "\[init/main\]"
#K= 57e10cafef647d6b51e435fb430512cc FT_path_module_queries.2 "\[init/main\]"
#K= 0b5cae527b414cdcc31a92eb31a27682 FT_path_module_queries.3 "\[init/main\]"
#K= 57e10cafef647d6b51e435fb430512cc FT_path_module_queries.4 "\[init/main\]"
#K= 24d85e3b86f3d5f7640995922d91e08c FT_comma_terminators.1   "kernel/params"
#K= 68b329da9893e34099c7d8ad5cb9c940 FT_comma_terminators.2   "kernel/params"
#K= f85c977f05f152b368e0460d28ff241a FT_comma_terminators.3   "kernel/params"
#K= fad4898a43dc75a4132fa8cbaff8dc69 FT_hyphen_underscore.1   "\[[^]]*kvm-intel\]"
#K= fad4898a43dc75a4132fa8cbaff8dc69 FT_hyphen_underscore.2   "\[[^]]*kvm-intel\]"
#K= 55ed737cb7e33f2963f48ac11092e960 FT_hyphen_underscore.3   "\[[^]]*kvm-intel\]"
#K= 55ed737cb7e33f2963f48ac11092e960 FT_hyphen_underscore.4   "\[[^]]*kvm-intel\]"
#K= 48c19fd4f58019d5f5b7b6a2e74d78c7 FT_test_classes.1        "\[test_dynamic_debug\]"
#K= c2bed077cf647192be32cfea96b23909 FT_test_classes.2        "\[test_dynamic_debug\]"
#K= 8a491819b146f1360bbdcd90a3efe9ce FT_test_classes.3        dmesg
#K= 8ddbca66047a9bcb365c83ba86e64dff FT_classmap_inheritance.1 "\[test_dynamic_debug\]"
#K= 40547b5bf57fbe362e12b160f77a96e7 FT_classmap_inheritance.2 "\[test_dynamic_debug_submod\]"
#K= cc7321681d48292b22f2d7bc3d7713e3 FT_classmap_inheritance.3 "test_dynamic_debug"
#K= be22d46fe4a8535612ff6d985c4b9e57 FT_classmap_inheritance.4 "test_dynamic_debug"
#K= bcac079cef963e3e3cedd75c123fd618 FT_classmap_inheritance.5 "test_dynamic_debug"
#K= c6bd6a197fc1bbff7eac50b18c907bd4 FT_classmap_inheritance.6 "test_dynamic_debug"
#K= 89a2be5de17c0992c2226b3b5d74cf18 FT_classmap_inheritance.7 "\[test_dynamic_debug\]"
#K= d6cd17cc2be4aac80d0c94367f4c423e FT_classmap_inheritance.8 "test_dynamic_debug"
#K= 29d6c3d962400aa663d01a1ca665bc23 FT_classmap_inheritance.9 dmesg
#K= ffd3e80edada1d8df6f5d6b77cb38a6a FT_modprobe_w_param.1 dmesg
#K= a7cda12a021262cf0124e3bc10d3e94d FT_modprobe_w_param.2 dmesg
#K= f8298d680147fe5914a6ef3c254663fc FT_modprobe_w_param.3 dmesg
#K= f8d23f92a10096d33db2a38ebd7fcb1d FT_modprobe_w_param.4 dmesg
#K= 9cb2721f82677472659226fb7ef95098 FT_modprobe_w_param.5 dmesg
#K= 0678d5e814b4474875627dce63e43964 FT_modprobe_w_param.6 dmesg
#K= 5c53c2ddfb19fa749df743084e4d051d FT_modprobe_w_param.7 dmesg
#K= 164c28417569b8e6cc9bae8759966b27 FT_modprobe_w_param.8 dmesg
#K= 2f51606105bf023995d4547ca67531fc FT_modprobe_w_param.9 dmesg
#K= 7009082c1034b0b1cf6bd1c1399ce3d8 FT_modprobe_w_param.10 dmesg
#K= df3d719a422b86a114a2518450d58899 FT_modprobe_w_param.11 dmesg
#K= 583b16b7c58fc011a4d4999b8003f3c7 FT_modprobe_w_param.12 dmesg
#K= a8a3d4fba0b51aab208150740bd2c035 FT_modprobe_w_param.13 dmesg
#K= 302720d1b954f9b3eb32ff55e4f2dd17 FT_modprobe_w_param.14 dmesg
#K= acefd028d416091ed039182222be8861 FT_modprobe_w_param.15 dmesg
#K= 224a508324d382afbf77db8a270817c0 FT_modprobe_w_param.16 dmesg
#K= 68589cf1eecff15a9a94389c6228a7b9 FT_modprobe_w_param.17 dmesg
#K= b4b52c1d1a8502e97e0ce751e46a189e FT_modprobe_w_param.18 dmesg
#K= 283bf15ec1671b8c2aed4283315773ed FT_modprobe_w_param.19 dmesg
#K= f28efa0292285e187efe1b36f1cbe7c1 FT_modprobe_w_param.20 dmesg
#K= 1a6e7afd5177d0f6ec0578cbfe477037 FT_modprobe_w_param.21 dmesg
#K= fcbe98e4d7ee27eec1308972349bbe05 FT_modprobe_w_param.22 dmesg
#K= be86509e6909f4413f79587a3b063ca7 FT_modprobe_w_param.23 dmesg
#K= 5565705b190d463a3df3f7b4b5674675 FT_modprobe_w_param.24 dmesg
#K= 7810df168729bf476e680f719edde3cc FT_modprobe_w_param.25 dmesg
#K= 6022c25aae8ac6ea800faab97ad8cf04 FT_modprobe_w_param.26 dmesg
#K= 506477150439d20e381b01e6e0042395 FT_modprobe_w_param.27 dmesg
#K= 2a7f727133d0cde424149c9e5dd17bb9 FT_modprobe_w_param.28 dmesg
#K= 3012f34dea91d90ad2a2e8acaec19c33 FT_modprobe_w_param.29 dmesg
#K= 0b352204414caacc26d4ffed61068c43 FT_modprobe_w_param.30 dmesg
#K= 1a6e7afd5177d0f6ec0578cbfe477037 FT_modprobe_w_param.31 dmesg
#K= fcbe98e4d7ee27eec1308972349bbe05 FT_modprobe_w_param.32 dmesg
#K= 2ec5890ec9f681c5147950db67e64eee FT_modprobe_w_param.33 dmesg
#K= 32eda8ee886fb0d8974652f71e372b14 FT_modprobe_w_param.34 dmesg
#K= 57f378f1d44b4f0ae3995ebb8dcce7b6 FT_modprobe_w_param.35 dmesg
#K= 6022c25aae8ac6ea800faab97ad8cf04 FT_modprobe_w_param.36 dmesg
#K= 95fab5d5f011477a6efb833f4d1436df FT_modprobe_w_param.37 dmesg
#K= 4c052212da31fb1ae5a9ad1b636ea72a FT_modprobe_w_param.38 dmesg
#K= c1d611e3b7933efa7fd0cc4bf132c3ee FT_modprobe_w_param.39 dmesg
#K= 0b352204414caacc26d4ffed61068c43 FT_modprobe_w_param.40 dmesg
#K= f47d2d1985265967b59254358ae56ae4 FT_modprobe_w_param.41 dmesg
#K= ca9e704044febd3baf16b68c8f32bbd8 FT_modprobe_w_param.42 dmesg
#K= 8becd569cc2a4211ffb8cc1034a6291c FT_modprobe_w_param.43 dmesg
#K= b8e6ddb3dcf88759120de31400ba2f87 FT_modprobe_w_param.44 dmesg
#K= 50d2e8ecbd81199b59c23dbec6c9e911 FT_modprobe_w_param.45 dmesg
#K= 4f869a2fee6de4b12238488dcd0fae07 FT_modprobe_w_param.46 dmesg
#K= 146a1294f452e51c12837bbd3ea723fa FT_modprobe_w_param.47 dmesg
#K= 93f3b66ab8ae0aba6f1c1f8efb5a587e FT_modprobe_w_param.48 dmesg
#K= 13ba45ebc71ecb62080da61688ff4e77 FT_modprobe_w_param.49 dmesg
#K= 28205cb5152f53fc6c12a6a4844c5963 FT_modprobe_w_param.50 dmesg
EOF
}

# ==============================================================================
# Utilites, test primitives

# ==============================================================================
# Run tests

# Clear any stale seen/unregistered/drifted hashes from previous runs
rm -f "$SEEN_HASHES_FILE" "$UNREG_HASHES_FILE" "$DRIFT_HASHES_FILE"

ifrmmod test_dynamic_debug

# Check if loadable module support or our test modules are missing/builtin
LACK_TMOD=0
if [ -d "/sys/module/test_dynamic_debug" ]; then
    # If module is present but not in /proc/modules, it is a builtin module (cannot unload/reload)
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

# Output consolidated blocks of unregistered and drifted fingerprints at the absolute end of the run
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
    exit $ksft_fail
fi

exit $ksft_pass

