# SPDX-License-Identifier: GPL-2.0-only
#!/bin/bash

V=${V:=0}  # invoke as V=1 $0  for global verbose

RED="\033[0;31m"
GREEN="\033[0;32m"
YELLOW="\033[0;33m"
BLUE="\033[0;34m"
MAGENTA="\033[0;35m"
CYAN="\033[0;36m"
NC="\033[0;0m"
error_msg=""

[ -e /proc/dynamic_debug/control ] || {
    echo -e "${RED}: this test requires CONFIG_DYNAMIC_DEBUG=y ${NC}"
    exit 0 # nothing to test here, no good reason to fail.
}

# need info to avoid failures due to untestable configs

[ -f "$KCONFIG_CONFIG" ] || KCONFIG_CONFIG=".config"
if [ -f "$KCONFIG_CONFIG" ]; then
    echo "# consulting KCONFIG_CONFIG: $KCONFIG_CONFIG"
    grep -q "CONFIG_DYNAMIC_DEBUG=y" $KCONFIG_CONFIG ; LACK_DD_BUILTIN=$?
    grep -q "CONFIG_TEST_DYNAMIC_DEBUG=m" $KCONFIG_CONFIG ; LACK_TMOD=$?
    grep -q "CONFIG_TEST_DYNAMIC_DEBUG_SUBMOD=m" $KCONFIG_CONFIG ; LACK_TMOD_SUBMOD=$?
    if [ $V -eq 1 ]; then
	echo LACK_DD_BUILTIN: $LACK_DD_BUILTIN
	echo LACK_TMOD: $LACK_TMOD
	echo LACK_TMOD_SUBMOD: $LACK_TMOD_SUBMOD
    fi
else
    LACK_DD_BUILTIN=0
    LACK_TMOD=0
    LACK_TMOD_SUBMOD=0
fi

function vx () {
    echo $1 > /sys/module/dynamic_debug/parameters/verbose
}

function ddgrep () {
    grep $1 /proc/dynamic_debug/control
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
    output=$((echo "$args" > /proc/dynamic_debug/control) 2>&1)
    exit_code=$?
    error_msg=$(echo $output | cut -d ":" -f 5 | sed -e 's/^[[:space:]]*//')
    handle_exit_code $BASH_LINENO $FUNCNAME $exit_code $exp_exit_code
}

function handle_exit_code() {
    local exp_exit_code=0
    [ $# == 4 ] && exp_exit_code=$4
    if [ $3 -ne $exp_exit_code ]; then
        echo -e "${RED}: $BASH_SOURCE:$1 $2() expected to exit with code $exp_exit_code"
	[ $3 == 1 ] && echo "Error: '$error_msg'"
        exit
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
        echo -ne "${BLUE}" && ddgrep $pattern && echo -ne "${NC}"
    fi
    [ $# -gt 1 ] && exp_cnt=$2
    if [ $cnt -ne $exp_cnt ]; then
        echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO check failed expected $exp_cnt on $1, got $cnt"
        exit
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
	    exit
	fi
    else
	if [ $2 -gt 0 ]; then
            echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO error trace instance \
		    '/sys/kernel/tracing/instances/$1' does not exist"
	    exit
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
        exit
    fi
    if [ "$output" == $1 ] && [ $3 -le 0 ]; then
        echo -e "${RED}: $BASH_SOURCE:$2 trace instance $1 is not closed"
        exit
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
    output=$((rmdir /sys/kernel/debug/tracing/instances/$1) 2>&1)
    exit_code=$?
    error_msg=$(echo $output | cut -d ":" -f 3 | sed -e 's/^[[:space:]]*//')
    handle_exit_code $BASH_LINENO $FUNCNAME $exit_code $exp_exit_code
}

function error_log_ref {
    # to show what I got
    : echo "# error-log-ref: $1"
    : echo cat \$2
}

function ifrmmod {
    lsmod | grep $1 2>&1>/dev/null || ([ "$2" == "-v" ] && echo "module '$1' is not loaded")
    lsmod | grep $1 2>&1>/dev/null && rmmod $1 && [ "$2" == "-v" ] && echo "unload module '$1'"
}

# $1 - text to search for
function search_trace() {
    search_trace_name 0 1 $1
}

# $1 - trace instance name, 0 for global event trace
# $2 - line number counting from the bottom
# $3 - text to search for
# $4 - optional -v to see verbose results
function search_trace_name() {
	if [ "$1" = "0" ]; then
	    buf=$(cat /sys/kernel/debug/tracing/trace)
	    line=$(tail -$2 /sys/kernel/debug/tracing/trace | head -1 | sed -e 's/^[[:space:]]*//')
	else
	    buf=$(cat /sys/kernel/debug/tracing/instances/$1/trace)
	    line=$(tail -$2 /sys/kernel/debug/tracing/instances/$1/trace | head -1 | \
		   sed -e 's/^[[:space:]]*//')
	fi
	if [ $2 = 0 ]; then
	    # whole-buf check
	    output=$(echo $buf | grep "$3")
	else
	    output=$(echo $line | grep "$3")
	fi
	if [ "$output" = "" ]; then
            echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO search for '$3' failed \
		    in line '$line' or '$buf'"
	    exit
	fi
	if [[ "$4" == "-v" || $V = 1 ]]; then
	    echo -e "${MAGENTA}: search_trace_name in $1 found: \n$output \nin:${BLUE} $buf ${NC}"
        fi
}

# $1 - error message to check
function check_err_msg() {
    if [ "$error_msg" != "$1" ]; then
        echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO error message '$error_msg' \
		does not match with '$1'"
        exit
    fi
}

# $1 - default destination to check
function check_default_dst() {
    dst=$(tail -50 /proc/dynamic_debug/control | grep "#: Default trace destination" | \
	  cut -d':' -f3 | sed -e 's/^[[:space:]]*//')
    if [ "$dst" != "$1" ]; then
        echo -e "${RED}: $BASH_SOURCE:$BASH_LINENO default dest '$dst' does not match with '$1'"
        exit
    fi
}

function basic_tests {
    echo -e "${GREEN}# BASIC_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP"
	return
    fi
    ddcmd =_ # zero everything (except class'd sites)
    check_match_ct =p 0
    # there are several main's :-/
    ddcmd module main file */module/main.c +p
    check_match_ct =p 14
    ddcmd =_
    check_match_ct =p 0
    # multi-cmd input, newline separated, with embedded comments
    cat <<"EOF" > /proc/dynamic_debug/control
      module main +mf                   # multi-query
      module main file init/main.c +ml  # newline separated
EOF
    # the intersection of all those main's is hard to track/count
    # esp when mixed with overlapping greps
    check_match_ct =mf 21
    check_match_ct =ml 0
    check_match_ct =mfl 6
    ddcmd =_
}

function comma_terminator_tests {
    echo -e "${GREEN}# COMMA_TERMINATOR_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP"
	return
    fi
    # try combos of spaces & commas
    check_match_ct '\[params\]' 4 -r
    ddcmd module,params,=_		# commas as spaces
    ddcmd module,params,+mpf		# turn on module's pr-debugs
    check_match_ct =pmf 4
    ddcmd ,module ,, ,  params, -p
    check_match_ct =mf 4
    ddcmd " , module ,,, ,  params, -m"	#
    check_match_ct =f 4
    ddcmd =_
}

function test_percent_splitting {
    echo -e "${GREEN}# TEST_PERCENT_SPLITTING - multi-command splitting on % ${NC}"
    if [ $LACK_TMOD -eq 1 ]; then
	echo "SKIP"
	return
    fi
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_
    modprobe test_dynamic_debug dyndbg=class,D2_CORE,+pf%class,D2_KMS,+pt%class,D2_ATOMIC,+pm
    check_match_ct =pf 1
    check_match_ct =pt 1
    check_match_ct =pm 1
    check_match_ct test_dynamic_debug 23 -r
    # add flags to those callsites
    ddcmd class,D2_CORE,+mf%class,D2_KMS,+lt%class,D2_ATOMIC,+ml
    check_match_ct =pmf 1
    check_match_ct =plt 1
    check_match_ct =pml 1
    check_match_ct test_dynamic_debug 23 -r
    ifrmmod test_dynamic_debug
}

function test_mod_submod {
    echo -e "${GREEN}# TEST_MOD_SUBMOD ${NC}"
    if [ $LACK_TMOD -eq 1 ]; then
	echo "SKIP"
	return
    fi
    if [ $LACK_TMOD_SUBMOD -eq 1 ]; then
	echo "SKIP"
	return
    fi
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug
    ddcmd =_

    # modprobe with class enablements
    modprobe test_dynamic_debug \
	dyndbg=class,D2_CORE,+pf%class,D2_KMS,+pt%class,D2_ATOMIC,+pm

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
    ddcmd class,D2_CORE,+mf%class,D2_KMS,+lt%class,D2_ATOMIC,+ml "# add some prefixes"
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

# test verifies different combinations of flags and trace destination
function test_flags {
    echo -e "${GREEN}# TEST_FLAGS ${NC}"

    modprobe test_dynamic_debug dyndbg=+Tlm
    check_match_ct =Tml 23 -v

    ddcmd open selftest
    check_trace_instance_dir selftest 1
    is_trace_instance_opened selftest

    # invalid combinations of flags and trace destination
    ddcmd module test_dynamic_debug =Tm:0 fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =Tm:0. fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =T:m.:0 fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =T:m.:0. fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =:0lT fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =:0lT. fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =:0.lm:0 fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =:0.lmT. fail
    check_err_msg "Invalid argument"

    ddcmd module test_dynamic_debug =Tm:selftest fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =Tm:selftest. fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =T:m.:selftest fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =T:m.:selftest. fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =:selftestlT fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =:selftestlT. fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =:selftest.lm:0 fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =:selftest.lmT fail
    check_err_msg "Invalid argument"

    ddcmd module test_dynamic_debug =Tl.m fail
    check_err_msg "Invalid argument"
    ddcmd module test_dynamic_debug =T.lm fail
    check_err_msg "Invalid argument"

    # valid combinations of flags and trace destination
    ddcmd module test_dynamic_debug =p
    check_match_ct =p 23 -v
    ddcmd module test_dynamic_debug =T
    check_match_ct =T:selftest 23 -v
    ddcmd module test_dynamic_debug =_
    check_match_ct =:selftest 23 -v

    ddcmd module test_dynamic_debug =T:0
    check_match_ct =T 23 -v
    ddcmd module test_dynamic_debug -_
    check_match_ct =T 23 -v
    ddcmd module test_dynamic_debug =T:0.mf
    check_match_ct =Tmf 23 -v
    ddcmd module test_dynamic_debug =T:selftest
    check_match_ct =T:selftest 23 -v
    ddcmd module test_dynamic_debug =T:selftest.mf
    check_match_ct =T:selftest.mf 23 -v
    ddcmd module test_dynamic_debug =_:selftest
    check_match_ct =:selftest 23 -v

    ddcmd module test_dynamic_debug =:0
    ddcmd module test_dynamic_debug =:selftest
    check_match_ct =:selftest 23 -v
    ddcmd module test_dynamic_debug =p:selftest
    check_match_ct =p:selftest 23 -v
    ddcmd module test_dynamic_debug +_
    check_match_ct =p:selftest 23 -v

    ddcmd module test_dynamic_debug =T:selftest.mlf
    ddcmd module test_dynamic_debug =:0
    check_match_ct =Tmfl 23 -v
    ddcmd module test_dynamic_debug =:selftest
    check_match_ct =T:selftest.mfl 23 -v
    ddcmd module test_dynamic_debug =:0
    check_match_ct =Tmfl 23 -v
    ddcmd module test_dynamic_debug =_:selftest
    check_match_ct =:selftest 23 -v

    ddcmd module test_dynamic_debug =:0.

    ddcmd close selftest
    is_trace_instance_closed selftest
    ifrmmod test_dynamic_debug
}

# test verifies default destination
function test_default_destination {
    echo -e "${GREEN}# TEST_DEFAULT_DESTINATION ${NC}"

    check_default_dst 0
    modprobe test_dynamic_debug

    ddcmd class,D2_CORE,+T	# default dest is 0
    check_match_ct =T 1 -v

    ddcmd open foo		# foo becomes default dest
    is_trace_instance_opened foo
    check_trace_instance_dir foo 1
    check_default_dst foo

    ddcmd class,D2_CORE,+T	# default dest is foo
    check_match_ct =T:foo 1 -v

    ddcmd open,0		# reopening sets default dest to 0
    check_default_dst 0

    ddcmd class,D2_CORE,-T
    check_match_ct =:foo 1 -v

    ddcmd class,D2_CORE,+T      # default dest is 0 but since callsite was already labelled
                                # then reuse label
    check_match_ct =T:foo 1 -v

    ddcmd open bar		# bar becomes default dest
    is_trace_instance_opened bar
    check_trace_instance_dir bar 1
    check_default_dst bar

    ddcmd class,D2_KMS,+T	# default dest is bar
    check_match_ct =T:bar 1 -v

    ddcmd class,D2_KMS,+T:0	# set 0 dest explicitly
    check_match_ct =T 1 -v

    ddcmd class,D2_KMS,-T

    ddcmd open,foo		# reopening sets default dest to foo
    check_default_dst foo

    ddcmd class,D2_KMS,+T       # default dest is 0 but since callsite was already labelled
                                # then reuse label
    check_match_ct =T:foo 2 -v

    ddcmd "class D2_CORE -T:0"
    ddcmd "class D2_KMS -T:0"
    check_default_dst foo

    ddcmd close foo
    is_trace_instance_closed foo
    check_default_dst 0         # after closing foo which was default dest we revert
                                # to 0 as default dest

    ddcmd close bar
    is_trace_instance_closed bar
    check_default_dst 0

    ifrmmod test_dynamic_debug
}

function test_actual_trace {
    echo -e "${GREEN}# TEST_ACTUAL_TRACE ${NC}"
    ddcmd =_
    echo > /sys/kernel/tracing/trace
    echo 1 >/sys/kernel/tracing/tracing_on
    echo 1 >/sys/kernel/tracing/events/dyndbg/enable
    modprobe test_dynamic_debug dyndbg=class,D2_CORE,+T:0
    search_trace "D2_CORE msg"
    search_trace_name 0 1 "D2_CORE msg"
    check_match_ct =T 1
    tmark "here comes the WARN"
    search_trace "here comes the WARN"
    doprints
    search_trace "D2_CORE msg"
    ifrmmod test_dynamic_debug
}

function self_start {
    echo \# open, modprobe +T:selftest
    ddcmd open selftest
    check_trace_instance_dir selftest 1
    is_trace_instance_opened selftest
    modprobe test_dynamic_debug dyndbg=+T:selftest.mf
    check_match_ct =T:selftest.mf 5
}

function self_end_normal {
    echo \# disable -T:selftest, rmmod, close
    ddcmd module test_dynamic_debug -T:selftest # leave mf
    check_match_ct =:selftest.mf 5 -v
    ddcmd module test_dynamic_debug +:0
    ddcmd close selftest
    is_trace_instance_closed selftest
    ifrmmod test_dynamic_debug
}

function self_end_disable_anon {
    echo \# disable, close, rmmod
    ddcmd module test_dynamic_debug -T
    check_match_ct =:selftest.mf 5
    ddcmd module test_dynamic_debug +:0
    ddcmd close selftest
    is_trace_instance_closed selftest
    ifrmmod test_dynamic_debug
}

function self_end_disable_anon_mf {
    echo \# disable, close, rmmod
    ddcmd module test_dynamic_debug -Tf
    check_match_ct =:selftest.m 5
    ddcmd module test_dynamic_debug +:0
    ddcmd close selftest
    is_trace_instance_closed selftest
    ifrmmod test_dynamic_debug
}

function self_end_nodisable {
    echo \# SKIPPING: ddcmd module test_dynamic_debug -T:selftest
    ddcmd close selftest fail # close fails because selftest is still being used
    check_err_msg "Device or resource busy"
    check_match_ct =T:selftest.mf 5
    rmmod test_dynamic_debug
    ddcmd close selftest # now selftest can be closed because rmmod removed
                         # all callsites which were using it
    is_trace_instance_closed selftest
}

function self_end_delete_directory {
    del_trace_instance_dir selftest 0
    check_err_msg "Device or resource busy"
    ddcmd module test_dynamic_debug -mT:selftest
    check_match_ct =:selftest.f 5
    del_trace_instance_dir selftest 0
    check_err_msg "Device or resource busy"
    ddcmd module test_dynamic_debug +:0
    ddcmd close selftest
    check_trace_instance_dir selftest 1
    is_trace_instance_closed selftest
    del_trace_instance_dir selftest 1
    check_trace_instance_dir selftest 0
}

function test_early_close () {
    ddcmd open kparm_stream
    ddcmd module usbcore +T:kparm_stream.mf
    check_match_ct =T:usb_stream.mf 161
    echo ":not-running # ddcmd module usbcore -T:kparm_stream.mf"
    ddcmd close kparm_stream
}

function self_test_ {
    echo "# SELFTEST $1"
    self_start
    self_end_$1
}

function cycle_tests_normal {
    echo -e "${GREEN}# CYCLE_TESTS_NORMAL ${NC}"
    self_test_ normal           # ok
    self_test_ disable_anon     # ok
    self_test_ normal           # ok
    self_test_ disable_anon_mf  # ok
}

function cycle_not_best_practices {
    echo -e "${GREEN}# CYCLE_TESTS_PROBLEMS ${NC}"
    self_test_ nodisable
    self_test_ normal
    self_test_ delete_directory
}

# test verifies proper life cycle - open, enable:named, disable:named, close
function test_private_trace_simple_proper {
    echo -e "${GREEN}# TEST_PRIVATE_TRACE_1 ${NC}"

    ddcmd open kparm_stream
    ddcmd module params +T:kparm_stream.mf
    check_match_ct =T:kparm_stream.mf 4

    ddcmd module params -T:kparm_stream.mf
    check_match_ct =T:kparm_stream.mf 0
    is_trace_instance_opened kparm_stream
    ddcmd module params +:0
    ddcmd close kparm_stream
    is_trace_instance_closed kparm_stream

    ddcmd =_
    check_trace_instance_dir kparm_stream 1
    is_trace_instance_closed kparm_stream
    del_trace_instance_dir kparm_stream 1
    check_trace_instance_dir kparm_stream 0
}

# test verifies new syntax and close attempt of trace instance which is still in use
function test_private_trace_syntax_close_in_use {
    echo -e "${GREEN}# TEST_PRIVATE_SYNTAX_CLOSE_IN_USE ${NC}"
    ddcmd =_
    echo > /sys/kernel/tracing/trace
    echo 1 >/sys/kernel/tracing/tracing_on
    echo 1 >/sys/kernel/tracing/events/dyndbg/enable

    ddcmd open foo
    is_trace_instance_opened foo
    check_trace_instance_dir foo 1

    modprobe test_dynamic_debug
    ddcmd class,D2_CORE,+T:foo.l,%class,D2_KMS,+fT:foo.ml	# test new syntax
    check_match_ct =T:foo.l 1
    check_match_ct =T:foo.mfl 1

    tmark test_private_trace_syntax_close_in_use about to do_prints
    search_trace "test_private_trace_syntax_close_in_use about to do_prints"
    search_trace_name "0" 1 "test_private_trace_syntax_close_in_use about to do_prints"

    doprints
    ddcmd class,D2_CORE,-T:0
    ddcmd close foo fail	# close fails because foo is still being used
    check_err_msg "Device or resource busy"	# verify error message

    ddcmd class,D2_KMS,-T:0
    ddcmd close foo	# now close succeeds
    check_trace_instance_dir foo 1	# verify trace instance foo dir exists
    is_trace_instance_closed foo	# verify trace instance foo is closed

    ddcmd close bar fail	# try to close trace instance bar which is not opened
    check_err_msg "No such file or directory"	# verify error message

    ifrmmod test_dynamic_debug
    search_trace_name foo 2 "D2_CORE msg"
    search_trace_name foo 1 "D2_KMS msg"
    del_trace_instance_dir foo 1	# delete trace instance foo dir
    check_trace_instance_dir foo 0	# verify trace instance foo dir does not exist
}

# test verifies new syntax and removal of module
function test_private_trace_syntax_rmmod {
    echo -e "${GREEN}# TEST_PRIVATE_TRACE_SYNTAX_RMMOD ${NC}"
    ddcmd =_
    echo > /sys/kernel/tracing/trace
    echo 1 >/sys/kernel/tracing/tracing_on
    echo 1 >/sys/kernel/tracing/events/dyndbg/enable

    ddcmd open foo \; open bar	# open foo and bar trace instances and verify they are opened
    is_trace_instance_opened foo
    is_trace_instance_opened bar

    modprobe test_dynamic_debug	# load module and test new syntax
    ddcmd class,D2_CORE,+T:foo
    ddcmd class,D2_KMS,+T:foo
    ddcmd class D2_CORE +T:foo \; class D2_KMS +T:foo
    ddcmd "class,D2_CORE,+T:foo;,class,D2_KMS,+T:foo"
    ddcmd class,D2_CORE,+T:foo\;class,D2_KMS,+T:foo

    check_match_ct =T:foo 2 -v
    check_match_ct =Tl 0
    check_match_ct =Tmf 0

    tmark test_private_trace_syntax_rmmod about to do_prints
    doprints

    rmmod test_dynamic_debug	# remove module whose callsites are writing debug logs
                                # to trace instance foo
    ddcmd "close bar;close foo"	# close bar and foo trace instances, if usage count
                                # of foo instance was not correctly decremented during
				# removal of module then close will fail

    is_trace_instance_closed bar	# verify that foo and bar trace instances are closed
    is_trace_instance_closed foo
    search_trace_name foo 2 "D2_CORE msg"
    search_trace_name foo 1 "D2_KMS msg"

    del_trace_instance_dir foo 1	# delete trace instance foo and verify its
                                        # directory was removed
    check_trace_instance_dir foo 0

    search_trace "test_private_trace_syntax_rmmod about to do_prints"

    del_trace_instance_dir bar 1	# delete trace instance bar and verify its
                                        # directory was removed
    check_trace_instance_dir bar 0
}

# test verifies new syntax and combination of delete attempt of trace
# instance which is still in use with module removal
function test_private_trace_syntax_delete_in_use_rmmod {
    echo -e "${GREEN}# TEST_PRIVATE_TRACE_SYNTAX_DELETE_IN_USE_RMMOD ${NC}"
    ddcmd =_
    echo > /sys/kernel/tracing/trace
    echo 1 >/sys/kernel/tracing/tracing_on
    echo 1 >/sys/kernel/tracing/events/dyndbg/enable

    ddcmd open foo	# open trace instance foo and test new syntax
    modprobe test_dynamic_debug dyndbg=class,D2_CORE,+T:foo%class,D2_KMS,+T:foo
    check_match_ct =Tl 0
    check_match_ct =Tmf 0
    check_match_ct =T:foo 2 -v

    tmark should be ready
    search_trace_name "0" 0 "should be ready"	# search in global trace

    doprints
    search_trace_name foo 2 "D2_CORE msg"	# search in trace instance foo
    search_trace_name foo 1 "D2_KMS msg"

    # premature delete
    del_trace_instance_dir foo 0	# delete fails because foo is being used
    check_trace_instance_dir foo 1	# verify trace instance foo dir exists
    ifrmmod test_dynamic_debug

    ddcmd "close foo"			# close will succeed only if foo usage count
                                        # was correctly decremented during module removal
    is_trace_instance_closed foo	# verify trace instance foo is closed
    del_trace_instance_dir foo 1	# foo delete works now

    check_trace_instance_dir foo 0	# verify trace instance foo dir does not exist
    search_trace "should be ready"
}

# $1 - trace-buf-name (or "0")
# $2 - reference-buffer
function search_in_trace_for {
    bufname=$1; shift;
    ref=$2;
    ref2=$(echo $ref | cut -c20-)
    echo $ref2
}

function test_private_trace_mixed_class {
    local modname="test_dynamic_debug"
    echo -e "${GREEN}# TEST_PRIVATE_TRACE_mixed_class ${NC}"

    local eyeball_ref=<<'EOD'
        modprobe-1173    [001] .....   7.781008: 0: test_dynamic_debug:do_cats: test_dd: D2_CORE msg
        modprobe-1173    [001] .....   7.781010: 0: test_dynamic_debug:do_cats: test_dd: D2_KMS msg
        modprobe-1173    [001] .....   7.781010: 0: test_dynamic_debug:do_levels: test_dd: V3 msg
             cat-1214    [001] .....   7.905494: 0: test_dd: doing categories
             cat-1214    [001] .....   7.905495: 0: test_dynamic_debug:do_cats: test_dd: D2_CORE msg
             cat-1214    [001] .....   7.905496: 0: test_dynamic_debug:do_cats: test_dd: D2_KMS msg
             cat-1214    [001] .....   7.905497: 0: test_dd: doing levels
             cat-1214    [001] .....   7.905498: 0: test_dynamic_debug:do_levels: test_dd: V3 msg
EOD

    ddcmd =_
    ddcmd module,params,+T:unopened fail
    check_err_msg "Invalid argument"
    is_trace_instance_closed unopened
    check_trace_instance_dir unopened 0

    ddcmd open bupkus
    is_trace_instance_opened bupkus
    check_trace_instance_dir bupkus 1
    modprobe $modname \
	     dyndbg=class,D2_CORE,+T:bupkus.mf%class,D2_KMS,+T:bupkus.mf%class,V3,+T:bupkus.mf

    # test various name misses
    ddcmd "module params =T:bupkus1" fail	# miss on name
    check_err_msg "Invalid argument"
    ddcmd "module params =T:unopened" fail	# unopened
    check_err_msg "Invalid argument"

    ddcmd "module params =mlfT:bupkus."		# we allow dot at the end of flags
    ddcmd "module params =T:bupkus."
    ddcmd "module params =:bupkus."
    ddcmd "module params =:0."

    check_match_ct =T:bupkus.mf 3		# the 3 classes enabled above
    ddcmd "module $modname =T:bupkus"		# enable the 5 non-class'd pr_debug()s
    check_match_ct =T:bupkus 8 -r		# 8=5+3

    doprints
    ddcmd close,bupkus fail
    check_err_msg "Device or resource busy"
    ddcmd "module * -T:0"			# misses class'd ones
    ddcmd close,bupkus fail

    ddcmd class,D2_CORE,-T:0%class,D2_KMS,-T:0%class,V3,-T:0 # turn off class'd and set dest to 0
    ddcmd close,bupkus
    is_trace_instance_closed bupkus

    # validate the 3 enabled class'd sites, with mf prefix
    search_trace_name bupkus 0 "test_dynamic_debug:do_cats: test_dd: D2_CORE msg"
    search_trace_name bupkus 0 "test_dynamic_debug:do_cats: test_dd: D2_KMS msg"
    search_trace_name bupkus 0 "test_dynamic_debug:do_levels: test_dd: V3 msg"

    search_trace_name bupkus 0 "test_dd: doing levels"
    search_trace_name bupkus 0 "test_dd: doing categories"

    # reopen wo error
    ddcmd open bupkus
    is_trace_instance_opened bupkus
    check_trace_instance_dir bupkus 1

    ddcmd "module test_dynamic_debug =T:bupkus"	# rearm the 5 plain-old prdbgs
    check_match_ct =T:bupkus 5

    doprints # 2nd time
    search_trace_name bupkus 0 "test_dd: doing categories"
    search_trace_name bupkus 0 "test_dd: doing levels""
    search_trace_name bupkus 2 "test_dd: doing categories"
    search_trace_name bupkus 1 "test_dd: doing levels""

    ddcmd close,bupkus fail
    check_err_msg "Device or resource busy"
    del_trace_instance_dir bupkus 0
    check_err_msg "Device or resource busy"
    check_trace_instance_dir bupkus 1
    is_trace_instance_opened bupkus
    check_trace_instance_dir bupkus 1

    # drop last users, now delete works
    ddcmd "module * -T:0"
    ddcmd close,bupkus
    is_trace_instance_closed bupkus
    del_trace_instance_dir bupkus 1
    check_trace_instance_dir bupkus 0
    ifrmmod test_dynamic_debug
}

function test_private_trace_overlong_name {
    echo -e "${GREEN}# TEST_PRIVATE_TRACE_overlong_name ${NC}"
    ddcmd =_
    name="A_bit_lengthy_trace_instance_name"
    ddcmd open $name
    is_trace_instance_opened $name
    check_trace_instance_dir $name 1

    ddcmd "file kernel/module/main.c +T:$name.mf "
    check_match_ct =T:A_bit_lengthy_trace_....mf 14

    ddcmd "module * -T"
    check_match_ct =:A_bit_lengthy_trace_....mf 14
    check_match_ct kernel/module/main.c 14 -r		# to be certain

    ddcmd "module * -T:0"
    ddcmd close,$name
    is_trace_instance_closed $name
    del_trace_instance_dir $name 1
    check_trace_instance_dir $name 0
}

function test_private_trace_fill_trace_index {
    echo -e "${GREEN}# TEST_PRIVATE_TRACE_fill_trace_index ${NC}"
    ddcmd =_
    name="trace_instance_"
    for i in {1..63}
    do
        ddcmd open $name$i
	is_trace_instance_opened $name$i
        check_trace_instance_dir $name$i 1
    done
    # catch the 1-too-many err
    ddcmd "open too_many" fail
    check_err_msg "No space left on device"

    ddcmd 'file kernel/module/main.c =T:trace_instance_63.m'
    check_match_ct =T:trace_instance_63.m 14

    for i in {1..62}
    do
        ddcmd close $name$i
        is_trace_instance_closed $name$i
        del_trace_instance_dir $name$i 1
        check_trace_instance_dir $name$i 0
    done
    ddcmd "module * -T:0"
    ddcmd close,trace_instance_63
    is_trace_instance_closed trace_instance_63
    del_trace_instance_dir trace_instance_63 1
    check_trace_instance_dir trace_instance_63 0
}

# prepares dynamic debug and trace environment for tests execution
function setup_env_for_tests {
    echo -e "${GREEN}# SETUP_ENV_FOR_TESTS ${NC}"

    echo "MODULES"
    ifrmmod test_dynamic_debug_submod -v	# unload test_dynamic_debug_submod module
                                                # if it is loaded
    ifrmmod test_dynamic_debug -v	# unload test_dynamic_debug module it if is loaded
    echo

    # display all callsites which have flags != "_"
    echo "CALLSITES with flags != \":0\""
    cat /proc/dynamic_debug/control | grep -v "=_" | grep -v "not set" | grep -v "^$" \
	    | grep -v "#: Opened trace instances" | grep -v "#: Default trace destination"
    ddcmd module,*,=_:0 # clear all flags and set dest to 0
    echo

    # close all opened trace instances and delete their respective directories
    echo "OPEN trace instance"
    output=$(tail -n9 /proc/dynamic_debug/control | grep "#: Opened trace instances" \
	    | cut -f3 -d":" | xargs -n1)
    for dst in $output
    do
        echo "close trace instance '$dst'"
	echo close,$dst > /proc/dynamic_debug/control
	echo "delete '/sys/kernel/debug/tracing/instances/$dst' directory"
	rmdir /sys/kernel/debug/tracing/instances/$dst
    done
    echo
}

function test_labelling {
    echo -e "${GREEN}# TEST_SITE_LABELLING - ${NC}"
    ifrmmod test_dynamic_debug
    ddcmd =_

    # trace params processing of the modprobe
    ddcmd open,param_log%module,params,+T:param_log.tmfs
    check_match_ct =T:param_log 4 -r -v

    # modprobe with params.  This uses the default_dest :param_log
    modprobe test_dynamic_debug \
	     dyndbg=class,D2_CORE,+Tmf%class,D2_KMS,+Tmf%class,D2_ATOMIC,+pmT

    # check the trace for params processing during modprobe, with the expected prefixes
    search_trace_name param_log 5 "params:parse_args:kernel/params.c: doing test_dynamic_debug"
    search_trace_name param_log 4 "params:parse_one:kernel/params.c: doing test_dynamic_debug"

    # and for the enabled test-module's pr-debugs
    search_trace_name param_log 3 "test_dynamic_debug:do_cats: test_dd: D2_CORE msg"
    search_trace_name param_log 2 "test_dynamic_debug:do_cats: test_dd: D2_KMS msg"
    search_trace_name param_log 1 "test_dynamic_debug: test_dd: D2_ATOMIC msg"

    # now change the labelled sites, by using the existing label
    ddcmd open new_out
    ddcmd label param_log +T:new_out	# redirect unclassed
    check_match_ct =T:new_out 4	-r	# the module params prdbgs got moved
    check_match_ct =T:param_log 2 -r	# CORE, KMS remain
    ddcmd label param_log class D2_CORE +T:new_out	# must name class to change it
    ddcmd label param_log class D2_KMS  +T:new_out	# case for class D2_* (wildcard) ??
    check_match_ct =T:param_log 0
    check_match_ct =T:new_out 6	-r	# all are redirected
    check_match_ct =T:new_out.mfst 4	# module/params.c prdbgs still have the flags

    doprints
    search_trace_name new_out 2 "test_dynamic_debug:do_cats: test_dd: D2_CORE msg"
    search_trace_name new_out 1 "test_dynamic_debug:do_cats: test_dd: D2_KMS msg"

    check_match_ct =T.new_out 6 -r -v
    check_match_ct =T: 6 -r -v

    # its not enough to turn off T
    ddcmd -T
    ddcmd class D2_CORE -T % class D2_KMS -T
    check_match_ct =T 0
    check_match_ct =:new_out 6 -r -v

    # must un-label prdbgs to close the label
    ddcmd label new_out +:0
    ddcmd label new_out class D2_CORE +:0
    ddcmd label new_out class D2_KMS +:0
    ddcmd close new_out

    check_match_ct =T:param_log 0	# ok, but
    check_match_ct :param_log 1 -r -v	# pick up the D2_ATOMIC
    ddcmd label param_log class D2_ATOMIC +:0
    ddcmd close param_log		# now it closes wo -EBUSY

    ifrmmod test_dynamic_debug

    del_trace_instance_dir param_log 1
    del_trace_instance_dir new_out 1
}

tests_list=(
    basic_tests
    comma_terminator_tests
    test_percent_splitting
    test_mod_submod
    test_flags
    test_default_destination
    test_actual_trace
    cycle_tests_normal
    cycle_not_best_practices
    test_private_trace_1
    test_private_trace_simple_proper
    test_private_trace_syntax_close_in_use
    test_private_trace_syntax_rmmod
    test_private_trace_syntax_delete_in_use_rmmod
    test_private_trace_mixed_class
    test_private_trace_mixed_class  # again, to test pre,post conditions

    test_private_trace_overlong_name

    test_labelling

    # works, takes 30 sec
    test_private_trace_fill_trace_index
)

# Run tests

setup_env_for_tests
for test in "${tests_list[@]}"
do
    $test
    echo ""
done
echo -en "${GREEN}# Done on: "
date
