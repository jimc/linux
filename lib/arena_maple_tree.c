// SPDX-License-Identifier: GPL-2.0+
/*
 * Specialized Arena Maple Tree Implementation.
 * Includes maple_tree.c with remapped symbols and overridden allocators.
 */
/* Block tracepoint generation in this translation unit */
#define TRACE_EVENT(...)
#define TRACE_EVENT_FN(...)
#define DECLARE_TRACE(...)
#define DEFINE_TRACE(...)
#define EXPORT_TRACEPOINT_SYMBOL(...)
#define EXPORT_TRACEPOINT_SYMBOL_GPL(...)
#define trace_ma_op(...)
#define trace_ma_read(...)
#define trace_ma_write(...)
#define tracepoint_string(x) (x)
#define _TRACE_MM_H

#include <linux/types.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <linux/mm.h>
#include <linux/maple_tree.h>
#include <linux/export.h>
#include <asm/barrier.h>

#define mas_alloc_cyclic        arena_mas_alloc_cyclic
#define mas_walk                arena_mas_walk
#define mas_empty_area          arena_mas_empty_area
#define mas_empty_area_rev      arena_mas_empty_area_rev
#define mas_store               arena_mas_store
#define mas_store_gfp           arena_mas_store_gfp
#define mas_store_prealloc      arena_mas_store_prealloc
#define mas_preallocate         arena_mas_preallocate
#define mas_destroy             arena_mas_destroy
#define mas_next                arena_mas_next
#define mas_next_range          arena_mas_next_range
#define mt_next                 arena_mt_next
#define mas_prev                arena_mas_prev
#define mas_prev_range          arena_mas_prev_range
#define mt_prev                 arena_mt_prev
#define mas_pause               arena_mas_pause
#define mas_find                arena_mas_find
#define mas_find_range          arena_mas_find_range
#define mas_find_rev            arena_mas_find_rev
#define mas_find_range_rev      arena_mas_find_range_rev
#define mas_erase               arena_mas_erase
#define mtree_load              arena_mtree_load
#define mtree_store_range       arena_mtree_store_range
#define mtree_store             arena_mtree_store
#define mtree_insert_range      arena_mtree_insert_range
#define mtree_insert            arena_mtree_insert
#define mtree_alloc_range       arena_mtree_alloc_range
#define mtree_alloc_cyclic      arena_mtree_alloc_cyclic
#define mtree_alloc_rrange      arena_mtree_alloc_rrange
#define mtree_erase             arena_mtree_erase
#define __mt_dup                arena___mt_dup
#define mtree_dup               arena_mtree_dup
#define __mt_destroy            arena___mt_destroy
#define mtree_destroy           arena_mtree_destroy
#define mt_find                 arena_mt_find
#define mt_find_after           arena_mt_find_after
#define maple_tree_tests_run    arena_maple_tree_tests_run
#define maple_tree_tests_passed arena_maple_tree_tests_passed
#define mt_cache_shrink         arena_mt_cache_shrink
#define mt_dump                 arena_mt_dump
#define mt_validate             arena_mt_validate
#define mas_dump                arena_mas_dump
#define mas_wr_dump             arena_mas_wr_dump

/* Remap non-static functions to avoid duplicate linker definitions */
#define mas_nomem               arena_mas_nomem
#define maple_tree_init         arena_maple_tree_init

/* Declare the arena-prefixed init/lock/unlock functions we need */
void arena_mas_init(struct ma_state *mas, struct maple_tree *mt, unsigned long index)
{
	mas_init(mas, mt, index);
}
EXPORT_SYMBOL(arena_mas_init);

void arena_mas_lock(struct ma_state *mas)
{
	mas_lock(mas);
}
EXPORT_SYMBOL(arena_mas_lock);

void arena_mas_unlock(struct ma_state *mas)
{
	mas_unlock(mas);
}
EXPORT_SYMBOL(arena_mas_unlock);

extern struct maple_tree *current_ddebug_write_tree;

/* Forward declarations of specialized helpers called inside maple_tree.c */
bool arena_mas_nomem(struct ma_state *mas, gfp_t gfp);
void arena_maple_tree_init(void);

static inline void *kmem_cache_alloc_specialized(struct kmem_cache *cache, gfp_t gfp);
static inline struct slab_sheaf *kmem_cache_prefill_sheaf_specialized(struct kmem_cache *cache, gfp_t gfp, unsigned int count);
static inline int kmem_cache_refill_sheaf_specialized(struct kmem_cache *cache, gfp_t gfp, struct slab_sheaf **sheaf, unsigned int count);
static inline void *kmem_cache_alloc_from_sheaf_specialized(struct kmem_cache *cache, gfp_t gfp, struct slab_sheaf *sheaf);
static inline unsigned int kmem_cache_sheaf_size_specialized(struct slab_sheaf *sheaf);
static inline void kmem_cache_return_sheaf_specialized(struct kmem_cache *cache, gfp_t gfp, struct slab_sheaf *sheaf);
static inline void kmem_cache_free_bulk_specialized(struct kmem_cache *cache, size_t size, void **nodes);
static inline void kfree_specialized(void *ptr);

/* Override allocation functions */
#undef kmem_cache_alloc
#define kmem_cache_alloc(cache, gfp) kmem_cache_alloc_specialized(cache, gfp)

#undef kmem_cache_prefill_sheaf
#define kmem_cache_prefill_sheaf(cache, gfp, count) kmem_cache_prefill_sheaf_specialized(cache, gfp, count)

#undef kmem_cache_refill_sheaf
#define kmem_cache_refill_sheaf(cache, gfp, sheaf, count) kmem_cache_refill_sheaf_specialized(cache, gfp, sheaf, count)

#undef kmem_cache_alloc_from_sheaf
#define kmem_cache_alloc_from_sheaf(cache, gfp, sheaf) kmem_cache_alloc_from_sheaf_specialized(cache, gfp, sheaf)

#undef kmem_cache_sheaf_size
#define kmem_cache_sheaf_size(sheaf) kmem_cache_sheaf_size_specialized(sheaf)

#undef kmem_cache_return_sheaf
#define kmem_cache_return_sheaf(cache, gfp, sheaf) kmem_cache_return_sheaf_specialized(cache, gfp, sheaf)

#undef kmem_cache_free_bulk
#define kmem_cache_free_bulk(cache, size, nodes) kmem_cache_free_bulk_specialized(cache, size, nodes)

#define kfree(ptr) kfree_specialized(ptr)

#include "maple_tree.c"

/* Clean up macros to define our specialized functions */
#undef kmem_cache_alloc
#undef kmem_cache_prefill_sheaf
#undef kmem_cache_refill_sheaf
#undef kmem_cache_alloc_from_sheaf
#undef kmem_cache_sheaf_size
#undef kmem_cache_return_sheaf
#undef kmem_cache_free_bulk
#undef kfree

static inline void *kmem_cache_alloc_specialized(struct kmem_cache *cache, gfp_t gfp)
{
	struct maple_tree *mt = current_ddebug_write_tree;
	if (mt && cache == maple_node_cache && (mt->ma_flags & MT_FLAGS_ARENA)) {
		struct arena_maple_tree *amt = container_of(mt, struct arena_maple_tree, mt);
		return ddebug_arena_alloc(amt->arena, gfp);
	}
	return kmem_cache_alloc_noprof(cache, gfp);
}

static inline struct slab_sheaf *kmem_cache_prefill_sheaf_specialized(struct kmem_cache *cache, gfp_t gfp, unsigned int count)
{
	struct maple_tree *mt = current_ddebug_write_tree;
	if (mt && cache == maple_node_cache && (mt->ma_flags & MT_FLAGS_ARENA)) {
		struct arena_maple_tree *amt = container_of(mt, struct arena_maple_tree, mt);
		return (struct slab_sheaf *)amt->arena;
	}
	return kmem_cache_prefill_sheaf(cache, gfp, count);
}

static inline int kmem_cache_refill_sheaf_specialized(struct kmem_cache *cache, gfp_t gfp, struct slab_sheaf **sheaf, unsigned int count)
{
	struct maple_tree *mt = current_ddebug_write_tree;
	if (mt && cache == maple_node_cache && (mt->ma_flags & MT_FLAGS_ARENA)) {
		struct arena_maple_tree *amt = container_of(mt, struct arena_maple_tree, mt);
		*sheaf = (struct slab_sheaf *)amt->arena;
		return 0;
	}
	return kmem_cache_refill_sheaf(cache, gfp, sheaf, count);
}

static inline void *kmem_cache_alloc_from_sheaf_specialized(struct kmem_cache *cache, gfp_t gfp, struct slab_sheaf *sheaf)
{
	struct maple_tree *mt = current_ddebug_write_tree;
	if (mt && (mt->ma_flags & MT_FLAGS_ARENA)) {
		struct arena_maple_tree *amt = container_of(mt, struct arena_maple_tree, mt);
		if (sheaf == (struct slab_sheaf *)amt->arena)
			return ddebug_arena_alloc(amt->arena, gfp);
	}
	return kmem_cache_alloc_from_sheaf_noprof(cache, gfp, sheaf);
}

static inline unsigned int kmem_cache_sheaf_size_specialized(struct slab_sheaf *sheaf)
{
	struct maple_tree *mt = current_ddebug_write_tree;
	if (mt && (mt->ma_flags & MT_FLAGS_ARENA)) {
		struct arena_maple_tree *amt = container_of(mt, struct arena_maple_tree, mt);
		if (sheaf == (struct slab_sheaf *)amt->arena)
			return 1000;
	}
	return kmem_cache_sheaf_size(sheaf);
}

static inline void kmem_cache_return_sheaf_specialized(struct kmem_cache *cache, gfp_t gfp, struct slab_sheaf *sheaf)
{
	if (sheaf == (struct slab_sheaf *)0x1234 || sheaf == (struct slab_sheaf *)0x5678)
		return;
	kmem_cache_return_sheaf(cache, gfp, sheaf);
}

static inline void kmem_cache_free_bulk_specialized(struct kmem_cache *cache, size_t size, void **nodes)
{
	struct maple_tree *mt = current_ddebug_write_tree;
	if (mt && (mt->ma_flags & MT_FLAGS_ARENA))
		return;
	kmem_cache_free_bulk(cache, size, nodes);
}

static inline void kfree_specialized(void *ptr)
{
	struct maple_tree *mt = current_ddebug_write_tree;
	if (mt && (mt->ma_flags & MT_FLAGS_ARENA))
		return;
	kfree(ptr);
}
