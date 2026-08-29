// SPDX-License-Identifier: GPL-2.0+
/*
 * Unit tests & benchmarks for Potted Bonsai Range Tree
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/bonsai_tree.h>
#include <linux/ktime.h>

static int __init test_bonsai_init(void)
{
	struct bonsai_tree bt = BONSAI_TREE_INIT;
	unsigned long i;
	void *val;
	int ret;
	ktime_t t0, t1;
	u64 ns_insert, ns_lookup;

	pr_info("test_bonsai: Starting Bonsai Tree unit tests...\n");

	/* Test 1: Initialize in small 4KB pot (order-0) */
	ret = bonsai_init(&bt, 0, GFP_KERNEL);
	if (ret) {
		pr_err("test_bonsai: bonsai_init failed (%d)\n", ret);
		return ret;
	}

	/* Test 2: Insert 100 sequential ranges (triggers splits and auto-repotting) */
	t0 = ktime_get();
	for (i = 0; i < 100; i++) {
		ret = bonsai_store(&bt, i * 10, (void *)(i + 1), GFP_KERNEL);
		if (ret) {
			pr_err("test_bonsai: store failed at key %lu (%d)\n", i * 10, ret);
			bonsai_destroy(&bt);
			return ret;
		}
	}
	t1 = ktime_get();
	ns_insert = ktime_to_ns(t1 - t0);

	pr_info("test_bonsai: Stored 100 ranges in %llu ns (height=%u, nodes=%u, pot_order=%u)\n",
		ns_insert, bt.height, bt.node_count, bt.pot_order);

	/* Test 3: Lookup verification across all keys */
	t0 = ktime_get();
	for (i = 0; i < 100; i++) {
		val = bonsai_lookup(&bt, i * 10);
		if (val != (void *)(i + 1)) {
			pr_err("test_bonsai: lookup mismatch at key %lu (got %p, expected %lu)\n",
			       i * 10, val, i + 1);
			bonsai_destroy(&bt);
			return -EINVAL;
		}
	}
	t1 = ktime_get();
	ns_lookup = ktime_to_ns(t1 - t0);

	pr_info("test_bonsai: 100 lookups verified in %llu ns (avg %llu ns/lookup)\n",
		ns_lookup, ns_lookup / 100);

	/* Test 4: Explicit Repotting Test */
	ret = bonsai_repot(&bt, bt.pot_order + 1, GFP_KERNEL);
	if (ret) {
		pr_err("test_bonsai: explicit bonsai_repot failed (%d)\n", ret);
		bonsai_destroy(&bt);
		return ret;
	}
	pr_info("test_bonsai: Repotted tree to order-%u successfully\n", bt.pot_order);

	/* Verify lookups still match post-repot */
	for (i = 0; i < 100; i++) {
		val = bonsai_lookup(&bt, i * 10);
		if (val != (void *)(i + 1)) {
			pr_err("test_bonsai: post-repot lookup failed at key %lu\n", i * 10);
			bonsai_destroy(&bt);
			return -EINVAL;
		}
	}

	/* Test 5: O(1) Bulk Teardown */
	bonsai_destroy(&bt);
	pr_info("test_bonsai: Destroyed tree in O(1). All unit tests PASSED!\n");

	return 0;
}

static void __exit test_bonsai_exit(void)
{
}

module_init(test_bonsai_init);
module_exit(test_bonsai_exit);

MODULE_AUTHOR("Jim Cromie <jim.cromie@gmail.com>");
MODULE_DESCRIPTION("Bonsai Tree Unit Tests & Microbenchmarks");
MODULE_LICENSE("GPL");
