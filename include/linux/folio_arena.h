/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _LINUX_FOLIO_ARENA_H
#define _LINUX_FOLIO_ARENA_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/page-flags.h>
#include <linux/mm.h>
#include <linux/gfp.h>

#ifdef CONFIG_DYNAMIC_DEBUG_CORE

struct folio_arena_chunk {
	struct list_head link;
	struct folio *folio;
};

struct folio_arena {
	struct list_head chunks;
	void *free_ptr;
	size_t remaining;
	size_t elem_size;
	unsigned int chunk_order;
	spinlock_t lock;
};

#define FOLIO_ARENA_INIT(name, _elem_size, _order) {			\
	.chunks = LIST_HEAD_INIT((name).chunks),			\
	.elem_size = (_elem_size),					\
	.chunk_order = (_order),					\
	.lock = __SPIN_LOCK_UNLOCKED((name).lock),			\
}

void folio_arena_init(struct folio_arena *fa, size_t elem_size,
		      unsigned int order);
void *folio_arena_alloc(struct folio_arena *fa, gfp_t gfp);
void folio_arena_free(struct folio_arena *fa);

#else /* !CONFIG_DYNAMIC_DEBUG_CORE */

struct folio_arena { };

#define FOLIO_ARENA_INIT(name, _elem_size, _order) { }

static inline void folio_arena_init(struct folio_arena *fa, size_t elem_size,
				    unsigned int order) { }
static inline void *folio_arena_alloc(struct folio_arena *fa, gfp_t gfp)
{
	return NULL;
}
static inline void folio_arena_free(struct folio_arena *fa) { }

#endif /* CONFIG_DYNAMIC_DEBUG_CORE */

#endif /* _LINUX_FOLIO_ARENA_H */
