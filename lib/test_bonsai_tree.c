// SPDX-License-Identifier: GPL-2.0+
/*
 * Unit tests & side-by-side A/B benchmarks for Potted Bonsai Range Tree
 * vs Standard Maple Tree.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/bonsai_tree.h>
#include <linux/maple_tree.h>
#include <linux/ktime.h>

#define BENCH_ENTRIES	150
#define BENCH_LOOKUPS	1000000

static void bench_ab_comparison(void)
{
	struct maple_tree mt;
	struct bonsai_tree bt = BONSAI_TREE_INIT;
	unsigned long i;
	void *val;
	ktime_t t0, t1;
	u64 mt_insert_ns, mt_lookup_ns, mt_destroy_ns;
	u64 bt_insert_ns, bt_lookup_ns, bt_destroy_ns;

	pr_info("\n=== Bonsai vs Maple Tree A/B Benchmark (Entries: %d, Lookups: %d) ===\n",
		BENCH_ENTRIES, BENCH_LOOKUPS);

	/* --------------------------------------------------------- */
	/* Pass A: Standard Maple Tree                               */
	/* --------------------------------------------------------- */
	mt_init_flags(&mt, MT_FLAGS_ALLOC_RANGE);

	/* Insertions */
	t0 = ktime_get();
	for (i = 0; i < BENCH_ENTRIES; i++)
		mtree_store_range(&mt, i * 10, i * 10, xa_mk_value(i + 1), GFP_KERNEL);
	t1 = ktime_get();
	mt_insert_ns = ktime_to_ns(t1 - t0);

	/* 1,000,000 Lookups */
	t0 = ktime_get();
	for (i = 0; i < BENCH_LOOKUPS; i++) {
		val = mtree_load(&mt, (i % BENCH_ENTRIES) * 10);
		if (unlikely(!val && i < BENCH_ENTRIES))
			pr_err("bench: maple_tree lookup failed for key %lu\n", (i % BENCH_ENTRIES) * 10);
	}
	t1 = ktime_get();
	mt_lookup_ns = ktime_to_ns(t1 - t0);

	/* Teardown */
	t0 = ktime_get();
	mtree_destroy(&mt);
	t1 = ktime_get();
	mt_destroy_ns = ktime_to_ns(t1 - t0);

	/* --------------------------------------------------------- */
	/* Pass B: Potted Bonsai Tree (25-way fanout + repotting)    */
	/* --------------------------------------------------------- */
	bonsai_init(&bt);

	/* Insertions */
	t0 = ktime_get();
	for (i = 0; i < BENCH_ENTRIES; i++) {
		int ret = bonsai_store(&bt, i * 10, (void *)(i + 1), GFP_KERNEL);
		if (unlikely(ret))
			pr_err("bench: bonsai store failed at %lu: %d\n", i * 10, ret);
	}
	t1 = ktime_get();
	bt_insert_ns = ktime_to_ns(t1 - t0);

	/* 1,000,000 Lookups */
	t0 = ktime_get();
	for (i = 0; i < BENCH_LOOKUPS; i++) {
		unsigned long k = (i % BENCH_ENTRIES) * 10;
		val = bonsai_lookup(&bt, k);
		if (unlikely(!val)) {
			if (i < BENCH_ENTRIES)
				pr_err("bench: bonsai lookup failed for key %lu\n", k);
		}
	}
	t1 = ktime_get();
	bt_lookup_ns = ktime_to_ns(t1 - t0);

	/* Teardown */
	t0 = ktime_get();
	bonsai_destroy(&bt);
	t1 = ktime_get();
	bt_destroy_ns = ktime_to_ns(t1 - t0);

	/* --------------------------------------------------------- */
	/* Results Summary                                           */
	/* --------------------------------------------------------- */
	pr_info("Phase           | Maple Tree       | Bonsai Tree      | Delta        | %% Delta\n");
	pr_info("----------------+------------------+------------------+--------------+---------\n");
	pr_info("Insert (%d)    | %13llu ns | %13llu ns | %+10lld ns | %+6lld%%\n",
		BENCH_ENTRIES, mt_insert_ns, bt_insert_ns,
		(s64)(bt_insert_ns - mt_insert_ns),
		mt_insert_ns ? ((s64)(bt_insert_ns - mt_insert_ns) * 100) / (s64)mt_insert_ns : 0);
	pr_info("Lookup (1M)     | %13llu ns | %13llu ns | %+10lld ns | %+6lld%%\n",
		mt_lookup_ns, bt_lookup_ns,
		(s64)(bt_lookup_ns - mt_lookup_ns),
		mt_lookup_ns ? ((s64)(bt_lookup_ns - mt_lookup_ns) * 100) / (s64)mt_lookup_ns : 0);
	pr_info("Teardown (O(1)) | %13llu ns | %13llu ns | %+10lld ns | %+6lld%%\n",
		mt_destroy_ns, bt_destroy_ns,
		(s64)(bt_destroy_ns - mt_destroy_ns),
		mt_destroy_ns ? ((s64)(bt_destroy_ns - mt_destroy_ns) * 100) / (s64)mt_destroy_ns : 0);
	pr_info("==============================================================================\n\n");
}

static int __init test_bonsai_init(void)
{
	struct bonsai_tree bt = BONSAI_TREE_INIT;
	unsigned long i;
	void *val;
	int ret;

	pr_info("test_bonsai: Starting Bonsai Tree unit tests...\n");

	/* Test 1: Initialize bonsai tree */
	bonsai_init(&bt);

	/* Test 2: Insert 100 sequential ranges (triggers splits and auto-repotting) */
	for (i = 0; i < 100; i++) {
		ret = bonsai_store(&bt, i * 10, xa_mk_value(i + 1), GFP_KERNEL);
		if (ret) {
			pr_err("test_bonsai: store failed at key %lu (%d)\n", i * 10, ret);
			bonsai_destroy(&bt);
			return ret;
		}
	}

	/* Test 3: Lookup verification across all keys */
	for (i = 0; i < 100; i++) {
		val = bonsai_lookup(&bt, i * 10);
		if (val != xa_mk_value(i + 1)) {
			pr_err("test_bonsai: lookup mismatch at key %lu (got %p, expected %lu)\n",
			       i * 10, val, i + 1);
			bonsai_destroy(&bt);
			return -EINVAL;
		}
	}

	/* Test 4: Explicit Repotting Test */
	ret = bonsai_repot(&bt, 0, GFP_KERNEL);
	if (ret) {
		pr_err("test_bonsai: explicit bonsai_repot failed (%d)\n", ret);
		bonsai_destroy(&bt);
		return ret;
	}

	/* Verify lookups still match post-repot */
	for (i = 0; i < 100; i++) {
		val = bonsai_lookup(&bt, i * 10);
		if (val != xa_mk_value(i + 1)) {
			pr_err("test_bonsai: post-repot lookup failed at key %lu\n", i * 10);
			bonsai_destroy(&bt);
			return -EINVAL;
		}
	}

	/* Test 5: Bonsai -> Maple Tree Graduation */
	{
		struct maple_tree mt_grad;
		mt_init_flags(&mt_grad, MT_FLAGS_ALLOC_RANGE);
		ret = bonsai_to_maple(&bt, &mt_grad, GFP_KERNEL);
		if (ret) {
			pr_err("test_bonsai: bonsai_to_maple failed (%d)\n", ret);
			bonsai_destroy(&bt);
			return ret;
		}
		for (i = 0; i < 100; i++) {
			val = mtree_load(&mt_grad, i * 10);
			if (val != xa_mk_value(i + 1)) {
				pr_err("test_bonsai: graduated maple lookup failed at %lu\n", i * 10);
				mtree_destroy(&mt_grad);
				bonsai_destroy(&bt);
				return -EINVAL;
			}
		}
		mtree_destroy(&mt_grad);
		pr_info("test_bonsai: Bonsai -> Maple Tree graduation verified 100 entries\n");
	}

	/* Test 6: O(1) Bulk Teardown */
	bonsai_destroy(&bt);
	pr_info("test_bonsai: All unit tests PASSED!\n");

	/* Run side-by-side A/B comparison against standard Maple Tree */
	bench_ab_comparison();

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
