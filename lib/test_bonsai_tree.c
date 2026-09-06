// SPDX-License-Identifier: GPL-2.0+
/*
 * Micro-benchmark: Bonsai Tree vs Maple Tree vs Binary Search
 *
 * Measures:
 * 0. Memory footprint (bytes allocated per interval count)
 * 1. Build / Ingestion throughput
 * 2. Lookup latency (Sequential & Random hits, ns/op)
 */

#define pr_fmt(fmt) "test_bonsai: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/bsearch.h>
#include <linux/compiler.h>
#include <linux/bonsai_tree.h>
#include <linux/maple_tree.h>

static bool do_stress = true;
module_param(do_stress, bool, 0444);
MODULE_PARM_DESC(do_stress, "Run stress and equivalence validation tests");

static bool do_bench = true;
module_param(do_bench, bool, 0444);
MODULE_PARM_DESC(do_bench, "Run micro-benchmark and latency telemetry");

static unsigned int max_scale = 16384;
module_param(max_scale, uint, 0444);
MODULE_PARM_DESC(max_scale, "Maximum scale (intervals) for stress/benchmark");

static unsigned int num_lookups = 1000000;
module_param(num_lookups, uint, 0444);
MODULE_PARM_DESC(num_lookups, "Number of lookups per scale in benchmark");

struct flat_interval {
	unsigned long start;
	unsigned long end;
	void *val;
};

static int cmp_flat_interval(const void *key, const void *elt)
{
	unsigned long addr = *(const unsigned long *)key;
	const struct flat_interval *inv = elt;

	if (addr < inv->start)
		return -1;
	if (addr > inv->end)
		return 1;
	return 0;
}

static void *bsearch_lookup(struct flat_interval *table, size_t nr, unsigned long key)
{
	struct flat_interval *res;

	res = bsearch(&key, table, nr, sizeof(*table), cmp_flat_interval);
	return res ? res->val : NULL;
}

struct bench_run {
	u64 build_ns;
	u64 seq_ns;
	u64 rnd_ns;
	size_t bytes;
	unsigned int node_count;
	unsigned int height;
};

#define DEFINE_TREE_BENCH(name, type, init_stmt, store_stmt, seal_stmt,		\
			  seq_lookup, rnd_lookup, destroy_stmt, post_seal)		\
static noinline void bench_##name(struct flat_interval *flat_table,			\
				  unsigned long *rnd_keys,				\
				  unsigned int num_intervals,				\
				  struct bench_run *res)				\
{											\
	type *tree;									\
	ktime_t t0, t1;									\
	unsigned int i, step = 16;							\
	void *sink = NULL;								\
											\
	tree = kzalloc(sizeof(*tree), GFP_KERNEL);					\
	if (!tree)									\
		return;									\
											\
	init_stmt;									\
	t0 = ktime_get();								\
	for (i = 0; i < num_intervals; i++) {						\
		unsigned long start = flat_table[i].start;				\
		unsigned long end = flat_table[i].end;					\
		void *val = flat_table[i].val;						\
											\
		store_stmt;								\
	}										\
	seal_stmt;									\
	t1 = ktime_get();								\
	res->build_ns = ktime_to_ns(ktime_sub(t1, t0));					\
	post_seal;									\
											\
	t0 = ktime_get();								\
	for (i = 0; i < num_lookups; i++) {						\
		unsigned long key = (i % num_intervals) * step + 4;			\
											\
		sink = seq_lookup;							\
		OPTIMIZER_HIDE_VAR(sink);						\
	}										\
	t1 = ktime_get();								\
	res->seq_ns = ktime_to_ns(ktime_sub(t1, t0));					\
											\
	t0 = ktime_get();								\
	for (i = 0; i < num_lookups; i++) {						\
		sink = rnd_lookup;							\
		OPTIMIZER_HIDE_VAR(sink);						\
	}										\
	t1 = ktime_get();								\
	res->rnd_ns = ktime_to_ns(ktime_sub(t1, t0));					\
											\
	destroy_stmt;									\
	kfree(tree);									\
}

static void bonsai_bench_init(struct bonsai_tree *tree, unsigned int num_intervals)
{
	bonsai_init(tree);
	bonsai_init_hint(tree, num_intervals, GFP_KERNEL);
}

static void bonsai_bench_post(struct bonsai_tree *tree, struct bench_run *res)
{
	res->bytes = tree->node_count * BONSAI_NODE_SIZE;
	res->node_count = tree->node_count;
	res->height = tree->height;
}

static void maple_bench_post(unsigned int num_intervals, struct bench_run *res)
{
	res->bytes = (num_intervals / 10 + 1) * 256;
	res->node_count = 0;
	res->height = 0;
}

DEFINE_TREE_BENCH(bonsai, struct bonsai_tree,
		  bonsai_bench_init(tree, num_intervals),
		  bonsai_store_range(tree, start, end, val, GFP_KERNEL),
		  bonsai_seal(tree),
		  bonsai_lookup(tree, key),
		  bonsai_lookup(tree, rnd_keys[i & 1023]),
		  bonsai_destroy(tree),
		  bonsai_bench_post(tree, res))

static void maple_bench_seal(void)
{
}

DEFINE_TREE_BENCH(maple, struct maple_tree,
		  mt_init_flags(tree, 0),
		  mtree_store_range(tree, start, end, val, GFP_KERNEL),
		  maple_bench_seal(),
		  mtree_load(tree, key),
		  mtree_load(tree, rnd_keys[i & 1023]),
		  mtree_destroy(tree),
		  maple_bench_post(num_intervals, res))

static void benchmark_scale(unsigned int num_intervals)
{
	struct flat_interval *flat_table;
	struct bench_run bonsai_res = {}, maple_res = {};
	ktime_t t0, t1;
	u64 bsearch_seq_ns, bsearch_rnd_ns;
	unsigned long *rnd_keys;
	unsigned int i, step = 16;
	void *sink = NULL;
	size_t flat_bytes;

	flat_table = kmalloc_array(num_intervals, sizeof(*flat_table), GFP_KERNEL);
	rnd_keys = kmalloc_array(1024, sizeof(*rnd_keys), GFP_KERNEL);
	if (!flat_table || !rnd_keys) {
		kfree(flat_table);
		kfree(rnd_keys);
		pr_err("failed to allocate test buffers for N=%u\n", num_intervals);
		return;
	}

	for (i = 0; i < num_intervals; i++) {
		flat_table[i].start = (unsigned long)i * step;
		flat_table[i].end = flat_table[i].start + step - 1;
		flat_table[i].val = (void *)(unsigned long)(i + 0x1000);
	}
	for (i = 0; i < 1024; i++) {
		unsigned int idx = get_random_u32_below(num_intervals);

		rnd_keys[i] = flat_table[idx].start + get_random_u32_below(step);
	}

	flat_bytes = num_intervals * sizeof(struct flat_interval);

	/* 0. Benchmark Bonsai & Maple Trees */
	bench_bonsai(flat_table, rnd_keys, num_intervals, &bonsai_res);
	bench_maple(flat_table, rnd_keys, num_intervals, &maple_res);

	/* 1. Sequential Bsearch Lookup */
	t0 = ktime_get();
	for (i = 0; i < num_lookups; i++) {
		unsigned long key = (i % num_intervals) * step + 4;

		sink = bsearch_lookup(flat_table, num_intervals, key);
		OPTIMIZER_HIDE_VAR(sink);
	}
	t1 = ktime_get();
	bsearch_seq_ns = ktime_to_ns(ktime_sub(t1, t0));

	/* 2. Random Bsearch Lookup */
	t0 = ktime_get();
	for (i = 0; i < num_lookups; i++) {
		sink = bsearch_lookup(flat_table, num_intervals, rnd_keys[i & 1023]);
		OPTIMIZER_HIDE_VAR(sink);
	}
	t1 = ktime_get();
	bsearch_rnd_ns = ktime_to_ns(ktime_sub(t1, t0));

	{
		unsigned int lookups = num_lookups ? num_lookups : 1;

		pr_info("=== Benchmark N = %5u intervals (%u lookups) ===\n",
			num_intervals, num_lookups);
		pr_info("  Memory   : Flat=%zu B | Bonsai=%zu B (nodes=%u, h=%u) | Maple=~%zu B\n",
			flat_bytes, bonsai_res.bytes, bonsai_res.node_count,
			bonsai_res.height, maple_res.bytes);
		pr_info("  Build    : Bonsai=%llu us | Maple=%llu us\n",
			bonsai_res.build_ns / 1000, maple_res.build_ns / 1000);
		pr_info("  Seq Look : BSearch=%llu ns/op | Bonsai=%llu ns/op | Maple=%llu ns/op\n",
			bsearch_seq_ns / lookups,
			bonsai_res.seq_ns / lookups,
			maple_res.seq_ns / lookups);
		pr_info("  Rnd Look : BSearch=%llu ns/op | Bonsai=%llu ns/op | Maple=%llu ns/op\n",
			bsearch_rnd_ns / lookups,
			bonsai_res.rnd_ns / lookups,
			maple_res.rnd_ns / lookups);
	}

	kfree(flat_table);
	kfree(rnd_keys);
}

static int test_bonsai_invalidation(void)
{
	struct bonsai_tree *bt;
	void *res;
	int ret, errs = 0;

	bt = kzalloc_obj(*bt, GFP_KERNEL);
	if (!bt)
		return -ENOMEM;

	bonsai_init(bt);
	bonsai_store_range(bt, 100, 199, (void *)0x1111, GFP_KERNEL);
	bonsai_store_range(bt, 200, 299, (void *)0x2222, GFP_KERNEL);
	bonsai_store_range(bt, 300, 399, (void *)0x3333, GFP_KERNEL);

	res = bonsai_lookup(bt, 250);
	if (res != (void *)0x2222) {
		pr_err("invalidation test: lookup before failed (%p)\n", res);
		errs++;
	}

	ret = bonsai_invalidate(bt, 250);
	if (ret) {
		pr_err("invalidation test: valid invalidate failed (%d)\n", ret);
		errs++;
	}

	res = bonsai_lookup(bt, 250);
	if (res) {
		pr_err("invalidation test: lookup after failed (%p)\n", res);
		errs++;
	}

	/* Double invalidation should return -EALREADY */
	ret = bonsai_invalidate(bt, 250);
	if (ret != -EALREADY) {
		pr_err("invalidation test: double invalidate expected -EALREADY, got %d\n", ret);
		errs++;
	}

	/* Out of bounds invalidation should return -ENOENT */
	ret = bonsai_invalidate(bt, 500);
	if (ret != -ENOENT) {
		pr_err("invalidation test: oob invalidate expected -ENOENT, got %d\n", ret);
		errs++;
	}

	res = bonsai_lookup(bt, 150);
	if (res != (void *)0x1111) {
		pr_err("invalidation test: adjacent slot 1 corrupted (%p)\n", res);
		errs++;
	}

	res = bonsai_lookup(bt, 350);
	if (res != (void *)0x3333) {
		pr_err("invalidation test: adjacent slot 3 corrupted (%p)\n", res);
		errs++;
	}

	bonsai_destroy(bt);
	kfree(bt);

	if (errs) {
		pr_err("invalidation test: FAIL (%d errors)\n", errs);
		return -EINVAL;
	}

	pr_info("invalidation test: PASS\n");
	return 0;
}

static int test_bonsai_maple_stress_equivalence(unsigned int num_intervals)
{
	struct flat_interval *flat_table;
	struct bonsai_tree *bt, *bt_unhinted;
	struct maple_tree mt;
	unsigned int i, step = 16, mismatches = 0;

	flat_table = kmalloc_array(num_intervals, sizeof(*flat_table), GFP_KERNEL);
	bt = kzalloc_obj(*bt, GFP_KERNEL);
	bt_unhinted = kzalloc_obj(*bt_unhinted, GFP_KERNEL);
	if (!flat_table || !bt || !bt_unhinted) {
		kfree(flat_table);
		kfree(bt);
		kfree(bt_unhinted);
		return -ENOMEM;
	}

	bonsai_init(bt);
	bonsai_init_hint(bt, num_intervals, GFP_KERNEL);
	bonsai_init(bt_unhinted);
	mt_init_flags(&mt, 0);

	for (i = 0; i < num_intervals; i++) {
		flat_table[i].start = (unsigned long)i * step;
		flat_table[i].end = flat_table[i].start + step - 1;
		flat_table[i].val = (void *)(unsigned long)(i + 0x1000);

		bonsai_store_range(bt, flat_table[i].start, flat_table[i].end,
				   flat_table[i].val, GFP_KERNEL);
		bonsai_store_range(bt_unhinted, flat_table[i].start,
				   flat_table[i].end, flat_table[i].val,
				   GFP_KERNEL);
		mtree_store_range(&mt, flat_table[i].start, flat_table[i].end,
				  flat_table[i].val, GFP_KERNEL);
	}

	bonsai_seal(bt);

	/* 0. Exhaustive Boundary Cross-Validation */
	for (i = 0; i < num_intervals; i++) {
		unsigned long s = flat_table[i].start;
		unsigned long e = flat_table[i].end;
		unsigned long m = s + (step / 2);
		void *expected = flat_table[i].val;
		void *b_s, *b_e, *b_m;
		void *m_s, *m_e, *m_m;
		void *u_m;

		b_s = bonsai_lookup(bt, s);
		b_e = bonsai_lookup(bt, e);
		b_m = bonsai_lookup(bt, m);

		m_s = mtree_load(&mt, s);
		m_e = mtree_load(&mt, e);
		m_m = mtree_load(&mt, m);

		u_m = bonsai_lookup(bt_unhinted, m);

		if (b_s != expected || b_e != expected || b_m != expected ||
		    m_s != expected || m_e != expected || m_m != expected ||
		    u_m != expected) {
			mismatches++;
			if (mismatches <= 5)
				pr_err("boundary mismatch slot %u: exp=%p bs=%p ms=%p bm=%p mm=%p be=%p me=%p\n",
				       i, expected, b_s, m_s, b_m, m_m, b_e, m_e);
		}
	}

	/* 1. High-Density Randomized Probe Stress (25k probes) */
	for (i = 0; i < 25000; i++) {
		unsigned long key = get_random_u32_below(num_intervals * step);
		void *b_val = bonsai_lookup(bt, key);
		void *m_val = mtree_load(&mt, key);
		void *u_val = bonsai_lookup(bt_unhinted, key);
		void *f_val = bsearch_lookup(flat_table, num_intervals, key);

		if (b_val != m_val || b_val != u_val || b_val != f_val) {
			mismatches++;
			if (mismatches <= 5)
				pr_err("rnd mismatch key %lu: b=%p m=%p u=%p f=%p\n",
				       key, b_val, m_val, u_val, f_val);
		}
	}

	/* 2. Invalidation & Erasure Equivalence (every 4th item) */
	for (i = 0; i < num_intervals; i += 4) {
		bonsai_invalidate(bt_unhinted, flat_table[i].start);
		mtree_store_range(&mt, flat_table[i].start, flat_table[i].end,
				  NULL, GFP_KERNEL);
	}

	for (i = 0; i < num_intervals; i++) {
		unsigned long m = flat_table[i].start + (step / 2);
		void *b_val = bonsai_lookup(bt_unhinted, m);
		void *m_val = mtree_load(&mt, m);

		if (i % 4 == 0) {
			if (b_val || m_val) {
				mismatches++;
				if (mismatches <= 5)
					pr_err("inval mismatch slot %u: b=%p m=%p (expected NULL)\n",
					       i, b_val, m_val);
			}
		} else {
			if (b_val != flat_table[i].val || m_val != flat_table[i].val) {
				mismatches++;
				if (mismatches <= 5)
					pr_err("survivor mismatch slot %u: b=%p m=%p exp=%p\n",
					       i, b_val, m_val, flat_table[i].val);
			}
		}
	}

	/* 3. Re-insertion & Mutation Stress */
	for (i = 0; i < num_intervals; i += 4) {
		void *new_val = (void *)(unsigned long)(i + 0x5000);

		bonsai_store_range(bt_unhinted, flat_table[i].start,
				   flat_table[i].end, new_val, GFP_KERNEL);
		mtree_store_range(&mt, flat_table[i].start, flat_table[i].end,
				  new_val, GFP_KERNEL);
	}

	for (i = 0; i < num_intervals; i += 4) {
		unsigned long m = flat_table[i].start + (step / 2);
		void *expected = (void *)(unsigned long)(i + 0x5000);
		void *b_val = bonsai_lookup(bt_unhinted, m);
		void *m_val = mtree_load(&mt, m);

		if (b_val != expected || m_val != expected) {
			mismatches++;
			if (mismatches <= 5)
				pr_err("re-insert mismatch slot %u: b=%p m=%p exp=%p\n",
				       i, b_val, m_val, expected);
		}
	}

	bonsai_destroy(bt);
	bonsai_destroy(bt_unhinted);
	kfree(bt);
	kfree(bt_unhinted);
	mtree_destroy(&mt);
	kfree(flat_table);

	pr_info("  Stress/Validation N = %5u intervals : %s (mismatches=%u)\n",
		num_intervals, mismatches ? "FAIL" : "PASS", mismatches);

	return mismatches ? -EINVAL : 0;
}

static const unsigned int scale_steps[] = {
	10, 50, 250, 1000, 4000, 8192, 16384
};

static int __init test_bonsai_init(void)
{
	unsigned int i;
	int err, failures = 0;

	pr_info("Starting Bonsai Tree test harness (do_stress=%d, do_bench=%d, max_scale=%u, num_lookups=%u)\n",
		do_stress, do_bench, max_scale, num_lookups);

	if (test_bonsai_invalidation())
		failures++;

	if (do_stress) {
		pr_info("--- Running Bonsai vs Maple Stress & Equivalence Validation ---\n");
		for (i = 0; i < ARRAY_SIZE(scale_steps); i++) {
			if (scale_steps[i] > max_scale)
				break;
			err = test_bonsai_maple_stress_equivalence(scale_steps[i]);
			if (err)
				failures++;
		}
	}

	if (do_bench) {
		pr_info("--- Running Micro-Benchmark & Latency Telemetry ---\n");
		for (i = 0; i < ARRAY_SIZE(scale_steps); i++) {
			if (scale_steps[i] > max_scale)
				break;
			benchmark_scale(scale_steps[i]);
		}
		pr_info("Benchmark complete.\n");
	}

	if (failures) {
		pr_err("Test harness finished with %d failures\n", failures);
		return -EINVAL;
	}

	pr_info("All tests passed successfully.\n");
	return 0;
}

static void __exit test_bonsai_exit(void)
{
}

module_init(test_bonsai_init);
module_exit(test_bonsai_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jim Cromie <jim.cromie@gmail.com>");
MODULE_DESCRIPTION("Bonsai Tree Performance Benchmark");
