// SPDX-License-Identifier: GPL-2.0+
/*
 * Direct-Map Large Folio Scratchpad & Pool bump allocators.
 */
#include <linux/export.h>
#include <linux/string.h>
#include <linux/moduleparam.h>
#include <linux/folio_pool.h>

DEFINE_STATIC_KEY_TRUE(folio_pool_enabled_key);
EXPORT_SYMBOL_GPL(folio_pool_enabled_key);

static int folio_pool_enabled_set(const char *val, const struct kernel_param *kp)
{
	bool enable;
	int ret = kstrtobool(val, &enable);

	if (ret)
		return ret;

	if (enable)
		static_branch_enable(&folio_pool_enabled_key);
	else
		static_branch_disable(&folio_pool_enabled_key);

	return 0;
}

static int folio_pool_enabled_get(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "%c\n", static_branch_likely(&folio_pool_enabled_key) ? 'Y' : 'N');
}

static const struct kernel_param_ops folio_pool_enabled_ops = {
	.set = folio_pool_enabled_set,
	.get = folio_pool_enabled_get,
};

module_param_cb(enabled, &folio_pool_enabled_ops, NULL, 0644);
MODULE_PARM_DESC(enabled, "Toggle folio_pool/scratchpad bump allocator (0 = fallback to SLUB)");

/*
 * 1. Variable-Sized Scratchpad (Core Engine)
 */
void _folio_scratchpad_init_key(struct folio_scratchpad *sp, unsigned int order,
				struct static_key *key)
{
	INIT_LIST_HEAD(&sp->chunks);
	sp->free_ptr = NULL;
	sp->remaining = 0;
	sp->chunk_order = order;
	sp->key = key;
	spin_lock_init(&sp->lock);
}
EXPORT_SYMBOL_GPL(_folio_scratchpad_init_key);

void folio_scratchpad_init(struct folio_scratchpad *sp, unsigned int order)
{
	_folio_scratchpad_init_key(sp, order, NULL);
}
EXPORT_SYMBOL_GPL(folio_scratchpad_init);

static inline bool folio_scratchpad_is_enabled(const struct folio_scratchpad *sp)
{
	if (sp->key)
		return static_key_enabled(sp->key);
	return static_branch_likely(&folio_pool_enabled_key);
}

noinline void *folio_scratchpad_alloc(struct folio_scratchpad *sp, size_t size,
				     size_t align, gfp_t gfp)
{
	struct folio_pool_chunk *chunk;
	struct folio *folio;
	void *elem, *base;
	size_t chunk_size, aligned_size, pad, header_offset;
	unsigned long flags;

	if (!folio_scratchpad_is_enabled(sp))
		return kvzalloc(size, gfp);

	if (unlikely(!size))
		return NULL;

	align = max_t(size_t, sizeof(void *), align ? align : sizeof(void *));

	spin_lock_irqsave(&sp->lock, flags);
	pad = (uintptr_t)sp->free_ptr & (align - 1);
	if (pad)
		pad = align - pad;
	aligned_size = size + pad;

	if (sp->remaining < aligned_size) {
		size_t needed = ALIGN(sizeof(struct folio_pool_chunk), align) + size;
		unsigned int needed_order = get_order(needed);
		unsigned int order = max(sp->chunk_order, needed_order);

		spin_unlock_irqrestore(&sp->lock, flags);

		folio = folio_alloc(gfp, order);
		if (!folio && order > needed_order)
			folio = folio_alloc(gfp, needed_order);
		if (!folio)
			return NULL;

		base = folio_address(folio);
		chunk = (struct folio_pool_chunk *)base;
		chunk->folio = folio;
		chunk_size = folio_size(folio);
		header_offset = ALIGN(sizeof(*chunk), max_t(size_t, sizeof(void *), align));

		spin_lock_irqsave(&sp->lock, flags);
		list_add(&chunk->link, &sp->chunks);
		sp->free_ptr = base + header_offset;
		sp->remaining = chunk_size - header_offset;

		pad = (uintptr_t)sp->free_ptr & (align - 1);
		if (pad)
			pad = align - pad;
		aligned_size = size + pad;
	}

	if (sp->remaining < aligned_size) {
		spin_unlock_irqrestore(&sp->lock, flags);
		return NULL;
	}

	elem = sp->free_ptr + pad;
	sp->free_ptr += aligned_size;
	sp->remaining -= aligned_size;
	spin_unlock_irqrestore(&sp->lock, flags);

	memset(elem, 0, size);
	return elem;
}
EXPORT_SYMBOL_GPL(folio_scratchpad_alloc);

noinline void folio_scratchpad_reset(struct folio_scratchpad *sp)
{
	struct folio_pool_chunk *head, *chunk, *tmp;
	size_t header_offset;
	unsigned long flags;

	spin_lock_irqsave(&sp->lock, flags);
	if (list_empty(&sp->chunks)) {
		sp->free_ptr = NULL;
		sp->remaining = 0;
		spin_unlock_irqrestore(&sp->lock, flags);
		return;
	}

	/* Retain primary head chunk; free overflow chunks */
	head = list_first_entry(&sp->chunks, struct folio_pool_chunk, link);
	list_for_each_entry_safe(chunk, tmp, &sp->chunks, link) {
		if (chunk == head)
			continue;
		list_del(&chunk->link);
		folio_put(chunk->folio);
	}

	header_offset = ALIGN(sizeof(*head), sizeof(void *));
	sp->free_ptr = folio_address(head->folio) + header_offset;
	sp->remaining = folio_size(head->folio) - header_offset;
	spin_unlock_irqrestore(&sp->lock, flags);
}
EXPORT_SYMBOL_GPL(folio_scratchpad_reset);

noinline void folio_scratchpad_free(struct folio_scratchpad *sp)
{
	struct folio_pool_chunk *chunk, *tmp;
	struct folio *folio;
	unsigned long flags;

	spin_lock_irqsave(&sp->lock, flags);
	sp->free_ptr = NULL;
	sp->remaining = 0;
	list_for_each_entry_safe(chunk, tmp, &sp->chunks, link) {
		folio = chunk->folio;
		list_del(&chunk->link);
		folio_put(folio);
	}
	spin_unlock_irqrestore(&sp->lock, flags);
}
EXPORT_SYMBOL_GPL(folio_scratchpad_free);

/**
 * folio_scratchpad_discard - Discard allocation if at top of active chunk
 * @sp: Scratchpad instance
 * @ptr: Address of allocation to release
 * @size: Size passed to prior allocation
 *
 * If @ptr is the most recent allocation in the active chunk, rewinds
 * free_ptr in O(1) time without fragmentation. Repeated calls in strict
 * reverse LIFO order can unwind N items within the active chunk, provided
 * element sizes are alignment-matched so inter-element padding is zero.
 */
void folio_scratchpad_discard(struct folio_scratchpad *sp, void *ptr, size_t size)
{
	unsigned long flags;

	if (!ptr || !size)
		return;

	if (unlikely(!folio_scratchpad_is_enabled(sp))) {
		kvfree(ptr);
		return;
	}

	spin_lock_irqsave(&sp->lock, flags);
	if (sp->free_ptr == (char *)ptr + size) {
		sp->free_ptr = ptr;
		sp->remaining += size;
	}
	spin_unlock_irqrestore(&sp->lock, flags);
}
EXPORT_SYMBOL_GPL(folio_scratchpad_discard);

void folio_scratchpad_trim(struct folio_scratchpad *sp, size_t unused_bytes)
{
	unsigned long flags;

	if (!unused_bytes)
		return;

	if (unlikely(!folio_scratchpad_is_enabled(sp)))
		return;

	spin_lock_irqsave(&sp->lock, flags);
	if (sp->free_ptr) {
		sp->free_ptr = (char *)sp->free_ptr - unused_bytes;
		sp->remaining += unused_bytes;
	}
	spin_unlock_irqrestore(&sp->lock, flags);
}
EXPORT_SYMBOL_GPL(folio_scratchpad_trim);

/*
 * 2. Fixed-Slot Uniform Pool (Specialized Thin Wrapper on Scratchpad)
 */
void folio_pool_init_align(struct folio_pool *fp, size_t elem_size,
			   size_t elem_align, unsigned int order)
{
	folio_scratchpad_init(&fp->base, order);
	fp->elem_size = elem_size;
	fp->elem_align = max_t(size_t, sizeof(void *), elem_align ? elem_align : sizeof(void *));
}
EXPORT_SYMBOL_GPL(folio_pool_init_align);

void _folio_pool_init_key(struct folio_pool *fp, size_t elem_size, unsigned int order,
			  struct static_key *key)
{
	size_t align = sizeof(void *);

	if (elem_size && is_power_of_2(elem_size))
		align = max_t(size_t, sizeof(void *), elem_size);
	_folio_scratchpad_init_key(&fp->base, order, key);
	fp->elem_size = elem_size;
	fp->elem_align = align;
}
EXPORT_SYMBOL_GPL(_folio_pool_init_key);

void folio_pool_init(struct folio_pool *fp, size_t elem_size, unsigned int order)
{
	_folio_pool_init_key(fp, elem_size, order, NULL);
}
EXPORT_SYMBOL_GPL(folio_pool_init);

void folio_scratchpad_stats(struct folio_scratchpad *sp, unsigned int *nr_chunks,
			    size_t *chunk_size, size_t *tail_used)
{
	unsigned long flags;
	size_t csz;

	csz = (PAGE_SIZE << sp->chunk_order);
	if (chunk_size)
		*chunk_size = csz;

	spin_lock_irqsave(&sp->lock, flags);
	if (nr_chunks)
		*nr_chunks = list_count_nodes(&sp->chunks);
	if (tail_used)
		*tail_used = csz > sp->remaining ? csz - sp->remaining : 0;
	spin_unlock_irqrestore(&sp->lock, flags);
}
EXPORT_SYMBOL_GPL(folio_scratchpad_stats);
