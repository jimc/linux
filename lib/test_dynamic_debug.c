// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel module for testing dynamic_debug
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

static void do_prints(void); /* device under test */
static int param_set_do_prints(const char *instr, const struct kernel_param *kp)
{
	do_prints();
	return 0;
}
static int param_get_do_prints(char *buffer, const struct kernel_param *kp)
{
	do_prints();
	return scnprintf(buffer, PAGE_SIZE, "did do_prints\n");
}
static const struct kernel_param_ops param_ops_do_prints = {
	.set = param_set_do_prints,
	.get = param_get_do_prints,
};
module_param_cb(do_prints, &param_ops_do_prints, NULL, 0600);

#define CLASSMAP_BITMASK(width, base) (((1UL << (width)) - 1) << base)

/* sysfs param wrapper, proto-API */
#define DYNDBG_CLASSMAP_PARAM_(_model, _flags, _init)			\
	static unsigned long bits_##_model = _init;			\
	static struct ddebug_class_param _flags##_##_model = {		\
		.bits = &bits_##_model,					\
		.flags = #_flags,					\
		.map = &map_##_model,					\
	};								\
	module_param_cb(_flags##_##_model, &param_ops_dyndbg_classes,	\
			&_flags##_##_model, 0600)
#ifdef DEBUG
#define DYNDBG_CLASSMAP_PARAM(_model, _flags)  DYNDBG_CLASSMAP_PARAM_(_model, _flags, ~0)
#else
#define DYNDBG_CLASSMAP_PARAM(_model, _flags)  DYNDBG_CLASSMAP_PARAM_(_model, _flags, 0)
#endif

/*
 * Demonstrate/test all 4 class-typed classmaps with a sys-param.
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
 * Declare all 4 enums now, for different types
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

/* recapitulate DRM's parent(drm.ko) <-- _submod(drivers,helpers) */
#if !defined(TEST_DYNAMIC_DEBUG_SUBMOD)
/*
 * In single user, or parent / coordinator (drm.ko) modules, define
 * classmaps on the client enums above, and then declares the PARAMS
 * ref'g the classmaps.  Each is exported.
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
 * now add the sysfs-params
 */

DYNDBG_CLASSMAP_PARAM(disjoint_bits, p);
DYNDBG_CLASSMAP_PARAM(level_num, p);

#else /* TEST_DYNAMIC_DEBUG_SUBMOD */

/*
 * in submod/drm-drivers, use the classmaps defined in top/parent
 * module above.
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

static void do_prints(void)
{
	pr_debug("do_prints:\n");
	do_cats();
	do_levels();
}

static int __init test_dynamic_debug_init(void)
{
	pr_debug("init start\n");
	do_prints();
	pr_debug("init done\n");
	return 0;
}

static void __exit test_dynamic_debug_exit(void)
{
	pr_debug("exited\n");
}

module_init(test_dynamic_debug_init);
module_exit(test_dynamic_debug_exit);

MODULE_AUTHOR("Jim Cromie <jim.cromie@gmail.com>");
MODULE_DESCRIPTION("Kernel module for testing dynamic_debug");
MODULE_LICENSE("GPL");
