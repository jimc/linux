// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel module to test/demonstrate dynamic_debug features,
 * particularly classmaps and their support for subsystems like DRM.
 *
 * Authors:
 *      Jim Cromie	<jim.cromie@gmail.com>
 */

#if defined(TEST_DYNAMIC_DEBUG_SUBMOD)
  #define pr_fmt(fmt) "test_dd_submod: " fmt
#else
  #define pr_fmt(fmt) "test_dd: " fmt
#endif

#include <linux/module.h>

/* re-gen output by reading or writing sysfs node: do_prints */

static void do_prints(unsigned int); /* device under test */
static int param_set_do_prints(const char *instr, const struct kernel_param *kp)
{
	int rc;
	unsigned int ct;

	rc = kstrtouint(instr, 0, &ct);
	if (rc) {
		pr_err("expecting numeric input, using 1 instead\n");
		ct = 1;
	}
	do_prints(ct);
	return 0;
}
static int param_get_do_prints(char *buffer, const struct kernel_param *kp)
{
	do_prints(1);
	return scnprintf(buffer, PAGE_SIZE, "did 1 do_prints\n");
}
static const struct kernel_param_ops param_ops_do_prints = {
	.set = param_set_do_prints,
	.get = param_get_do_prints,
};
module_param_cb(do_prints, &param_ops_do_prints, NULL, 0600);

#define CLASSMAP_BITMASK(width, base) (((1UL << (width)) - 1) << (base))

/*
 * Demonstrate/test both types of classmaps, each with a sys-param.
 *
 * Each is 3 part: client-enum decl, _DEFINE, _PARAM.
 * Declare them in blocks to show patterns of use (repetitions and
 * changes) within each.
 *
 * 1st, dyndbg expects a client-provided enum-type as source of
 * category/classid truth.  DRM has DRM_UT_<CORE,DRIVER,KMS,etc>.
 *
 * Modules with multiple CLASSMAPS must have enums with distinct
 * value-ranges, arranged below with explicit enum_sym = X inits.
 *
 * Declare all enums now, for different types
 */

/* numeric input, independent bits */
enum cat_disjoint_bits {
	D2_CORE = 0,
	D2_DRIVER,
	D2_KMS,
	D2_PRIME,
	D2_ATOMIC,
	D2_VBL,
	D2_STATE,
	D2_LEASE,
	D2_DP,
	D2_DRMRES };

/* numeric verbosity, V2 > V1 related */
enum cat_level_num { V0 = 14, V1, V2, V3, V4, V5, V6, V7 };

/*
 * use/demonstrate multi-module-group classmaps, as for DRM
 */
#if !defined(TEST_DYNAMIC_DEBUG_SUBMOD)
/*
 * For module-groups of 1+, define classmaps with names (stringified
 * enum-symbols) copied from above. 1-to-1 mapping is recommended.
 * The classmap is exported, so that other modules in the group can
 * link to it and control their prdbgs.
 */
DYNDBG_CLASSMAP_DEFINE(map_disjoint_bits, DD_CLASS_TYPE_DISJOINT_BITS,
		       D2_CORE,
		       "D2_CORE",
		       "D2_DRIVER",
		       "D2_KMS",
		       "D2_PRIME",
		       "D2_ATOMIC",
		       "D2_VBL",
		       "D2_STATE",
		       "D2_LEASE",
		       "D2_DP",
		       "D2_DRMRES");

DYNDBG_CLASSMAP_DEFINE(map_level_num, DD_CLASS_TYPE_LEVEL_NUM,
		       V0, "V0", "V1", "V2", "V3", "V4", "V5", "V6", "V7");

/*
 * for use-cases that want it, provide a sysfs-param to set the
 * classes in the classmap.  It is at this interface where the
 * "v3>v2" property is applied to DD_CLASS_TYPE_LEVEL_NUM inputs.
 */
DYNDBG_CLASSMAP_PARAM(p_disjoint_bits,	map_disjoint_bits, p);
DYNDBG_CLASSMAP_PARAM(p_level_num,	map_level_num, p);

#else /* TEST_DYNAMIC_DEBUG_SUBMOD */
/*
 * the +1 members of a multi-module group refer to the classmap
 * DEFINEd (and exported) above.
 */
DYNDBG_CLASSMAP_USE(map_disjoint_bits);
DYNDBG_CLASSMAP_USE(map_level_num);

#endif

/* stand-in for all pr_debug etc */
#define prdbg(SYM) __pr_debug_cls(SYM, #SYM " msg\n")

static void do_cats(void)
{
	pr_debug("doing categories\n");

	prdbg(D2_CORE);
	prdbg(D2_DRIVER);
	prdbg(D2_KMS);
	prdbg(D2_PRIME);
	prdbg(D2_ATOMIC);
	prdbg(D2_VBL);
	prdbg(D2_STATE);
	prdbg(D2_LEASE);
	prdbg(D2_DP);
	prdbg(D2_DRMRES);
}

static void do_levels(void)
{
	pr_debug("doing levels\n");

	prdbg(V1);
	prdbg(V2);
	prdbg(V3);
	prdbg(V4);
	prdbg(V5);
	prdbg(V6);
	prdbg(V7);
}

static void do_prints(unsigned int ct)
{
	/* maybe clamp this */
	pr_debug("do-prints %d times:\n", ct);
	for (; ct; ct--) {
		do_cats();
		do_levels();
	}
}

static int __init test_dynamic_debug_init(void)
{
	pr_debug("init start\n");
	do_prints(1);
	pr_debug("init done\n");
	return 0;
}

static void __exit test_dynamic_debug_exit(void)
{
	pr_debug("exited\n");
}

module_init(test_dynamic_debug_init);
module_exit(test_dynamic_debug_exit);

MODULE_DESCRIPTION("test/demonstrate dynamic-debug features");
MODULE_AUTHOR("Jim Cromie <jim.cromie@gmail.com>");
MODULE_DESCRIPTION("Kernel module for testing dynamic_debug");
MODULE_LICENSE("GPL");
