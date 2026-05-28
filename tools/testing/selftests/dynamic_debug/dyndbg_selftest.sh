#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

V=${V:=0}  # invoke as V=1 $0  for global verbose
RED="\033[0;31m"
GREEN="\033[0;32m"
YELLOW="\033[0;33m"
BLUE="\033[0;34m"
MAGENTA="\033[0;35m"
CYAN="\033[0;36m"
NC="\033[0;0m"
error_msg=""

# Standard kselftest exit codes
ksft_pass=0
ksft_fail=1
ksft_skip=4

[ -e /proc/dynamic_debug/control ] || {
    echo -e "${RED}: this test requires CONFIG_DYNAMIC_DEBUG=y ${NC}"
    exit $ksft_skip # nothing to test here, no good reason to fail.
}

# need info to avoid failures due to untestable configs

[ -f "$KCONFIG_CONFIG" ] || KCONFIG_CONFIG=".config"
if [ -f "$KCONFIG_CONFIG" ]; then
    echo "# consulting KCONFIG_CONFIG: $KCONFIG_CONFIG"
    grep -q "CONFIG_DYNAMIC_DEBUG=y" $KCONFIG_CONFIG ; LACK_DD_BUILTIN=$?
    grep -q "CONFIG_TEST_DYNAMIC_DEBUG=m" $KCONFIG_CONFIG ; LACK_TMOD=$?
    grep -q "CONFIG_TEST_DYNAMIC_DEBUG_SUBMOD=m" $KCONFIG_CONFIG ; LACK_TMOD_SUBMOD=$?
else
    # if no config, try runtime probes
    modprobe -n test_dynamic_debug 2>/dev/null ; LACK_TMOD=$?
    modprobe -n test_dynamic_debug_submod 2>/dev/null ; LACK_TMOD_SUBMOD=$?
    # assume builtin dyndbg if control exists (checked above)
    LACK_DD_BUILTIN=0
fi

function vx () {
    echo "$1" > /sys/module/dynamic_debug/parameters/verbose
}

function ddgrep () {
    grep "$1" /proc/dynamic_debug/control
}

function doprints () {
    cat /sys/module/test_dynamic_debug/parameters/do_prints
}

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
    output=$( (echo "$args" > /proc/dynamic_debug/control) 2>&1)
    exit_code=$?
    error_msg=$(echo "$output" | cut -d ":" -f 5 | sed -e 's/^[[:space:]]*//')
    handle_exit_code $BASH_LINENO $FUNCNAME $exit_code $exp_exit_code
}

function handle_exit_code() {
    local exp_exit_code=0
    [ $# == 4 ] && exp_exit_code=$4
    if [ $3 -ne $exp_exit_code ]; then
        echo -e "${RED}: $BASH_SOURCE:$1 $2() expected to exit with code $exp_exit_code, got $3"
	[ $3 == 1 ] && echo "Error: '$error_msg'"
        exit $ksft_fail
    fi
}

# $1 - pattern to match, pattern in $1 is enclosed by spaces for a match ""\s$1\s"
# $2 - number of times the pattern passed in $1 is expected to match
# $3 - optional can be set either to "-r" or "-v"
#       "-r" means relaxed matching in this case pattern provided in $1 is passed
#       as is without enclosing it with spaces
#       "-v" prints matching lines
# $4 - optional when $3 is set to "-r" then $4 can be used to pass "-v"
function check_match_ct {
    pattern="\s$1\s"
    exp_cnt=0

    [ "$3" == "-r" ] && pattern="$1"
    let cnt=$(ddgrep "$pattern" | wc -l)
    if [ $V -eq 1 ] || [ "$3" == "-v" ] || [ "$4" == "-v" ]; then
        echo -ne "${BLUE}" && ddgrep "$pattern" && echo -ne "${NC}"
    fi
    [ $# -gt 1 ] && exp_cnt=$2
    if [ $cnt -ne $exp_cnt ]; then
        echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO check failed expected $exp_cnt on $1, got $cnt"
        exit $ksft_fail
    else
        echo ": $cnt matches on $1"
    fi
}

# $1 - trace instance name
# #2 - if > 0 then directory is expected to exist, if <= 0 then otherwise
# $3 - "-v" for verbose
function check_trace_instance_dir {
    if [ -e /sys/kernel/tracing/instances/$1 ]; then
        if [ "$3" == "-v" ] ; then
            echo "ls -l /sys/kernel/tracing/instances/$1: "
            ls -l /sys/kernel/tracing/instances/$1
        fi
	if [ $2 -le 0 ]; then
            echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO error trace instance \
		    '/sys/kernel/tracing/instances/$1' does exist"
	    exit $ksft_fail
	fi
    else
	if [ $2 -gt 0 ]; then
            echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO error trace instance \
		    '/sys/kernel/tracing/instances/$1' does not exist"
	    exit $ksft_fail
        fi
    fi
}

function tmark {
    echo $* > /sys/kernel/tracing/trace_marker
}

# $1 - trace instance name
# $2 - line number
# $3 - if > 0 then the instance is expected to be opened, otherwise
# the instance is expected to be closed
function check_trace_instance {
    output=$(tail -n9 /proc/dynamic_debug/control | grep ": Opened trace instances" \
	    | xargs -n1 | grep $1)
    if [ "$output" != $1 ] && [ $3 -gt 0 ]; then
        echo -e "${RED}: $BASH_SOURCE:$2 trace instance $1 is not opened"
        exit $ksft_fail
    fi
    if [ "$output" == $1 ] && [ $3 -le 0 ]; then
        echo -e "${RED}: $BASH_SOURCE:$2 trace instance $1 is not closed"
        exit $ksft_fail
    fi
}

function is_trace_instance_opened {
    check_trace_instance $1 $BASH_LINENO 1
}

function is_trace_instance_closed {
    check_trace_instance $1 $BASH_LINENO 0
}

# $1 - trace instance directory to delete
# $2 - if > 0 then directory is expected to be deleted successfully, if <= 0 then otherwise
function del_trace_instance_dir() {
    exp_exit_code=1
    [ $2 -gt 0 ] && exp_exit_code=0
    output=$( (rmdir /sys/kernel/tracing/instances/$1) 2>&1)
    exit_code=$?
    error_msg=$(echo "$output" | cut -d ":" -f 3 | sed -e 's/^[[:space:]]*//')
    handle_exit_code $BASH_LINENO $FUNCNAME $exit_code $exp_exit_code
}

function error_log_ref {
    # to show what I got
    : echo "# error-log-ref: $1"
    : echo cat \$2
}

function ifrmmod {
    lsmod | grep $1 2>&1>/dev/null && rmmod $1
}

# $1 - text to search for
function search_trace() {
    search_trace_name 0 1 $1
}

# $1 - trace instance name, 0 for global event trace
# $2 - line number counting from the bottom
# $3 - text to search for
function search_trace_name() {
	if [ "$1" = "0" ]; then
	    buf=$(cat /sys/kernel/tracing/trace)
	    line=$(tail -$2 /sys/kernel/tracing/trace | head -1 | sed -e 's/^[[:space:]]*//')
	else
	    buf=$(cat /sys/kernel/tracing/instances/$1/trace)
	    line=$(tail -$2 /sys/kernel/tracing/instances/$1/trace | head -1 | \
		   sed -e 's/^[[:space:]]*//')
	fi
	if [ $2 = 0 ]; then
	    # whole-buf check
	    output=$(echo "$buf" | grep "$3")
	else
	    output=$(echo "$line" | grep "$3")
	fi
	if [ "$output" = "" ]; then
            echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO search for '$3' failed \
		    in line '$line' or '$buf'"
	    exit $ksft_fail
	fi
	if [ $V = 1 ]; then
	    echo -e "${MAGENTA}: search_trace_name in $1 found: \n$output \nin:${BLUE} $buf ${NC}"
        fi
}

# $1 - error message to check
function check_err_msg() {
    if [ "$error_msg" != "$1" ]; then
        echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO error message '$error_msg' \
		does not match with '$1'"
        exit $ksft_fail
    fi
}

function basic_tests {
    echo -e "${GREEN}# BASIC_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	return
    fi
    ddcmd =_ # zero everything
    check_match_ct =p 0

    # module params are builtin to handle boot args
    check_match_ct '\[kernel/params\]' 4 -r
    ddcmd module params +mpf
    check_match_ct =pmf 4

    # multi-cmd input, newline separated, with embedded comments
    cat <<"EOF" > /proc/dynamic_debug/control
      module params =_				# clear params
      module params +mf				# set flags
      module params func parse_args +sl		# other flags
EOF
    check_match_ct =mf 3
    check_match_ct =mfsl 1
    ddcmd =_
}

function comma_terminator_tests {
    echo -e "${GREEN}# COMMA_TERMINATOR_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP - test requires params, which is a builtin module"
	return
    fi
    # try combos of spaces & commas
    check_match_ct '\[kernel/params\]' 4 -r
    ddcmd module,params,=_		# commas as spaces
    ddcmd module,params,+mpf		# turn on module's pr-debugs
    check_match_ct =pmf 4
    ddcmd ,module ,, ,  params, -p
    check_match_ct =mf 4
    ddcmd " , module ,,, ,  params, -m"	#
    check_match_ct =f 4
    ddcmd =_
}

function test_multiquery_splitting {
    echo -e "${GREEN}# TEST_MULTIQUERY_SPLITTING - multi-command splitting on @ ${NC}"
    if [ $LACK_TMOD -eq 1 ]; then
	echo "SKIP - test requires test-dynamic-debug.ko"
	return
    fi
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_
    modprobe test_dynamic_debug dyndbg=class,D2_CORE,+pf@class,D2_KMS,+pt@class,D2_ATOMIC,+pm
    check_match_ct =pf 1
    check_match_ct =pt 1
    check_match_ct =pm 1
    check_match_ct test_dynamic_debug 23 -r
    # add flags to those callsites
    ddcmd class,D2_CORE,+mf@class,D2_KMS,+lt@class,D2_ATOMIC,+ml
    check_match_ct =pmf 1
    check_match_ct =plt 1
    check_match_ct =pml 1
    check_match_ct test_dynamic_debug 23 -r
    ifrmmod test_dynamic_debug
}

function test_mod_submod {
    echo -e "${GREEN}# TEST_MOD_SUBMOD ${NC}"
    if [ $LACK_TMOD -eq 1 ]; then
	echo "SKIP - test requires test-dynamic-debug.ko"
	return
    fi
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_

    # modprobe with class enablements
    modprobe test_dynamic_debug \
	dyndbg=class,D2_CORE,+pf@class,D2_KMS,+pt@class,D2_ATOMIC,+pm

    check_match_ct '\[test_dynamic_debug\]' 23 -r
    check_match_ct =pf 1
    check_match_ct =pt 1
    check_match_ct =pm 1

    modprobe test_dynamic_debug_submod
    check_match_ct test_dynamic_debug_submod 23 -r
    check_match_ct '\[test_dynamic_debug\]' 23 -r
    check_match_ct test_dynamic_debug 46 -r

    # no enablements propagate here
    check_match_ct =pf 1
    check_match_ct =pt 1
    check_match_ct =pm 1

    # change classes again, this time submod too
    ddcmd class,D2_CORE,+mf@class,D2_KMS,+lt@class,D2_ATOMIC,+ml "# add some prefixes"
    check_match_ct =pmf 1
    check_match_ct =plt 1
    check_match_ct =pml 1
    #  submod changed too
    check_match_ct =mf 1
    check_match_ct =lt 1
    check_match_ct =ml 1

    # now work the classmap-params
    # fresh start, to clear all above flags (test-fn limits)
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    modprobe test_dynamic_debug_submod # get supermod too

    echo 1 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 4 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( V1-3 + D2_CORE )
    check_match_ct =p 8
    echo 3 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 0 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( D2_CORE, D2_DRIVER )
    check_match_ct =p 4
    echo 0x16 > /sys/module/test_dynamic_debug/parameters/p_disjoint_bits
    echo 0 > /sys/module/test_dynamic_debug/parameters/p_level_num
    # 2 mods * ( D2_DRIVER, D2_KMS, D2_ATOMIC )
    check_match_ct =p 6

    # recap DRM_USE_DYNAMIC_DEBUG regression
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    # set super-mod params
    modprobe test_dynamic_debug p_disjoint_bits=0x16 p_level_num=5
    check_match_ct =p 7
    modprobe test_dynamic_debug_submod
    # see them picked up by submod
    check_match_ct =p 14
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
}

function test_slash_queries {
    echo -e "${GREEN}# TEST_SLASH_QUERIES ${NC}"
    ddcmd =_

    # Find how many 'main' modules we have in total (by basename)
    # Use a more precise regex to avoid false positives like [irqdomain]
    local total_main=$(grep -c "\[\([^]]*/\)\?main\]" /proc/dynamic_debug/control)
    echo "# found $total_main total 'main' modules"

    if [ $total_main -eq 0 ]; then
        echo "SKIP - no 'main' modules found to test slashes"
        return
    fi

    echo "# testing 'module */main'"
    ddcmd module "*/main" +p
    # This should match modules that HAVE a slash and end in /main
    local slash_main=$(grep -c "\[[^]]*/main\]" /proc/dynamic_debug/control)
    check_match_ct =p $slash_main -r
    
    echo "# testing 'module init/main' (specific path)"
    ddcmd =_
    ddcmd module "init/main" +p
    local init_main=$(grep -c "\[init/main\]" /proc/dynamic_debug/control)
    check_match_ct =p $init_main -r

    echo "# testing 'module main' (basename match)"
    ddcmd =_
    ddcmd module main +p
    # This should match ALL $total_main entries due to kbasename matching
    check_match_ct =p $total_main -r

    ddcmd =_
}

tests_list=(
    basic_tests
    test_slash_queries
    # these require test_dynamic_debug*.ko
    comma_terminator_tests
    test_multiquery_splitting
    test_mod_submod
)

# Run tests

ifrmmod test_dynamic_debug_submod
ifrmmod test_dynamic_debug

for test in "${tests_list[@]}"
do
    $test
    echo ""
done
echo -en "${GREEN}# Done on: "
date
echo -en "${NC}"

exit $ksft_pass
