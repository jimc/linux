#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Hash-based validation and state verification helper library.

# Default APP to DYNDBG if not already set
APP="${APP:-DYNDBG}"
APP_LOWER=$(echo "$APP" | tr '[:upper:]' '[:lower:]')

# Global files for tracking seen and new hashes
SEEN_HASHES_FILE="/tmp/${APP_LOWER}_seen_hashes_$$"
NEW_HASHES_FILE="/tmp/${APP_LOWER}_new_hashes_$$"

# Global sequence counter to ensure unique dmesg block markers
TEST_SEQ_CTR=0

function slice_and_hash_dmesg {
    # Slices dmesg, strips timestamps, and returns the MD5 hash
    # $1 - unique test key (e.g. normal_513)

    local label="$1"
    local app="${APP:-DYNDBG}"
    local start_marker="${app}_START_${label}"
    local end_marker="${app}_END_${label}"

    # 1. Slice log buffer between our markers
    # 2. Exclude the start and end marker lines to keep the hash invariant to line/label shifts
    # 3. Filter only target dynamic-debug and test module prints to ignore background scheduling noise
    # 4. Strip printk timestamps (e.g. "[ 123.456789] ") to ensure platform independence
    local log_slice=$(dmesg | sed -n "/$start_marker/,/$end_marker/p" | \
        grep -E -v "${app}_START_|${app}_END_" | \
        grep -E "dyndbg:|test_dd:|test_dynamic_debug|handling " | \
        sed -e 's/^\[[^]]*\] //')

    # Calculate and print the invariant fingerprint
    echo "$log_slice" | tr -d '\r' | md5sum | cut -d' ' -f1
}

function slice_by_grep {
    # Isolate lines matching a pattern from a file
    # $1 - pattern to grep
    # $2 - file path (reads $CONTROL_FILE if not provided)
    local pattern="$1"
    local file_path="${2:-$CONTROL_FILE}"
    grep "$pattern" "$file_path"
}

function slice_and_hash_by_grep {
    # Slices a file by a pattern and returns its MD5 hash
    # $1 - pattern to grep
    # $2 - file path (reads $CONTROL_FILE if not provided)
    local pattern="$1"
    local file_path="${2:-$CONTROL_FILE}"
    local slice=$(slice_by_grep "$pattern" "$file_path")
    echo "$slice" | tr -d '\r' | md5sum | cut -d' ' -f1
}

function verify_fingerprint {
    # Verifies a calculated fingerprint against the GOLDEN_RECORDS database
    # $1 - unique test key (e.g. normal_513)
    # $2 - optional extra args
    # $3 - the calculated fingerprint hash to verify
    # $4 - description of what was captured (e.g. "Dmesg Log" or "Control File")
    # $5 - the raw captured text block (to display in case of mismatch)

    local label="$1"
    local extra_args="$2"
    local fingerprint="$3"
    local capture_desc="$4"
    local raw_capture="$5"

    # 100% simple and robust lookup: check if the computed hash exists in GOLDEN_RECORDS
    if GOLDEN_RECORDS | grep -q "$fingerprint"; then
        local short_hash="${fingerprint:0:12}"
        [ "$V" -ge 1 ] && echo -e "${GREEN}✔ Verified! [$extra_args] matches label '$label' ($short_hash) [via: '$CUMULATIVE_DDCMDS']${NC}"
        if [ $V -ge 2 ]; then
            echo -e "${CYAN}--- Captured Invariant ${capture_desc} Output ($label) ---"
            if [ "$capture_desc" = "Control File" ] || [ "$capture_desc" = "File Slice" ]; then
                echo "$raw_capture" | sed -E "s/ =([_a-z]*[a-z][_a-z]*) / ${YELLOW}=\1${CYAN} /g"
            else
                echo "$raw_capture"
            fi
            echo -e "-----------------------------------${NC}"
        fi
        echo "$fingerprint" >> "$SEEN_HASHES_FILE"

        # Write to long-term drift telemetry (Host will capture and enrich via stdout TELEMETRY:)
        local T="${T:-0}"
        if [ "$T" != "0" ] && [ "$T" != "n" ]; then
            echo "TELEMETRY: $label $fingerprint VERIFIED $extra_args [via:\"$CUMULATIVE_DDCMDS\"]"
        fi
    else
        # Computed hash not found! Check if the label itself exists anywhere in the database
        local status_str="UNREGISTERED"
        if GOLDEN_RECORDS | grep -q -E "[[:space:]]${label}[[:space:]]"; then
            # Label exists, but hash is different: OUTDATED / DRIFTED!
            local expected_hash=$(GOLDEN_RECORDS | grep -E "[[:space:]]${label}[[:space:]]" | head -n1 | awk '{print $2}')
            local short_expected="${expected_hash:0:12}"
            local short_got="${fingerprint:0:12}"
            echo -e "${RED}: ${capture_desc^^} STATE DRIFTED! Label '$label' has changed.${NC}"
            echo -e "Expected: '$short_expected' ($expected_hash)"
            echo -e "Got:      '$short_got' ($fingerprint)${NC}"
            status_str="DRIFTED"
        else
            # Label does not exist: BRAND NEW!
            echo -e "${YELLOW}: NEW ${capture_desc^^} RECORD NEEDED! Label '$label' is unregistered.${NC}"
        fi

        # Write to long-term drift telemetry (Host will capture and enrich via stdout TELEMETRY:)
        local T="${T:-0}"
        if [ "$T" != "0" ] && [ "$T" != "n" ]; then
            echo "TELEMETRY: $label $fingerprint $status_str $extra_args [via:\"$CUMULATIVE_DDCMDS\"]"
        fi

        echo -e "\nAdd or replace this line in GOLDEN_RECORDS():"
        printf "#K= %-32s %-24s %s\n" "${fingerprint}" "${label}" "${extra_args}"
        echo -e "\n--- Captured Invariant ${capture_desc} Output ---"
        if [ "$capture_desc" = "Control File" ] || [ "$capture_desc" = "File Slice" ]; then
            echo "$raw_capture" | sed -E "s/ =([_a-z]*[a-z][_a-z]*) / ${YELLOW}=\1${NC} /g"
        else
            echo "$raw_capture"
        fi
        echo -e "-----------------------------------"
        echo -e "${NC}"

        # Accumulate the mismatch line for consolidation at the end
        printf "#K= %-32s %-24s %s\n" "${fingerprint}" "${label}" "${extra_args}" >> "$NEW_HASHES_FILE"
    fi
}

function verify_dmesg_fingerprint {
    # Verifies a dmesg log fingerprint against the GOLDEN_RECORDS database
    # $1 - unique test key (e.g. normal_513)
    # $2 - optional extra args
    # $3 - the calculated fingerprint hash to verify

    local label="$1"
    local extra_args="dmesg:${2:-1}"
    local fingerprint="$3"
    local app="${APP:-DYNDBG}"

    # Slices dmesg ONLY on failure path to keep success path fast and simple
    if ! GOLDEN_RECORDS | grep -q "$fingerprint"; then
        local start_marker="${app}_START_${label}"
        local end_marker="${app}_END_${label}"
        local log_slice=$(dmesg | sed -n "/$start_marker/,/$end_marker/p" | \
            grep -E -v "${app}_START_|${app}_END_" | \
            grep -E "dyndbg:|test_dd:|test_dynamic_debug|handling " | \
            sed -e 's/^\[[^]]*\] //')
        verify_fingerprint "$label" "$extra_args" "$fingerprint" "Dmesg Log" "$log_slice"
    else
        # Success path: log the hit
        local short_hash="${fingerprint:0:12}"
        [ "$V" -ge 1 ] && echo -e "${GREEN}✔ Verified! [$extra_args] matches label '$label' ($short_hash) [via: '$CUMULATIVE_DDCMDS']${NC}"
        echo "$fingerprint" >> "$SEEN_HASHES_FILE"

        # Write to long-term drift telemetry (Host will capture and enrich via stdout TELEMETRY:)
        local T="${T:-0}"
        if [ "$T" != "0" ] && [ "$T" != "n" ]; then
            echo "TELEMETRY: $label $fingerprint VERIFIED $extra_args [via:\"$CUMULATIVE_DDCMDS\"]"
        fi
    fi
}

function verify_grep_fingerprint {
    # Verifies a sliced file fingerprint against the GOLDEN_RECORDS database
    # $1 - the calculated fingerprint hash to verify
    # $2 - unique test key (e.g. FT_basic_queries_params_mpf)
    # $3 - pattern to grep
    # $4 - optional file path (defaults to $CONTROL_FILE)
    # $5 - optional extra args (defaults to control:"pattern")

    local fingerprint="$1"
    local label="$2"
    local pattern="$3"
    local file="${4:-$CONTROL_FILE}"
    local extra_args="${5:-control:\"$pattern\"}"

    # Captures control lines ONLY on failure path
    if ! GOLDEN_RECORDS | grep -q "$fingerprint"; then
        local slice=$(slice_by_grep "$pattern" "$file")
        verify_fingerprint "$label" "$extra_args" "$fingerprint" "File Slice" "$slice"
    else
        # Success path: log the hit
        local short_hash="${fingerprint:0:12}"
        [ "$V" -ge 1 ] && echo -e "${GREEN}✔ Verified! [$extra_args] matches label '$label' ($short_hash) [via: '$CUMULATIVE_DDCMDS']${NC}"
        echo "$fingerprint" >> "$SEEN_HASHES_FILE"

        # Write to long-term drift telemetry (Host will capture and enrich via stdout TELEMETRY:)
        local T="${T:-0}"
        if [ "$T" != "0" ] && [ "$T" != "n" ]; then
            echo "TELEMETRY: $label $fingerprint VERIFIED $extra_args [via:\"$CUMULATIVE_DDCMDS\"]"
        fi
    fi
}

function verify_grep_state {
    # Captures a file slice by pattern, computes its hash,
    # and verifies it against the GOLDEN_RECORDS database.
    # $1 - unique test key
    # $2 - pattern to slice
    # $3 - optional file path (defaults to $CONTROL_FILE)
    # $4 - optional extra args

    local label="$1"
    local pattern="$2"
    local file="${3:-$CONTROL_FILE}"
    local extra_args="$4"

    # 1. Compute
    local hash=$(slice_and_hash_by_grep "$pattern" "$file")

    # 2. Verify
    verify_grep_fingerprint "$hash" "$label" "$pattern" "$file" "$extra_args"
}

function do_logging_and_verify {
    # inside START/END markers:
    # runs module's do_bulk sysnode with a repeat count,
    # and verifies the fingerprint of the response.
    # $1 - repeat count (default: 1)
    # $2 - character of test / descriptive label (default: bulk)

    local ct="${1:-1}"
    local desc_name="${2:-bulk}"
    local app="${APP:-DYNDBG}"
    local line_num="${BASH_LINENO[0]}"
    
    # Increment global sequence counter to ensure unique dmesg block markers
    ((TEST_SEQ_CTR++))

    # Sanitize desc_name to be completely safe from special regex/wildcard characters and spaces
    local safe_name=$(echo "$desc_name" | tr -c 'a-zA-Z0-9_' '_' | tr -s '_')
    local label="${safe_name}_${line_num}_seq${TEST_SEQ_CTR}"

    echo "${app}_START_${label}" > /dev/kmsg
    echo "$ct" > /sys/module/test_dynamic_debug/parameters/do_bulk
    echo "${app}_END_${label}" > /dev/kmsg

    # 1. Compute
    local hash=$(slice_and_hash_dmesg "$label")

    # 2. Verify
    verify_dmesg_fingerprint "$label" "$ct" "$hash"
}

function verify_modprobe_param_logging {
    # $1 - parameter name (e.g. do_classes)
    # $2 - parameter value (e.g. 1)
    # $3 - short descriptive tag (e.g. classes)
    local param="$1"
    local val="$2"
    local tag="$3"
    local app="${APP:-DYNDBG}"
    local line_num="${BASH_LINENO[0]}"

    # Make sure both modules are completely unloaded to trigger a fresh load
    ifrmmod test_dynamic_debug_submod
    ifrmmod test_dynamic_debug

    ((TEST_SEQ_CTR++))
    local label="modprobe_${tag}_line${line_num}_seq${TEST_SEQ_CTR}"

    echo "${app}_START_${label}" > /dev/kmsg
    modprobe test_dynamic_debug "${param}=${val}"
    echo "${app}_END_${label}" > /dev/kmsg

    local hash=$(slice_and_hash_dmesg "$label")
    verify_dmesg_fingerprint "$label" "1" "$hash"
}

function audit_golden_records {
    local seen_file="$SEEN_HASHES_FILE"

    if [ ! -f "$seen_file" ]; then
        return
    fi

    # Require GOLDEN_RECORDS to be defined in the caller script
    if ! declare -f GOLDEN_RECORDS >/dev/null; then
        return
    fi

    echo -e "${YELLOW}# --- GOLDEN_RECORDS Audit ---${NC}"
    local stale_found=0
    local total_records=$(GOLDEN_RECORDS | grep -c "^#K=")

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
        echo -e "${GREEN}# All $total_records GOLDEN_RECORDS entries were successfully hit!${NC}"
    fi

    # Detect duplicate labels in the database
    local dupes=$(GOLDEN_RECORDS | grep "^#K=" | awk '{print $3}' | sort | uniq -d)
    if [ -n "$dupes" ]; then
        echo -e "\n${RED}# WARNING: Duplicate labels detected in GOLDEN_RECORDS():${NC}"
        echo "$dupes" | sed 's/^/#   /'
    fi

    # Clean up
    rm -f "$seen_file"
}
