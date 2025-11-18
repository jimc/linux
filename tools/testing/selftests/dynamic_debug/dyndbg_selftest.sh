#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

# Standard kselftest exit codes
ksft_pass=0
ksft_fail=1
ksft_skip=4

RED="\033[0;31m"
GREEN="\033[0;32m"
YELLOW="\033[0;33m"
BLUE="\033[0;34m"
MAGENTA="\033[0;35m"
CYAN="\033[0;36m"
NC="\033[0;0m"
error_msg=""
V=${V:=0}  # invoke as V=1 $0  for global verbose

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
else
    # if no config, try runtime probes
    modprobe -n test_dynamic_debug 2>/dev/null ; LACK_TMOD=$?
    # assume builtin dyndbg if control exists (checked above)
    LACK_DD_BUILTIN=0
fi

# Clear any stale seen hashes from previous runs
rm -f /tmp/dyndbg_seen_hashes_$$

# ==============================================================================
# GOLDEN_RECORDS (MD5 Fingerprint Verification Database)
#
# This database stores the expected invariant log content hashes for our tests.
# Using 'compute_dmesg_block_fingerprint', we slice dmesg between custom kmsg markers,
# strip variable printk timestamps, and check if the computed hash is present
# here. This provides bulletproof, zero-maintenance log verification!
# ==============================================================================
function GOLDEN_RECORDS {
    cat << 'EOF'
#K: <md5_hash>                       <test_key>               <args>
EOF
}

# Slices dmesg, strips timestamps, hashes the output, and verifies the fingerprint
# $1 - unique test key (e.g. normal_513)
# $2 - optional extra args (like repeat count, etc.)
function compute_dmesg_block_fingerprint {
    local test_key="$1"
    local extra_args="$2"

    local start_marker="DYNDBG_START_${test_key}"
    local end_marker="DYNDBG_END_${test_key}"

    # 1. Slice log buffer between our markers
    # 2. Exclude the start and end marker lines to keep the hash invariant to line/label shifts
    # 3. Strip printk timestamps (e.g. "[ 123.456789] ") to ensure platform independence
    local log_slice=$(dmesg | sed -n "/$start_marker/,/$end_marker/p" | grep -E -v "DYNDBG_START_|DYNDBG_END_" | sed -e 's/^\[[^]]*\] //')

    # Calculate the invariant fingerprint (strip carriage returns)
    local fingerprint=$(echo "$log_slice" | tr -d '\r' | md5sum | cut -d' ' -f1)

    # 100% simple and robust lookup: check if the computed hash exists in GOLDEN_RECORDS
    if GOLDEN_RECORDS | grep -q "$fingerprint"; then
        echo -e "${GREEN}: Verified! Log fingerprint matches golden-sample ($fingerprint)${NC}"
        echo "$fingerprint" >> /tmp/dyndbg_seen_hashes_$$
    else
        echo -e "${RED}: FINGERPRINT MISMATCH!"
        echo -e "Calculated hash: '$fingerprint' not found in GOLDEN_RECORDS()"
        echo -e "\nAdd or replace this line in GOLDEN_RECORDS():"
        printf "#K= %-32s %-24s %s\n" "${fingerprint}" "${test_key}" "${extra_args}"
        echo -e "\n--- Sliced Invariant Log Output ---"
        echo "$log_slice"
        echo -e "-----------------------------------"
        echo -e "\nAdd or replace this line in GOLDEN_RECORDS():"
        printf "#K= %-32s %-24s %s\n" "${fingerprint}" "${test_key}" "${extra_args}"
        echo -e "${NC}"
        exit $ksft_fail
    fi
}

# invokes test-module's do_bulk pr-debug-workload N times, wrapped
# inside START/END markers.  


# Triggers do_bulk with a repeat count, writes start/end markers with
# descriptive tag, and fingerprints it


# $1 - repeat count (default: 1)
# $2 - character of test / descriptive label (default: bulk)
function do_bulk_and_fingerprint {
    local ct="${1:-1}"
    local desc_name="${2:-bulk}"
    local line_num="${BASH_LINENO[0]}"
    
    # Sanitize desc_name to be completely safe from special regex/wildcard characters and spaces
    local safe_name=$(echo "$desc_name" | tr -c 'a-zA-Z0-9_' '_' | tr -s '_')
    local test_key="${safe_name}_${line_num}"

    echo "DYNDBG_START_${test_key}" > /dev/kmsg
    echo "$ct" > /sys/module/test_dynamic_debug/parameters/do_bulk
    echo "DYNDBG_END_${test_key}" > /dev/kmsg

    # test the output vs known results, report
    compute_dmesg_block_fingerprint "$test_key" "$ct"
}

# Utilites, test primitives

function ifrmmod {
    lsmod | grep "$1" >/dev/null 2>&1 && rmmod $1
}

function ddgrep () {
    grep "$1" /proc/dynamic_debug/control
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
    if [ "$3" -ne $exp_exit_code ]; then
        echo -e "${RED}: $BASH_SOURCE:$1 $2() expected to exit with code $exp_exit_code, got $3"
	[ "$3" == 1 ] && echo "Error: '$error_msg'"
        exit $ksft_fail
    fi
}

# $1 - pattern to match, pattern in $1 is enclosed by spaces for a match ""\s$1\s"
# $2 - number of times the pattern passed in $1 is expected to match
# $3 - optional can be set either to "-r" or "-v"
#       "-r" means relaxed matching in this case pattern provided in
#       $1 is passed as is without enclosing it with spaces "-v"
#       prints matching lines
# $4 - optional when $3 is set to "-r" then $4 can be used to pass "-v"
function count_pr_debugs {
    pattern="\s$1\s"
    exp_cnt=0

    [ "$3" == "-r" ] && pattern="$1"
    let cnt=$(ddgrep "$pattern" | wc -l)
    if [ "$V" -eq 1 ] || [ "$3" == "-v" ] || [ "$4" == "-v" ]; then
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

function basic_tests {
    echo -e "${GREEN}# BASIC_TESTS ${NC}"
    if [ $LACK_DD_BUILTIN -eq 1 ]; then
	echo "SKIP"
	exit $ksft_skip
    fi
    ddcmd =_ # zero everything
    count_pr_debugs =p 0

    # module params are builtin to handle boot args
    count_pr_debugs '\[kernel/params\]' 4 -r
    ddcmd module params +mpf
    count_pr_debugs =pmf 4

    # multi-cmd input, newline separated, with embedded comments
    cat <<"EOF" > /proc/dynamic_debug/control
      module params =_				# clear params
      module params +mf				# set flags
      module params func parse_args +sl		# other flags
EOF
    count_pr_debugs =mf 3
    count_pr_debugs =mfsl 1
    ddcmd =_
}

function test_subsystem_module_queries {
    echo -e "${GREEN}# TEST_SUBSYTEM_MODULE_QUERIES ${NC}"
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
    count_pr_debugs =p $slash_main -r

    echo "# testing 'module init/main' (specific path)"
    ddcmd =_
    ddcmd module "init/main" +p
    local init_main=$(grep -c "\[init/main\]" /proc/dynamic_debug/control)
    count_pr_debugs =p $init_main

    echo "# testing 'module main' (basename match)"
    ddcmd =_
    ddcmd module main +p
    # This should match ALL $total_main entries due to kbasename matching
    count_pr_debugs =p $total_main
}

function test_hyphen_underscore {
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

    # 1. Enable using literal hyphen name
    echo "#   trying hyphen name: $mod_with_hyphen"
    ddcmd module "$mod_with_hyphen" +p
    local count_hyphen=$(grep -c "\[$mod_with_hyphen\]" /proc/dynamic_debug/control)
    count_pr_debugs =p $count_hyphen -r

    # 2. Disable and then enable using underscore name
    ddcmd =_
    echo "#   trying underscore name: $mod_with_underscore"
    ddcmd module "$mod_with_underscore" +p
    count_pr_debugs =p $count_hyphen -r

    # 3. Try kbasename with hyphen (if it has a path)
    local base_hyphen=$(basename "$mod_with_hyphen")
    if [ "$base_hyphen" != "$mod_with_hyphen" ]; then
        ddcmd =_
        echo "#   trying hyphen kbasename: $base_hyphen"
        ddcmd module "$base_hyphen" +p
        local count_base=$(grep -c "\[\([^]]*/\)\?$base_hyphen\]" /proc/dynamic_debug/control)
        count_pr_debugs =p $count_base -r
    fi

    # 4. Try kbasename with underscore
    local base_underscore=$(echo "$base_hyphen" | tr '-' '_')
    ddcmd =_
    echo "#   trying underscore kbasename: $base_underscore"
    ddcmd module "$base_underscore" +p
    local count_base=$(grep -c "\[\([^]]*/\)\?$base_hyphen\]" /proc/dynamic_debug/control)
    count_pr_debugs =p $count_base -r

    ddcmd =_
}

tests_list=(
    basic_tests
    test_subsystem_module_queries
    test_hyphen_underscore
)

# Run tests

function audit_golden_records {
    local seen_file="/tmp/dyndbg_seen_hashes_$$"

    if [ ! -f "$seen_file" ]; then
        return
    fi

    echo -e "${YELLOW}# --- GOLDEN_RECORDS Audit ---${NC}"
    local stale_found=0

    # Read each active record line from GOLDEN_RECORDS
    while read -r line; do
        # Extract the hash (second word) from the #K= line
        local hash=$(echo "$line" | awk '{print $2}')

        # Check if this hash was seen during the run
        if ! grep -q "$hash" "$seen_file" 2>/dev/null; then
            if [ $stale_found -eq 0 ]; then
                echo -e "${YELLOW}# The following GOLDEN_RECORDS entries were never hit and may be stale:${NC}"
                stale_found=1
            fi
            echo -e "${YELLOW}#K_STALE= $line${NC}"
        fi
    done < <(GOLDEN_RECORDS | grep "^#K=" | grep -v "<md5_hash>")

    if [ $stale_found -eq 0 ]; then
        echo -e "${GREEN}# All GOLDEN_RECORDS entries were successfully hit!${NC}"
    fi

    # Clean up
    rm -f "$seen_file"
}

ifrmmod test_dynamic_debug

for test in "${tests_list[@]}"
do
    $test
    echo ""
done
echo -en "${GREEN}# Done on: "
date
echo -en "${NC}"

audit_golden_records

exit $ksft_pass
