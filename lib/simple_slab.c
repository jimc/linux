// SPDX-License-Identifier: GPL-2.0
/*
 * Simple Slab, Ledger & 2D-Radix Memory Subsystem
 *
 * Core implementation of compound buddy-page allocator (alloc_pages_node)
 * for permanent kernel databases (e.g. Lockdep).
 *
 * Copyright (C) 2026 Jim Cromie <jim.cromie@gmail.com>
 */
#include <linux/simple_slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>


static struct simple_slab *__scratchpad_alloc_chunk(struct scratchpad *sp, gfp_t gfp)
{
	struct simple_slab *slab;
	unsigned int order;
	struct page *page;
	size_t chunk_size;

	if (!scratchpad_is_enabled(sp))
		return NULL;

	order = sp->growth.chunk_order;

	page = alloc_pages_node(NUMA_NO_NODE, gfp | __GFP_COMP, order);
	if (!page && order > sp->growth.init_order) {
		order = sp->growth.init_order;
		page = alloc_pages_node(NUMA_NO_NODE, gfp | __GFP_COMP, order);
	}

	if (!page)
		return NULL;

	if (sp->growth.flags & SCRATCH_F_RAW_CHUNKS)
		return page_address(page);

	slab = page_address(page);
	INIT_LIST_HEAD(&slab->link);
	slab->page = page;
	slab->order = order;

	chunk_size = (size_t)PAGE_SIZE << order;
	list_add_tail(&slab->link, &sp->chunks);

	sp->free_ptr = (void *)((unsigned long)slab + sizeof(*slab));
	sp->remaining = chunk_size - sizeof(*slab);

	if (sp->growth.dwell_cur > 0) {
		sp->growth.dwell_cur--;
	} else if (sp->growth.chunk_order < sp->growth.max_order) {
		sp->growth.chunk_order++;
		sp->growth.dwell_cur = SCRATCH_INIT_DWELL;
	}

	return slab;
}

void *__scratchpad_alloc(struct scratchpad *sp, size_t size,
			 size_t align, gfp_t gfp)
{
	size_t header_offset, needed_bytes, chunk_size;
	struct simple_slab *slab;
	void *free_p, *aligned;
	size_t pad;

	if (!align)
		align = __alignof__(unsigned long);

	free_p = sp->free_ptr;
	if (free_p) {
		aligned = PTR_ALIGN(free_p, align);
		pad = (size_t)(aligned - free_p);

		if (sp->remaining >= size + pad) {
			sp->free_ptr = (char *)aligned + size;
			sp->remaining -= (size + pad);
			return aligned;
		}
	}

	header_offset = (sp->growth.flags & SCRATCH_F_RAW_CHUNKS) ? 0 : ALIGN(sizeof(struct simple_slab), align);
	needed_bytes = header_offset + size;
	chunk_size = (size_t)PAGE_SIZE << sp->growth.chunk_order;

	while (needed_bytes > chunk_size && sp->growth.chunk_order < sp->growth.max_order) {
		sp->growth.chunk_order++;
		sp->growth.dwell_cur = SCRATCH_INIT_DWELL;
		chunk_size = (size_t)PAGE_SIZE << sp->growth.chunk_order;
	}

	if (needed_bytes > chunk_size)
		return NULL;

	slab = __scratchpad_alloc_chunk(sp, gfp);
	if (!slab)
		return NULL;

	if (sp->growth.flags & SCRATCH_F_RAW_CHUNKS) {
		sp->free_ptr = NULL;
		sp->remaining = 0;
		return slab;
	}

	aligned = (void *)((unsigned long)slab + header_offset);
	sp->free_ptr = (char *)aligned + size;
	sp->remaining = ((size_t)PAGE_SIZE << slab->order) - header_offset - size;

	return aligned;
}
EXPORT_SYMBOL_GPL(__scratchpad_alloc);

void *scratchpad_alloc_align(struct scratchpad *sp, size_t size,
			     size_t align, gfp_t gfp)
{
	return __scratchpad_alloc(sp, size, align, gfp);
}
EXPORT_SYMBOL_GPL(scratchpad_alloc_align);

void __scratchpad_discard(struct scratchpad *sp, void *ptr, size_t size)
{
	if (!ptr)
		return;

	if ((char *)ptr + size == sp->free_ptr) {
		sp->free_ptr = ptr;
		sp->remaining += size;
	}
}
EXPORT_SYMBOL_GPL(__scratchpad_discard);

void scratchpad_discard(struct scratchpad *sp, void *ptr, size_t size)
{
	__scratchpad_discard(sp, ptr, size);
}
EXPORT_SYMBOL_GPL(scratchpad_discard);

void __scratchpad_trim(struct scratchpad *sp, size_t unused_bytes)
{
	if (!unused_bytes || !sp->free_ptr)
		return;

	sp->free_ptr = (char *)sp->free_ptr - unused_bytes;
	sp->remaining += unused_bytes;
}
EXPORT_SYMBOL_GPL(__scratchpad_trim);

void scratchpad_trim(struct scratchpad *sp, size_t unused_bytes)
{
	__scratchpad_trim(sp, unused_bytes);
}
EXPORT_SYMBOL_GPL(scratchpad_trim);

void __scratchpad_stats(struct scratchpad *sp, unsigned int *nr_chunks,
			size_t *chunk_size, size_t *tail_used)
{
	struct simple_slab *slab;
	unsigned int count = 0;
	size_t total_sz = 0;

	list_for_each_entry(slab, &sp->chunks, link) {
		count++;
		total_sz += (size_t)PAGE_SIZE << slab->order;
	}

	if (sp->static_buf && count == 0) {
		*nr_chunks = 1;
		*chunk_size = sp->static_size;
		*tail_used = sp->static_size - sp->remaining;
		return;
	}

	*nr_chunks = count;
	*chunk_size = total_sz;

	if (!list_empty(&sp->chunks)) {
		slab = list_last_entry(&sp->chunks, struct simple_slab, link);
		*tail_used = ((size_t)PAGE_SIZE << slab->order) - sizeof(*slab) - sp->remaining;
	} else {
		*tail_used = 0;
	}
}
EXPORT_SYMBOL_GPL(__scratchpad_stats);

void scratchpad_stats(struct scratchpad *sp, unsigned int *nr_chunks,
		      size_t *chunk_size, size_t *tail_used)
{
	__scratchpad_stats(sp, nr_chunks, chunk_size, tail_used);
}
EXPORT_SYMBOL_GPL(scratchpad_stats);
