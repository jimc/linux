// SPDX-License-Identifier: GPL-2.0-only
/// Find structural alignment mismatches where a variable instantiated in an
/// ELF section is explicitly aligned, but its underlying structure definition
/// lacks the same alignment attribute.
///
/// This is a critical systems-correctness check: if a macro forces __aligned(8)
/// on variable instantiations inside a custom linker section (such as
/// __section("__dyndbg_classes")), but the structure definition (the type)
/// itself lacks the matching __aligned(8) attribute, the compiler calculates
/// the type size as its natural alignment (e.g., 12 or 28 bytes on 32-bit i386)
/// instead of the aligned footprint (16 or 32 bytes).
///
/// When the execution engine walks the array of section variables (via pointer
/// arithmetic e.g. ptr++), the compiled code indexes at the unaligned step-size
/// while the linker physically placed them at aligned offsets. This mismatch
/// leads to silent stride offset shifts, reading trailing zero-padding bytes
/// as valid pointers (EAX: 00000000), resulting in boot-time NULL pointer
/// dereference crashes inside strcmp().
///
/// This script analyzes standard C variable declarations, DECLARE_DYNDBG_CLASSMAP
/// macros, and DYNAMIC_DEBUG_CLASSMAP_DEFINE/USE macros to ensure their
/// structural definitions natively carry the matching alignment constraints.
///
// Confidence: High
// Copyright: (C) 2026 Jim Cromie <jim.cromie@gmail.com>
// URL: https://coccinelle.gitlabpages.inria.fr/website/

virtual report

// ==========================================
// PATH A: Standard variable declarations
// ==========================================

@r_var_std@
identifier struct_name;
identifier var_name;
position p;
@@

struct struct_name var_name@p;

@r_struct_std@
identifier r_var_std.struct_name;
position p2;
@@

struct struct_name@p2 {
    ...
};

@script:python r_py_std@
struct_name << r_var_std.struct_name;
var_name << r_var_std.var_name;
p_var << r_var_std.p;
p_struct << r_struct_std.p2;
@@

import coccilib.report as report

var_file = p_var[0].file
var_line = int(p_var[0].line)

with open(var_file, 'r') as f:
    var_lines = f.readlines()

decl_code = var_lines[var_line - 1]

if "__section" in decl_code and "__aligned" in decl_code:
    struct_file = p_struct[0].file
    start_line = int(p_struct[0].line)

    with open(struct_file, 'r') as f:
        struct_lines = f.readlines()

    struct_code = "".join(struct_lines[start_line - 1 : start_line + 40])
    if "__aligned" not in struct_code and "aligned(" not in struct_code:
        msg = "WARNING: variable '%s' of struct %s is placed in an aligned section, but the struct definition at %s:%s lacks native __aligned attribute!" % (var_name, struct_name, struct_file, start_line)
        report.print_report(p_var[0], msg)


// ==========================================
// PATH B: Macro-based DECLARE declarations (master)
// ==========================================

@r_var_macro_declare@
identifier var_name;
declarer name DECLARE_DYNDBG_CLASSMAP;
position p;
@@

DECLARE_DYNDBG_CLASSMAP(var_name@p, ...);

// On master, the struct is called ddebug_class_map (without underscore)
@r_struct_macro_declare@
position p2;
@@

struct ddebug_class_map@p2 {
    ...
};

@script:python r_py_macro_declare@
var_name << r_var_macro_declare.var_name;
p_var << r_var_macro_declare.p;
p_struct << r_struct_macro_declare.p2;
@@

import coccilib.report as report

struct_file = p_struct[0].file
start_line = int(p_struct[0].line)

with open(struct_file, 'r') as f:
    struct_lines = f.readlines()

struct_code = "".join(struct_lines[start_line - 1 : start_line + 40])
if "__aligned" not in struct_code and "aligned(" not in struct_code:
    msg = "WARNING: variable '%s' instantiated via DECLARE_DYNDBG_CLASSMAP has aligned variables, but the struct ddebug_class_map definition at %s:%s lacks native __aligned attribute!" % (var_name, struct_file, start_line)
    report.print_report(p_var[0], msg)


// ==========================================
// PATH C: Macro-based DEFINE / USE declarations (topic)
// ==========================================

@r_var_macro_define@
identifier var_name;
declarer name DYNAMIC_DEBUG_CLASSMAP_DEFINE, DYNAMIC_DEBUG_CLASSMAP_USE;
position p;
@@

(
DYNAMIC_DEBUG_CLASSMAP_DEFINE(var_name@p, ...);
|
DYNAMIC_DEBUG_CLASSMAP_USE(var_name@p, ...);
)

// On topic branch, the struct is called _ddebug_class_map
@r_struct_macro_define@
position p2;
@@

struct _ddebug_class_map@p2 {
    ...
};

@script:python r_py_macro_define@
var_name << r_var_macro_define.var_name;
p_var << r_var_macro_define.p;
p_struct << r_struct_macro_define.p2;
@@

import coccilib.report as report

struct_file = p_struct[0].file
start_line = int(p_struct[0].line)

with open(struct_file, 'r') as f:
    struct_lines = f.readlines()

struct_code = "".join(struct_lines[start_line - 1 : start_line + 40])
if "__aligned" not in struct_code and "aligned(" not in struct_code:
    msg = "WARNING: variable '%s' instantiated via DYNAMIC_DEBUG_CLASSMAP has aligned variables, but the struct _ddebug_class_map definition at %s:%s lacks native __aligned attribute!" % (var_name, struct_file, start_line)
    report.print_report(p_var[0], msg)
