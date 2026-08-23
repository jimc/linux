// SPDX-License-Identifier: GPL-2.0
/*
 * Scoped Lexical Scratchpad Memory Subsystem
 *
 * Core implementation of compound buddy-page allocator (alloc_pages_node)
 * for scoped bursts, intra-scope reuse, and instant bulk reset.
 *
 * Copyright (C) 2026 Jim Cromie <jim.cromie@gmail.com>
 */
#include <linux/scratchpad.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

static struct scratchpad_chunk *__scratchpad_alloc_chunk(struct scratchpad *sp,
							 unsigned int order,
							 gfp_t gfp)
{
	struct scratchpad_chunk *chunk;
	struct page *page;
	size_t chunk_size;

	if (!scratchpad_is_enabled(sp))
		return NULL;

	page = alloc_pages_node(NUMA_NO_NODE, gfp | __GFP_COMP, order);
	if (!page && order > 0) {
		order = 0;
		page = alloc_pages_node(NUMA_NO_NODE, gfp | __GFP_COMP, order);
	}

	if (!page)
		return NULL;

	chunk = page_address(page);
	INIT_LIST_HEAD(&chunk->link);
	chunk->page = page;
	chunk->order = order;

	chunk_size = (size_t)PAGE_SIZE << order;
	list_add_tail(&chunk->link, &sp->chunks);

	sp->free_ptr = (void *)((unsigned long)chunk + sizeof(*chunk));
	sp->remaining = chunk_size - sizeof(*chunk);

	return chunk;
}

void *__scratchpad_alloc(struct scratchpad *sp, size_t size,
			 size_t align, gfp_t gfp)
{
	size_t header_offset, needed_bytes;
	struct scratchpad_chunk *chunk;
	void *free_p, *aligned;
	unsigned int order;
	size_t pad;

	if (!gfp)
		gfp = sp->gfp ?: GFP_KERNEL;

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

	header_offset = ALIGN(sizeof(struct scratchpad_chunk), align);
	needed_bytes = header_offset + size;
	order = max_t(unsigned int, sp->order ?: SCRATCHPAD_DEFAULT_ORDER,
		      get_order(needed_bytes));

	chunk = __scratchpad_alloc_chunk(sp, order, gfp);
	if (!chunk)
		return NULL;

	aligned = PTR_ALIGN((char *)chunk + header_offset, align);
	sp->free_ptr = (char *)aligned + size;
	sp->remaining = ((size_t)PAGE_SIZE << chunk->order) -
			((char *)sp->free_ptr - (char *)chunk);

	return aligned;
}
EXPORT_SYMBOL_GPL(__scratchpad_alloc);

void *scratchpad_alloc_align(struct scratchpad *sp, size_t size,
			     size_t align, gfp_t gfp)
{
	return __scratchpad_alloc(sp, size, align, gfp);
}
EXPORT_SYMBOL_GPL(scratchpad_alloc_align);

void scratchpad_rewind(struct scratchpad *sp, struct scratchpad_mark mark)
{
	struct scratchpad_chunk *chunk, *tmp;

	/* If allocations crossed into new chunks, free trailing chunks */
	if (mark.chunk && mark.chunk != sp->chunks.prev) {
		list_for_each_entry_safe_reverse(chunk, tmp, &sp->chunks, link) {
			if (&chunk->link == mark.chunk)
				break;
			list_del(&chunk->link);
			__free_pages(chunk->page, chunk->order);
		}
	}

	sp->free_ptr = mark.free_ptr;
	sp->remaining = mark.remaining;
}
EXPORT_SYMBOL_GPL(scratchpad_rewind);

int __scratchpad_prime(struct scratchpad *sp, gfp_t gfp)
{
	void *ptr;

	if (sp->free_ptr != NULL || !list_empty(&sp->chunks))
		return 0;

	ptr = __scratchpad_alloc(sp, sizeof(void *), sizeof(void *), gfp);
	if (!ptr)
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

int _scratchpad_init(struct scratchpad *sp, size_t elem_size, size_t elem_align,
		     unsigned int order, struct static_key *key, gfp_t gfp)
{
	INIT_LIST_HEAD(&sp->chunks);
	sp->free_ptr = NULL;
	sp->remaining = 0;
	sp->static_buf = NULL;
	sp->static_size = 0;
	sp->elem_size = elem_size ?: sizeof(void *);
	sp->align_quantum = elem_align ?: __alignof__(void *);
	sp->order = order ?: SCRATCHPAD_DEFAULT_ORDER;
	sp->gfp = gfp ?: GFP_KERNEL;
	sp->freelist = NULL;
	sp->key = key;

	if (gfp)
		return __scratchpad_prime(sp, gfp);

	return 0;
}
EXPORT_SYMBOL_GPL(_scratchpad_init);

int __scratchpad_reserve(struct scratchpad *sp, size_t min_bytes, gfp_t gfp)
{
	if (sp->remaining >= min_bytes)
		return 0;

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
	struct scratchpad_chunk *chunk, *tmp;
	bool is_first = true;

	list_for_each_entry_safe(chunk, tmp, &sp->chunks, link) {
		if (is_first && keep_first) {
			is_first = false;
			continue;
		}
		list_del(&chunk->link);
		__free_pages(chunk->page, chunk->order);
	}
}

void scratchpad_reset(struct scratchpad *sp)
{
	struct scratchpad_chunk *first;

	sp->freelist = NULL;

	if (list_empty(&sp->chunks)) {
		sp->free_ptr = sp->static_buf;
		sp->remaining = sp->static_size;
	} else {
		__scratchpad_free_chunks(sp, true);
		first = list_first_entry(&sp->chunks, struct scratchpad_chunk, link);
		sp->free_ptr = (void *)((unsigned long)first + sizeof(*first));
		sp->remaining = ((size_t)PAGE_SIZE << first->order) - sizeof(*first);
	}
}
EXPORT_SYMBOL_GPL(scratchpad_reset);

void scratchpad_free(struct scratchpad *sp)
{
	__scratchpad_free_chunks(sp, false);

	sp->freelist = NULL;
	sp->free_ptr = sp->static_buf;
	sp->remaining = sp->static_size;
}
EXPORT_SYMBOL_GPL(scratchpad_free);
