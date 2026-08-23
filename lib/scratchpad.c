// SPDX-License-Identifier: GPL-2.0
/*
 * Scratchpad & Scratchrec Ephemeral Memory Subsystem
 *
 * Implementation of transactional lifecycles (reset, free, trim, discard),
 * dynamic dwell geometry, and keyed slab fallbacks.
 *
 * Copyright (C) 2026 Jim Cromie <jim.cromie@gmail.com>
 */
#include <linux/scratchpad.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

int __scratchpad_prime(struct scratchpad *sp, gfp_t gfp)
{
	struct simple_slab *slab;

	if (sp->free_ptr != NULL || !list_empty(&sp->chunks))
		return 0;

	slab = __scratchpad_alloc(sp, sizeof(void *), sizeof(void *), gfp);
	if (!slab)
		return -ENOMEM;

	sp->free_ptr = (char *)sp->free_ptr - sizeof(void *);
	sp->remaining += sizeof(void *);

	return 0;
}
EXPORT_SYMBOL_GPL(__scratchpad_prime);

int scratchpad_prime(struct scratchpad *sp, gfp_t gfp)
{
	return __scratchpad_prime(sp, gfp);
}
EXPORT_SYMBOL_GPL(scratchpad_prime);

int _scratchpad_init(struct scratchpad *sp, unsigned int min_order,
		     struct static_key *key, gfp_t gfp)
{
	INIT_LIST_HEAD(&sp->chunks);
	sp->free_ptr = NULL;
	sp->remaining = 0;
	sp->static_buf = NULL;
	sp->static_size = 0;
	sp->growth.init_order = min_t(unsigned int, min_order, SCRATCH_DEFAULT_MAX_ORDER);
	sp->growth.chunk_order = sp->growth.init_order;
	sp->growth.max_order = SCRATCH_DEFAULT_MAX_ORDER;
	sp->growth.dwell_cur = (sp->growth.max_order > sp->growth.init_order) ? SCRATCH_INIT_DWELL : 0;
	sp->growth.flags = 0;
	sp->key = key;

	if (gfp)
		return __scratchpad_prime(sp, gfp);

	return 0;
}
EXPORT_SYMBOL_GPL(_scratchpad_init);

int _scratchrec_init(struct scratchrec *sr, size_t elem_size,
		      unsigned int min_order, struct static_key *key, gfp_t gfp)
{
	int ret = _scratchpad_init(&sr->base, min_order, key, gfp);

	sr->freelist = NULL;
	sr->elem_size = elem_size;
	sr->elem_align = sizeof(void *);
	return ret;
}
EXPORT_SYMBOL_GPL(_scratchrec_init);

int _scratchrec_init_align(struct scratchrec *sr, size_t elem_size,
			   size_t elem_align, unsigned int min_order,
			   struct static_key *key, gfp_t gfp)
{
	int ret = _scratchpad_init(&sr->base, min_order, key, gfp);

	sr->freelist = NULL;
	sr->elem_size = elem_size;
	sr->elem_align = elem_align;
	return ret;
}
EXPORT_SYMBOL_GPL(_scratchrec_init_align);

int __scratchrec_prime(struct scratchrec *sr, gfp_t gfp)
{
	return __scratchpad_prime(&sr->base, gfp);
}
EXPORT_SYMBOL_GPL(__scratchrec_prime);

int scratchrec_prime(struct scratchrec *sr, gfp_t gfp)
{
	return scratchpad_prime(&sr->base, gfp);
}
EXPORT_SYMBOL_GPL(scratchrec_prime);

int __scratchpad_reserve(struct scratchpad *sp, size_t min_bytes, gfp_t gfp)
{
	if (sp->remaining >= min_bytes)
		return 0;

	while (sp->growth.chunk_order < sp->growth.max_order &&
	       ((size_t)PAGE_SIZE << sp->growth.chunk_order) < min_bytes + sizeof(struct simple_slab)) {
		sp->growth.chunk_order++;
	}

	if (!__scratchpad_alloc(sp, min_bytes, __alignof__(unsigned long), gfp))
		return -ENOMEM;

	sp->free_ptr = (char *)sp->free_ptr - min_bytes;
	sp->remaining += min_bytes;

	return 0;
}
EXPORT_SYMBOL_GPL(__scratchpad_reserve);

int scratchpad_reserve(struct scratchpad *sp, size_t min_bytes, gfp_t gfp)
{
	return __scratchpad_reserve(sp, min_bytes, gfp);
}
EXPORT_SYMBOL_GPL(scratchpad_reserve);

static void __scratchpad_free_chunks(struct scratchpad *sp, bool keep_first)
{
	struct simple_slab *slab, *tmp;
	bool is_first = true;

	list_for_each_entry_safe(slab, tmp, &sp->chunks, link) {
		if (is_first && keep_first) {
			is_first = false;
			continue;
		}
		list_del(&slab->link);
		__free_pages(slab->page, slab->order);
	}
}

void __scratchpad_reset(struct scratchpad *sp)
{
	struct simple_slab *first;

	if (list_empty(&sp->chunks)) {
		sp->free_ptr = sp->static_buf;
		sp->remaining = sp->static_size;
	} else {
		__scratchpad_free_chunks(sp, true);
		first = list_first_entry(&sp->chunks, struct simple_slab, link);
		sp->free_ptr = (void *)((unsigned long)first + sizeof(*first));
		sp->remaining = ((size_t)PAGE_SIZE << first->order) - sizeof(*first);
	}

	sp->growth.chunk_order = sp->growth.init_order;
	sp->growth.dwell_cur = (sp->growth.max_order > sp->growth.init_order) ? SCRATCH_INIT_DWELL : 0;
}

void scratchpad_reset(struct scratchpad *sp)
{
	__scratchpad_reset(sp);
}
EXPORT_SYMBOL_GPL(scratchpad_reset);

void __scratchpad_free(struct scratchpad *sp)
{
	__scratchpad_free_chunks(sp, false);

	sp->free_ptr = sp->static_buf;
	sp->remaining = sp->static_size;
	sp->growth.chunk_order = sp->growth.init_order;
	sp->growth.dwell_cur = (sp->growth.max_order > sp->growth.init_order) ? SCRATCH_INIT_DWELL : 0;
}

void scratchpad_free(struct scratchpad *sp)
{
	__scratchpad_free(sp);
}
EXPORT_SYMBOL_GPL(scratchpad_free);
