// SPDX-License-Identifier: GPL-2.0
//! Dynamic Debug Query Parser in Rust.
#![allow(missing_docs)]

use kernel::error::code;
use kernel::prelude::*;

const __LOG_PREFIX: &[u8] = b"dyndbg\0";

#[repr(C)]
pub struct ddebug_query {
    pub filename: *const core::ffi::c_char,
    pub module: *const core::ffi::c_char,
    pub function: *const core::ffi::c_char,
    pub format: *const core::ffi::c_char,
    pub class_string: *const core::ffi::c_char,
    pub first_lineno: core::ffi::c_uint,
    pub last_lineno: core::ffi::c_uint,
}

#[repr(C)]
pub struct flag_settings {
    pub flags: core::ffi::c_uint,
    pub mask: core::ffi::c_uint,
}

const FLAG_PRINT: u32 = 1 << 0;
const FLAG_INCL_MODNAME: u32 = 1 << 1;
const FLAG_INCL_FUNCNAME: u32 = 1 << 2;
const FLAG_INCL_LINENO: u32 = 1 << 3;
const FLAG_INCL_SOURCENAME: u32 = 1 << 4;
const FLAG_INCL_TID: u32 = 1 << 5;
const FLAG_INCL_STACK: u32 = 1 << 6;
const FLAG_COUNT: u32 = 1 << 7;

const DDEBUG_LINE_MAX: u32 = 65535;

fn parse_lineno(s: &str) -> Result<u32, &'static str> {
    if s.is_empty() {
        Ok(0)
    } else {
        s.parse::<u32>().map_err(|_| "bad line-number")
    }
}

fn parse_linerange(
    first_str: &str,
    query: &mut ddebug_query,
) -> Result<(), &'static str> {
    if query.first_lineno != 0 || query.last_lineno != 0 {
        return Err("match-spec: line used 2x");
    }

    let (first, last) = if let Some(idx) = first_str.find('-') {
        (&first_str[..idx], Some(&first_str[idx + 1..]))
    } else {
        (first_str, None)
    };

    let first_line = parse_lineno(first)?;
    let last_line = if let Some(last_s) = last {
        let l = parse_lineno(last_s)?;
        if l == 0 {
            u32::MAX
        } else {
            l
        }
    } else {
        first_line
    };

    if last_line < first_line {
        return Err("last-line < 1st-line");
    }

    // Clamp values > DDEBUG_LINE_MAX to DDEBUG_LINE_MAX per design
    if first_line > DDEBUG_LINE_MAX {
        query.first_lineno = DDEBUG_LINE_MAX;
        pr_info!("clamping first_lineno to {}\n", DDEBUG_LINE_MAX);
    } else {
        query.first_lineno = first_line;
    }

    if last_line > DDEBUG_LINE_MAX {
        query.last_lineno = DDEBUG_LINE_MAX;
        pr_info!("clamping last_lineno to {}\n", DDEBUG_LINE_MAX);
    } else {
        query.last_lineno = last_line;
    }

    Ok(())
}

unsafe fn split_file_tail(
    filename: *mut u8,
    query: &mut ddebug_query,
) -> Result<(), &'static str> {
    unsafe {
        let mut p = filename;
        while *p != b'\0' {
            if *p == b':' {
                *p = b'\0';
                let tail = p.add(1);
                let first_char = *tail;
                if first_char.is_ascii_alphabetic()
                    || first_char == b'*'
                    || first_char == b'?'
                {
                    if !query.function.is_null() {
                        return Err("match-spec: func overridden by file tail");
                    }
                    query.function = tail as *const core::ffi::c_char;
                } else {
                    let tail_str = core::ffi::CStr::from_ptr(
                        tail as *const core::ffi::c_char,
                    )
                    .to_str()
                    .map_err(|_| "invalid utf8 in file tail")?;
                    parse_linerange(tail_str, query)?;
                }
                break;
            }
            p = p.add(1);
        }
    }
    Ok(())
}

fn parse_flags(
    flag_str: &str,
    modifiers: &mut flag_settings,
) -> Result<(), &'static str> {
    let mut chars = flag_str.chars();
    let op = match chars.next() {
        Some('+') => '+',
        Some('-') => '-',
        Some('=') => '=',
        _ => return Err("bad flag-op, at start of flags string"),
    };

    let mut flags = 0;
    for c in chars {
        let flag = match c {
            'p' => FLAG_PRINT,
            'm' => FLAG_INCL_MODNAME,
            'f' => FLAG_INCL_FUNCNAME,
            'l' => FLAG_INCL_LINENO,
            't' => FLAG_INCL_TID,
            's' => FLAG_INCL_SOURCENAME,
            'd' => FLAG_INCL_STACK,
            'c' => FLAG_COUNT,
            '_' => continue, // Accept '_' as a flag placeholder
            _ => return Err("unknown flag"),
        };
        flags |= flag;
    }

    modifiers.flags = flags;
    match op {
        '=' => {
            modifiers.mask = 0;
        }
        '+' => {
            modifiers.mask = !0;
        }
        '-' => {
            modifiers.mask = !flags;
            modifiers.flags = 0;
        }
        _ => unreachable!(),
    }
    Ok(())
}

unsafe fn unescape_inplace(ptr: *mut u8) {
    unsafe {
        let mut r = ptr; // read pointer
        let mut w = ptr; // write pointer
        while *r != b'\0' {
            if *r == b'\\' {
                r = r.add(1);
                match *r {
                    b'a' => { *w = 0x07; r = r.add(1); }
                    b'b' => { *w = 0x08; r = r.add(1); }
                    b't' => { *w = b'\t'; r = r.add(1); }
                    b'n' => { *w = b'\n'; r = r.add(1); }
                    b'v' => { *w = 0x0b; r = r.add(1); }
                    b'f' => { *w = 0x0c; r = r.add(1); }
                    b'r' => { *w = b'\r'; r = r.add(1); }
                    b'e' => { *w = 0x1b; r = r.add(1); }
                    b'\\' => { *w = b'\\'; r = r.add(1); }
                    b'?' => { *w = b'?'; r = r.add(1); }
                    b'\'' => { *w = b'\''; r = r.add(1); }
                    b'"' => { *w = b'"'; r = r.add(1); }
                    b'0'..=b'7' => {
                        // Octal escape
                        let mut val = 0u32;
                        let mut count = 0;
                        while count < 3 && *r >= b'0' && *r <= b'7' {
                            val = (val << 3) + (*r - b'0') as u32;
                            r = r.add(1);
                            count += 1;
                        }
                        *w = val as u8;
                    }
                    b'\0' => {
                        *w = b'\\';
                    }
                    _ => {
                        // Keep backslash and the character if unrecognized
                        *w = b'\\';
                        w = w.add(1);
                        *w = *r;
                        r = r.add(1);
                    }
                }
            } else {
                *w = *r;
                r = r.add(1);
            }
            w = w.add(1);
        }
        *w = b'\0';
    }
}

unsafe fn tokenize_inplace(
    input: *mut u8,
    len: usize,
) -> Result<KVec<*mut u8>, &'static str> {
    let slice = unsafe { core::slice::from_raw_parts_mut(input, len) };
    let mut tokens = KVec::new();
    let mut i = 0;

    while i < len {
        let c = slice[i];
        if c.is_ascii_whitespace() || c == b',' {
            i += 1;
            continue;
        }

        if c == b'#' {
            break;
        }

        if c == b'"' || c == b'\'' {
            let quote_char = c;
            i += 1;
            let start = i;
            let mut end = None;
            while i < len {
                if slice[i] == quote_char {
                    end = Some(i);
                    i += 1;
                    break;
                }
                i += 1;
            }
            if let Some(end_idx) = end {
                slice[end_idx] = b'\0';
                tokens
                    .push(&mut slice[start] as *mut u8, GFP_KERNEL)
                    .map_err(|_| "allocation failed")?;
            } else {
                return Err("unclosed quote");
            }
        } else {
            let start = i;
            let mut hit_comment = false;
            while i < len {
                let next_c = slice[i];
                if next_c.is_ascii_whitespace() || next_c == b',' {
                    break;
                }
                if next_c == b'#' {
                    hit_comment = true;
                    break;
                }
                i += 1;
            }
            if i < len {
                slice[i] = b'\0';
                i += 1;
            }
            tokens
                .push(&mut slice[start] as *mut u8, GFP_KERNEL)
                .map_err(|_| "allocation failed")?;
            if hit_comment {
                break;
            }
        }
    }
    Ok(tokens)
}

#[no_mangle]
pub unsafe extern "C" fn rust_ddebug_parse_query(
    query_str: *mut core::ffi::c_char,
    modname: *const core::ffi::c_char,
    query: *mut ddebug_query,
    modifiers: *mut flag_settings,
) -> core::ffi::c_int {
    if query_str.is_null() || query.is_null() || modifiers.is_null() {
        return code::EINVAL.to_errno();
    }

    unsafe {
        // Initialize output structures
        core::ptr::write(
            query,
            ddebug_query {
                filename: core::ptr::null(),
                module: core::ptr::null(),
                function: core::ptr::null(),
                format: core::ptr::null(),
                class_string: core::ptr::null(),
                first_lineno: 0,
                last_lineno: 0,
            },
        );
        core::ptr::write(modifiers, flag_settings { flags: 0, mask: 0 });

        let len = core::ffi::CStr::from_ptr(query_str).to_bytes().len();
        let tokens = match tokenize_inplace(query_str as *mut u8, len) {
            Ok(t) => t,
            Err(_) => return code::EINVAL.to_errno(),
        };

        if tokens.is_empty() {
            return code::EINVAL.to_errno();
        }

        // The flags token is strictly the last token
        let flag_idx = tokens.len() - 1;

        // Parse standard match-spec pairs before the flags token
        let query_tokens = &tokens[..flag_idx];
        if query_tokens.len() % 2 != 0 {
            pr_err!("expecting pairs of match-spec <value>\n");
            return code::EINVAL.to_errno();
        }

        let q = &mut *query;

        for i in (0..query_tokens.len()).step_by(2) {
            let kw_ptr = query_tokens[i];
            let arg_ptr = query_tokens[i + 1];

            let kw = match core::ffi::CStr::from_ptr(
                kw_ptr as *const core::ffi::c_char,
            )
            .to_str()
            {
                Ok(s) => s,
                Err(_) => return code::EINVAL.to_errno(),
            };

            match kw {
                "func" => {
                    if !q.function.is_null() {
                        return code::EINVAL.to_errno();
                    }
                    q.function = arg_ptr as *const core::ffi::c_char;
                }
                "file" => {
                    if !q.filename.is_null() {
                        return code::EINVAL.to_errno();
                    }
                    q.filename = arg_ptr as *const core::ffi::c_char;
                    if let Err(_) = split_file_tail(arg_ptr, q) {
                        return code::EINVAL.to_errno();
                    }
                }
                "module" => {
                    if !q.module.is_null() {
                        return code::EINVAL.to_errno();
                    }
                    q.module = arg_ptr as *const core::ffi::c_char;
                }
                "format" => {
                    if !q.format.is_null() {
                        return code::EINVAL.to_errno();
                    }
                    unescape_inplace(arg_ptr);
                    q.format = arg_ptr as *const core::ffi::c_char;
                }
                "line" => {
                    let arg_str = match core::ffi::CStr::from_ptr(
                        arg_ptr as *const core::ffi::c_char,
                    )
                    .to_str()
                    {
                        Ok(s) => s,
                        Err(_) => return code::EINVAL.to_errno(),
                    };
                    if let Err(_) = parse_linerange(arg_str, q) {
                        return code::EINVAL.to_errno();
                    }
                }
                "class" => {
                    if !q.class_string.is_null() {
                        return code::EINVAL.to_errno();
                    }
                    q.class_string = arg_ptr as *const core::ffi::c_char;
                }
                _ => {
                    pr_err!("unknown keyword \"{}\"\n", kw);
                    return code::EINVAL.to_errno();
                }
            }
        }

        if q.module.is_null() && !modname.is_null() {
            q.module = modname;
        }

        // Parse the flags token
        let flag_str = match core::ffi::CStr::from_ptr(
            tokens[flag_idx] as *const core::ffi::c_char,
        )
        .to_str()
        {
            Ok(s) => s,
            Err(_) => return code::EINVAL.to_errno(),
        };

        if let Err(_) = parse_flags(flag_str, &mut *modifiers) {
            return code::EINVAL.to_errno();
        }
    }

    0
}
