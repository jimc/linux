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
    lsmod | grep $1 2>&1>/dev/null || echo $1 not there
    lsmod | grep $1 2>&1>/dev/null && rmmod $1
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

# proper life cycle - open, enable:named, disable:named, close
function test_private_trace_simple_proper {
    echo -e "${GREEN}# TEST_PRIVATE_TRACE_1 ${NC}"
    # ddcmd close kparm_stream
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

function test_private_trace_2 {
    echo -e "${GREEN}# TEST_PRIVATE_TRACE_2 ${NC}"
    ddcmd =_
    echo > /sys/kernel/tracing/trace
    echo 1 >/sys/kernel/tracing/tracing_on
    echo 1 >/sys/kernel/tracing/events/dyndbg/enable
    ddcmd open foo
    is_trace_instance_opened foo
    check_trace_instance_dir foo 1

    modprobe test_dynamic_debug
    ddcmd class,D2_CORE,+T:foo.l,%class,D2_KMS,+fT:foo.ml
    check_match_ct =T:foo.l 1
    check_match_ct =T:foo.mfl 1

    # purpose ?
    echo 1 >/sys/kernel/tracing/tracing_on
    echo 1 >/sys/kernel/tracing/events/dyndbg/enable

    tmark test_private_trace about to do_prints
    search_trace "test_private_trace about to do_prints"
    search_trace_name "0" 1 "test_private_trace about to do_prints"

    doprints
    ddcmd class,D2_CORE,-T:foo
    ddcmd close foo fail
    check_err_msg "Device or resource busy"
    ddcmd class,D2_KMS,-T:foo
    ddcmd close foo
    check_trace_instance_dir foo 1
    is_trace_instance_closed foo
    ddcmd close bar fail
    check_err_msg "No such file or directory"
    ifrmmod test_dynamic_debug
    search_trace_name foo 2 "D2_CORE msg"
    search_trace_name foo 1 "D2_KMS msg"
    del_trace_instance_dir foo 1
    check_trace_instance_dir foo 0
}

function test_private_trace_3 {
    echo -e "${GREEN}# TEST_PRIVATE_TRACE_3 ${NC}"
    ddcmd =_
    echo > /sys/kernel/tracing/trace
    echo 1 >/sys/kernel/tracing/tracing_on
    echo 1 >/sys/kernel/tracing/events/dyndbg/enable
    ddcmd open foo \; open bar
    is_trace_instance_opened foo
    is_trace_instance_opened bar
    modprobe test_dynamic_debug
    ddcmd class,D2_CORE,+T:foo
    ddcmd class,D2_KMS,+T:foo
    ddcmd class D2_CORE +T:foo \; class D2_KMS +T:foo
    ddcmd "class,D2_CORE,+T:foo;,class,D2_KMS,+T:foo"
    ddcmd class,D2_CORE,+T:foo\;class,D2_KMS,+T:foo
    check_match_ct =T 2 -v
    check_match_ct =Tl 0
    check_match_ct =Tmf 0
    echo 1 >/sys/kernel/tracing/tracing_on
    echo 1 >/sys/kernel/tracing/events/dyndbg/enable
    tmark test_private_trace_2 about to do_prints
    doprints
    rmmod test_dynamic_debug
    ddcmd "close bar;close foo"
    is_trace_instance_closed bar
    is_trace_instance_closed foo
    search_trace_name foo 2 "D2_CORE msg"
    search_trace_name foo 1 "D2_KMS msg"
    del_trace_instance_dir foo 1
    check_trace_instance_dir foo 0
    search_trace "test_private_trace_2 about to do_prints"
    del_trace_instance_dir bar 1
    check_trace_instance_dir bar 0
}

function test_private_trace_4 {
    echo -e "${GREEN}# TEST_PRIVATE_TRACE_4 ${NC}"
    ddcmd =_
    echo > /sys/kernel/tracing/trace
    echo 1 >/sys/kernel/tracing/tracing_on
    echo 1 >/sys/kernel/tracing/events/dyndbg/enable

    ddcmd open foo
    modprobe test_dynamic_debug dyndbg=class,D2_CORE,+T:foo%class,D2_KMS,+T:foo
    check_match_ct =Tl 0
    check_match_ct =Tmf 0
    check_match_ct =T 2

    # are these 2 doing anything ?
    echo 1 >/sys/kernel/tracing/tracing_on
    echo 1 >/sys/kernel/tracing/events/dyndbg/enable

    tmark should be ready
    search_trace_name "0" 0 "should be ready"	# in global trace

    doprints
    search_trace_name foo 2 "D2_CORE msg"	# in private buf
    search_trace_name foo 1 "D2_KMS msg"

    # premature delete
    del_trace_instance_dir foo 0
    check_trace_instance_dir foo 1	# doesn't delete
    ifrmmod test_dynamic_debug

    ddcmd "close foo"
    is_trace_instance_closed foo
    del_trace_instance_dir foo 1	# delete works now

    check_trace_instance_dir foo 0
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

tests_list=(
    basic_tests
    comma_terminator_tests
    test_percent_splitting
    test_mod_submod
    test_actual_trace
    cycle_tests_normal
    cycle_not_best_practices
    test_private_trace_1
    test_private_trace_simple_proper
    test_private_trace_2
    test_private_trace_3
    test_private_trace_4
    test_private_trace_mixed_class
    test_private_trace_mixed_class  # again, to test pre,post conditions

    test_private_trace_overlong_name

    # works, takes 30 sec
    test_private_trace_fill_trace_index
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
