#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# dyndbg_package_metadata.sh - Extract, package, and strip dynamic debug site metadata
#
# Usage:
#   scripts/dyndbg_package_metadata.sh [--strip] <vmlinux|module.ko> [output.dyndbg.zst]

set -euo pipefail

STRIP=0
if [[ "${1:-}" == "--strip" ]]; then
    STRIP=1
    shift
fi

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 [--strip] <vmlinux|module.ko> [output.dyndbg.zst]" >&2
    exit 1
fi

TARGET="$1"
OUTPUT="${2:-}"

if [[ ! -f "$TARGET" ]]; then
    echo "Error: target file '$TARGET' not found" >&2
    exit 1
fi

# Extract Build-ID if available
BUILD_ID=""
if readelf -n "$TARGET" 2>/dev/null | grep -q "Build ID:"; then
    BUILD_ID=$(readelf -n "$TARGET" | grep "Build ID:" | awk '{print $3}')
fi

if [[ -z "$OUTPUT" ]]; then
    if [[ -n "$BUILD_ID" ]]; then
        OUTPUT="${TARGET%/*}/${BUILD_ID}.dyndbg.zst"
    else
        OUTPUT="${TARGET}.dyndbg.zst"
    fi
fi

TMPDIR="$(mktemp -d /tmp/dyndbg_pkg.XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT

PAYLOAD_RAW="$TMPDIR/payload.raw"

# Pack binary header + intervals + string pool using python
python3 - "$TARGET" "$BUILD_ID" "$PAYLOAD_RAW" << 'EOF'
import sys, struct, subprocess, os

target_path, build_id_hex, out_file = sys.argv[1:4]

def get_symbol_map(vmlinux_path):
    syms = {}
    try:
        out = subprocess.check_output(["nm", vmlinux_path], stderr=subprocess.DEVNULL).decode()
        for line in out.splitlines():
            parts = line.strip().split()
            if len(parts) == 3:
                addr, typ, name = parts
                syms[name] = int(addr, 16)
    except Exception:
        pass
    return syms

def get_sections(vmlinux_path):
    sections = []
    out = subprocess.check_output(["readelf", "-SW", vmlinux_path]).decode()
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("["):
            continue
        line = line.replace("[ ", "[")
        parts = line.split()
        if len(parts) >= 6 and parts[0].endswith("]"):
            try:
                name = parts[1]
                addr = int(parts[3], 16)
                off = int(parts[4], 16)
                size = int(parts[5], 16)
                sections.append((name, addr, off, size))
            except ValueError:
                continue
    return sections

def get_cstr_from_vma(vmlinux_path, sections, vma):
    for name, addr, off, size in sections:
        if addr <= vma < addr + size:
            file_off = off + (vma - addr)
            with open(vmlinux_path, "rb") as f:
                f.seek(file_off)
                raw = f.read(256)
                end = raw.find(b'\x00')
                if end != -1:
                    return raw[:end].decode('utf-8', errors='replace')
                return raw.decode('utf-8', errors='replace')
    return "<unknown>"

syms = get_symbol_map(target_path)
if "__start___dyndbg_sites" not in syms or "__stop___dyndbg_sites" not in syms:
    print(f"Warning: No __dyndbg_sites symbols found in {target_path}", file=sys.stderr)
    sys.exit(0)

sections = get_sections(target_path)
sites_start = syms["__start___dyndbg_sites"]
sites_stop = syms["__stop___dyndbg_sites"]

sites_bytes = None
for name, addr, off, size in sections:
    if addr <= sites_start and sites_stop <= addr + size:
        with open(target_path, "rb") as f:
            f.seek(off + (sites_start - addr))
            sites_bytes = f.read(sites_stop - sites_start)
        break

if not sites_bytes:
    print(f"Warning: Failed to extract __dyndbg_sites data from {target_path}", file=sys.stderr)
    sys.exit(0)

site_size = 24
site_count = len(sites_bytes) // site_size

mod_names = []
file_names = []
func_names = []

for i in range(site_count):
    mod_ptr, func_ptr, file_ptr = struct.unpack_from("<QQQ", sites_bytes, i * site_size)
    mod_names.append(get_cstr_from_vma(target_path, sections, mod_ptr))
    file_names.append(get_cstr_from_vma(target_path, sections, file_ptr))
    func_names.append(get_cstr_from_vma(target_path, sections, func_ptr))

# Build deduplicated string pool and interval list
pool = bytearray()
str_offsets = {}

def get_str_offset(s):
    if s not in str_offsets:
        off = len(pool)
        pool.extend(s.encode('utf-8') + b'\x00')
        str_offsets[s] = off
    return str_offsets[s]

records = []

# 1. Module intervals (Tag = 0)
start = 0
for i in range(1, site_count):
    if mod_names[i] != mod_names[start]:
        records.append((start, i - 1, 0, get_str_offset(mod_names[start])))
        start = i
records.append((start, site_count - 1, 0, get_str_offset(mod_names[start])))

# 2. File intervals (Tag = 1)
start = 0
for i in range(1, site_count):
    if file_names[i] != file_names[start]:
        records.append((start, i - 1, 1, get_str_offset(file_names[start])))
        start = i
records.append((start, site_count - 1, 1, get_str_offset(file_names[start])))

# 3. Function intervals (Tag = 2)
start = 0
for i in range(1, site_count):
    if func_names[i] != func_names[start]:
        records.append((start, i - 1, 2, get_str_offset(func_names[start])))
        start = i
records.append((start, site_count - 1, 2, get_str_offset(func_names[start])))

# Header: magic (4B), version (2B), flags (2B), desc_count (4B), interval_count (4B), strings_len (4B), build_id (20B), reserved (8B)
# Total header = 48 bytes
magic = 0x44594E44 # 'DYND'
version = 2
flags = 0
desc_count = site_count
interval_count = len(records)
strings_len = len(pool)

build_id_bytes = bytes.fromhex(build_id_hex) if len(build_id_hex) == 40 else b'\x00' * 20
if len(build_id_bytes) < 20:
    build_id_bytes = build_id_bytes.ljust(20, b'\x00')

hdr = struct.pack("<IHHIII20s8s", magic, version, flags, desc_count, interval_count, strings_len, build_id_bytes, b'\x00' * 8)

rec_bytes = bytearray()
for start_idx, end_idx, tag, str_off in records:
    rec_bytes.extend(struct.pack("<HHBB2xI", start_idx, end_idx, tag, 0, str_off))

with open(out_file, 'wb') as f:
    f.write(hdr)
    f.write(rec_bytes)
    f.write(pool)

print(f"Condensed {site_count} sites into {interval_count} intervals, {strings_len} string bytes ({len(hdr) + len(rec_bytes) + len(pool)} raw bytes)")
EOF

# Compress via zstd
zstd -19 -f "$PAYLOAD_RAW" -o "$OUTPUT"
echo "Generated $OUTPUT ($(stat -c%s "$OUTPUT") bytes)"

if [[ "$STRIP" -eq 1 ]]; then
    objcopy --remove-section=__dyndbg_sites "$TARGET"
    echo "Stripped __dyndbg_sites from $TARGET"
fi
