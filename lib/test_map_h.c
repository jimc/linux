// SPDX-License-Identifier: GPL-2.0-only
/*
 * test include/linux/map.h macros: MAP_LIST & MAP_FN
 *
 * Authors:
 *      Jim Cromie	<jim.cromie@gmail.com>
 */

#include <linux/map.h>
#include <linux/module.h>

/* compile-time test MAP_FN */
static int sum = MAP_FN( +, 1, 2, 3);
static char* caten = MAP_FN(__stringify, YES, NO, MAYBE);

/* client/user defines enum NAMES as source of truth */
enum client_categories { DRMx_CORE, DRMx_DRIVER, DRMx_KMS };

/* test MAP_LIST(__stringify, ...). others deemed equivalent */
#define STRVEC_FROM_ENUM_VALS_(_vec_, ...)				\
	const char *_vec_##_names[] = {					\
		MAP_LIST(__stringify, ##__VA_ARGS__) }

static STRVEC_FROM_ENUM_VALS_(debug_cats, DRMx_CORE, DRMx_DRIVER, DRMx_KMS);

#define BUILD_BUG_STREQ(_var, ref)					\
	BUILD_BUG_ON(__builtin_memcmp(_var, ref, __builtin_strlen(ref)))
#define BUILD_BUG_STREQ_VI(_vec_, idx, ref)		\
	BUILD_BUG_STREQ(_vec_##_names[idx], ref)

/* force compile, even if its not called */
static void MAPH_BUILD_BUG(void)
{
	BUILD_BUG_ON(sum != 6);
	BUILD_BUG_STREQ(caten, "YESNOMAYBE");

	BUILD_BUG_STREQ_VI(debug_cats, 0, "DRMx_CORE");
	BUILD_BUG_STREQ_VI(debug_cats, 1, "DRMx_DRIVER");
	BUILD_BUG_STREQ_VI(debug_cats, 2, "DRMx_KMS");

#ifdef MAPH_FORCE_FAIL
	/* either breaks compile */
	BUILD_BUG_ON(sum != 8);
	BUILD_BUG_STREQ(debug_cats, 1, "'not-in-map'");
#endif
}

#define rTEST_eq(_v_, _ref)						\
	({								\
		if (strcmp(_v_, _ref)) {				\
			pr_warn("failed: %s eq %s\n", _v_, _ref);	\
		} else {						\
			pr_debug("ok: %s eq %s\n", _v_, _ref);		\
		}							\
	})
#define rTEST_eq_VI(_v_, _i, _ref)	rTEST_eq(_v_##_names[_i], _ref)

static int __init test_maph_init(void)
{
	pr_debug("init start\n");

	rTEST_eq(caten, "YESNOMAYBE");
	rTEST_eq_VI(debug_cats, 0, "DRMx_CORE");
	rTEST_eq_VI(debug_cats, 1, "DRMx_DRIVER");
	rTEST_eq_VI(debug_cats, 2, "DRMx_KMS");

	/* force fail and ignore */
	rTEST_eq_VI(debug_cats, 1, "'not-in-map'");
	return 0;
}

static void __exit test_maph_exit(void)
{
	pr_debug("exited\n");
}

module_init(test_maph_init);
module_exit(test_maph_exit);

MODULE_AUTHOR("Jim Cromie <jim.cromie@gmail.com>");
MODULE_LICENSE("GPL");
