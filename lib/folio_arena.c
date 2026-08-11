// SPDX-License-Identifier: GPL-2.0+
/*
 * Direct-Map Large Folio Arena bump allocator.
 */
#include <linux/export.h>
#include <linux/string.h>
#include <linux/folio_arena.h>

void folio_arena_init(struct folio_arena *fa, size_t elem_size, unsigned int order)
{
	INIT_LIST_HEAD(&fa->chunks);
	fa->free_ptr = NULL;
	fa->remaining = 0;
	fa->elem_size = elem_size;
	fa->chunk_order = order;
	spin_lock_init(&fa->lock);
}
EXPORT_SYMBOL_GPL(folio_arena_init);

void *folio_arena_alloc(struct folio_arena *fa, gfp_t gfp)
{
	struct folio_arena_chunk *chunk;
	struct folio *folio;
	void *elem, *base;
	size_t chunk_size;
	unsigned long flags;

	spin_lock_irqsave(&fa->lock, flags);
	if (fa->remaining < fa->elem_size) {
		spin_unlock_irqrestore(&fa->lock, flags);

		folio = folio_alloc(gfp, fa->chunk_order);
		if (!folio)
			return NULL;

		base = folio_address(folio);
		chunk = (struct folio_arena_chunk *)base;
		chunk->folio = folio;
		chunk_size = folio_size(folio);

		spin_lock_irqsave(&fa->lock, flags);
		list_add(&chunk->link, &fa->chunks);
		fa->free_ptr = base + sizeof(*chunk);
		fa->remaining = chunk_size - sizeof(*chunk);
	}

	elem = fa->free_ptr;
	fa->free_ptr += fa->elem_size;
	fa->remaining -= fa->elem_size;
	spin_unlock_irqrestore(&fa->lock, flags);

	memset(elem, 0, fa->elem_size);
	return elem;
}
EXPORT_SYMBOL_GPL(folio_arena_alloc);

void folio_arena_free(struct folio_arena *fa)
{
	struct folio_arena_chunk *chunk, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&fa->lock, flags);
	fa->free_ptr = NULL;
	fa->remaining = 0;
	list_for_each_entry_safe(chunk, tmp, &fa->chunks, link) {
		struct folio *folio = chunk->folio;
		list_del(&chunk->link);
		folio_put(folio);
	}
	spin_unlock_irqrestore(&fa->lock, flags);
}
EXPORT_SYMBOL_GPL(folio_arena_free);
