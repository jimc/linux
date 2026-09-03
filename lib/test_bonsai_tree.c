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
#include <linux/bonsai_tree.h>
#include <linux/maple_tree.h>

#define NUM_LOOKUPS	1000000

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
	struct flat_interval *res = bsearch(&key, table, nr, sizeof(*table), cmp_flat_interval);
	return res ? res->val : NULL;
}

static void benchmark_scale(unsigned int num_intervals)
{
	struct flat_interval *flat_table;
	struct bonsai_tree bt;
	struct maple_tree mt;
	ktime_t t0, t1;
	u64 bonsai_build_ns, maple_build_ns;
	u64 bonsai_seq_ns, maple_seq_ns, bsearch_seq_ns;
	u64 bonsai_rnd_ns, maple_rnd_ns, bsearch_rnd_ns;
	unsigned long *rnd_keys;
	unsigned int i, step = 16;
	volatile void *sink = NULL;
	size_t flat_bytes, bonsai_bytes, maple_bytes;

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
		flat_table[i].val = (void *)(unsigned long)(i + 1);
	}
	for (i = 0; i < 1024; i++) {
		unsigned int idx = get_random_u32_below(num_intervals);
		rnd_keys[i] = flat_table[idx].start + get_random_u32_below(step);
	}

	flat_bytes = num_intervals * sizeof(struct flat_interval);

	/* 0. Benchmark Bonsai Tree Build */
	bonsai_init(&bt);
	bonsai_init_hint(&bt, num_intervals, GFP_KERNEL);
	t0 = ktime_get();
	for (i = 0; i < num_intervals; i++) {
		bonsai_store_range(&bt, flat_table[i].start, flat_table[i].end,
				   flat_table[i].val, GFP_KERNEL);
	}
	bonsai_seal(&bt);
	t1 = ktime_get();
	bonsai_build_ns = ktime_to_ns(ktime_sub(t1, t0));
	bonsai_bytes = bt.node_count * BONSAI_NODE_SIZE;

	/* 1. Benchmark Maple Tree Build */
	mt_init_flags(&mt, 0);
	t0 = ktime_get();
	for (i = 0; i < num_intervals; i++) {
		mtree_store_range(&mt, flat_table[i].start, flat_table[i].end,
				  flat_table[i].val, GFP_KERNEL);
	}
	t1 = ktime_get();
	maple_build_ns = ktime_to_ns(ktime_sub(t1, t0));
	maple_bytes = (num_intervals / 10 + 1) * 256;

	/* 2. Sequential Lookup Benchmark */
	t0 = ktime_get();
	for (i = 0; i < NUM_LOOKUPS; i++) {
		unsigned long key = (i % num_intervals) * step + 4;
		sink = bsearch_lookup(flat_table, num_intervals, key);
	}
	t1 = ktime_get();
	bsearch_seq_ns = ktime_to_ns(ktime_sub(t1, t0));

	t0 = ktime_get();
	for (i = 0; i < NUM_LOOKUPS; i++) {
		unsigned long key = (i % num_intervals) * step + 4;
		sink = bonsai_lookup(&bt, key);
	}
	t1 = ktime_get();
	bonsai_seq_ns = ktime_to_ns(ktime_sub(t1, t0));

	t0 = ktime_get();
	for (i = 0; i < NUM_LOOKUPS; i++) {
		unsigned long key = (i % num_intervals) * step + 4;
		sink = mtree_load(&mt, key);
	}
	t1 = ktime_get();
	maple_seq_ns = ktime_to_ns(ktime_sub(t1, t0));

	/* 3. Random Lookup Benchmark */
	t0 = ktime_get();
	for (i = 0; i < NUM_LOOKUPS; i++) {
		sink = bsearch_lookup(flat_table, num_intervals, rnd_keys[i & 1023]);
	}
	t1 = ktime_get();
	bsearch_rnd_ns = ktime_to_ns(ktime_sub(t1, t0));

	t0 = ktime_get();
	for (i = 0; i < NUM_LOOKUPS; i++) {
		sink = bonsai_lookup(&bt, rnd_keys[i & 1023]);
	}
	t1 = ktime_get();
	bonsai_rnd_ns = ktime_to_ns(ktime_sub(t1, t0));

	t0 = ktime_get();
	for (i = 0; i < NUM_LOOKUPS; i++) {
		sink = mtree_load(&mt, rnd_keys[i & 1023]);
	}
	t1 = ktime_get();
	maple_rnd_ns = ktime_to_ns(ktime_sub(t1, t0));

	pr_info("=== Benchmark N = %4u intervals (1M lookups) ===\n", num_intervals);
	pr_info("  Memory   : Flat=%zu B | Bonsai=%zu B (nodes=%u, h=%u) | Maple=~%zu B\n",
		flat_bytes, bonsai_bytes, bt.node_count, bt.height, maple_bytes);
	pr_info("  Build    : Bonsai=%llu us | Maple=%llu us\n",
		bonsai_build_ns / 1000, maple_build_ns / 1000);
	pr_info("  Seq Look : BSearch=%llu ns/op | Bonsai=%llu ns/op | Maple=%llu ns/op\n",
		bsearch_seq_ns / NUM_LOOKUPS, bonsai_seq_ns / NUM_LOOKUPS, maple_seq_ns / NUM_LOOKUPS);
	pr_info("  Rnd Look : BSearch=%llu ns/op | Bonsai=%llu ns/op | Maple=%llu ns/op\n",
		bsearch_rnd_ns / NUM_LOOKUPS, bonsai_rnd_ns / NUM_LOOKUPS, maple_rnd_ns / NUM_LOOKUPS);

	bonsai_destroy(&bt);
	mtree_destroy(&mt);
	kfree(flat_table);
	kfree(rnd_keys);
	(void)sink;
}

static int __init test_bonsai_init(void)
{
	pr_info("Starting Bonsai Tree vs Maple Tree vs Binary Search Benchmark\n");
	benchmark_scale(10);   /* Micro-module / BPF size */
	benchmark_scale(50);   /* Small kernel module */
	benchmark_scale(250);  /* Medium driver */
	benchmark_scale(1000); /* Large subsystem */
	benchmark_scale(4000); /* Whole-kernel builtin dyndbg size */
	pr_info("Benchmark complete.\n");
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
