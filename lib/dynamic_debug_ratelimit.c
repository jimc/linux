// SPDX-License-Identifier: GPL-2.0
/*
 * Dynamic Debug Rate-limiting Support
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ratelimit.h>
#include <linux/maple_tree.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/refcount.h>
#include "dynamic_debug_ratelimit.h"

static int dyndbg_ratelimit_interval = 5; /* seconds */
static int dyndbg_ratelimit_burst = 10;
module_param_named(ratelimit_interval, dyndbg_ratelimit_interval, int, 0644);
module_param_named(ratelimit_burst, dyndbg_ratelimit_burst, int, 0644);

static DEFINE_MTREE(dd_ratelimits_solo);
static DEFINE_MTREE(dd_ratelimits_shared);

struct ddebug_ratelimit_solo {
	struct ratelimit_state rs;
	struct rcu_head rcu;
};

struct ddebug_ratelimit_shared {
	struct ratelimit_state rs;
	refcount_t refcount;
	struct rcu_head rcu;
};

static inline void ddebug_ratelimit_shared_get(struct ddebug_ratelimit_shared *s)
{
	if (s)
		refcount_inc(&s->refcount);
}

static inline void ddebug_ratelimit_shared_put(struct ddebug_ratelimit_shared *s)
{
	if (s && refcount_dec_and_test(&s->refcount))
		kfree_rcu(s, rcu);
}

static inline bool ddebug_apply_ratelimit(struct _ddebug *desc, struct ratelimit_state *rs)
{
	if (!__ratelimit(rs))
		return false;

	int m = ratelimit_state_reset_miss(rs);
	if (m) {
		printk(KERN_DEBUG "ddebug: %d callbacks suppressed for format: %s\n", m, desc->format);
	}
	return true;
}

bool ddebug_apply_ratelimit_solo(struct _ddebug *desc)
{
	struct ddebug_ratelimit_solo *wrapper;
	bool ret = true;

	wrapper = mtree_load(&dd_ratelimits_solo, (unsigned long)desc);
	if (wrapper)
		ret = ddebug_apply_ratelimit(desc, &wrapper->rs);

	return ret;
}

bool ddebug_apply_ratelimit_shared(struct _ddebug *desc)
{
	struct ddebug_ratelimit_shared *wrapper;
	bool ret = true;

	wrapper = mtree_load(&dd_ratelimits_shared, (unsigned long)desc);
	if (wrapper)
		ret = ddebug_apply_ratelimit(desc, &wrapper->rs);

	return ret;
}

/* Store the shared wrapper during query building */
static struct ddebug_ratelimit_shared *query_shared_wrapper;

void ddebug_ratelimit_update(struct _ddebug *dp, unsigned int oldflags, unsigned int newflags)
{
	bool old_solo = !!(oldflags & _DPRINTK_FLAGS_RATELIMIT_SOLO);
	bool old_shared = !!(oldflags & _DPRINTK_FLAGS_RATELIMIT_SHARED);
	bool new_solo = !!(newflags & _DPRINTK_FLAGS_RATELIMIT_SOLO);
	bool new_shared = !!(newflags & _DPRINTK_FLAGS_RATELIMIT_SHARED);

	/* 1. Tear down old state if transitioning off or changing type */
	if (old_solo && !new_solo) {
		struct ddebug_ratelimit_solo *old_wrapper = mtree_erase(&dd_ratelimits_solo, (unsigned long)dp);
		if (old_wrapper)
			kfree_rcu(old_wrapper, rcu);
	}
	if (old_shared && !new_shared) {
		struct ddebug_ratelimit_shared *old_wrapper = mtree_erase(&dd_ratelimits_shared, (unsigned long)dp);
		if (old_wrapper)
			ddebug_ratelimit_shared_put(old_wrapper);
	}

	/* 2. Setup new state if transitioning on or changing type */
	if (new_solo && !old_solo) {
		struct ddebug_ratelimit_solo *new_wrapper = kzalloc(sizeof(*new_wrapper), GFP_KERNEL);
		if (new_wrapper) {
			ratelimit_state_init(&new_wrapper->rs, dyndbg_ratelimit_interval * HZ, dyndbg_ratelimit_burst);
			int rc = mtree_store(&dd_ratelimits_solo, (unsigned long)dp, new_wrapper, GFP_KERNEL);
			if (rc) {
				kfree(new_wrapper);
			}
		}
	}

	if (new_shared && !old_shared) {
		if (!query_shared_wrapper) {
			query_shared_wrapper = kzalloc(sizeof(*query_shared_wrapper), GFP_KERNEL);
			if (query_shared_wrapper) {
				ratelimit_state_init(&query_shared_wrapper->rs, dyndbg_ratelimit_interval * HZ, dyndbg_ratelimit_burst);
				refcount_set(&query_shared_wrapper->refcount, 1); /* Base query reference */
			}
		}
		if (query_shared_wrapper) {
			ddebug_ratelimit_shared_get(query_shared_wrapper);
			int rc = mtree_store(&dd_ratelimits_shared, (unsigned long)dp, query_shared_wrapper, GFP_KERNEL);
			if (rc) {
				ddebug_ratelimit_shared_put(query_shared_wrapper);
			}
		}
	}
}

void ddebug_ratelimit_clear_query(void)
{
	if (query_shared_wrapper) {
		ddebug_ratelimit_shared_put(query_shared_wrapper); /* Release base query reference */
		query_shared_wrapper = NULL;
	}
}

void ddebug_ratelimit_free_info(const struct _ddebug_info *di)
{
	int i;
	struct _ddebug *dp;

	for (i = 0; i < di->descs.len; i++) {
		dp = &di->descs.start[i];
		if (dp->flags & _DPRINTK_FLAGS_RATELIMIT_SOLO) {
			struct ddebug_ratelimit_solo *wrapper = mtree_erase(&dd_ratelimits_solo, (unsigned long)dp);
			if (wrapper)
				kfree(wrapper); /* Unloading module context is safe for direct kfree */
		}
		if (dp->flags & _DPRINTK_FLAGS_RATELIMIT_SHARED) {
			struct ddebug_ratelimit_shared *wrapper = mtree_erase(&dd_ratelimits_shared, (unsigned long)dp);
			if (wrapper)
				ddebug_ratelimit_shared_put(wrapper);
		}
	}
}
