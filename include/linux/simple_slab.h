/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Simple Slab, Ledger & 2D-Radix Memory Subsystem
 *
 * Lightweight compound buddy-page allocator (alloc_pages_node(..., __GFP_COMP, ...))
 * providing monotonic byte streams (scratchpad), fixed-slot record pools with
 * intrusive LIFO freelists (ledger), and 2D bit-shift radix matrices for
 * permanent kernel databases (e.g. Lockdep).
 *
 * Copyright (C) 2026 Jim Cromie <jim.cromie@gmail.com>
 */
#ifndef _LINUX_SIMPLE_SLAB_H
#define _LINUX_SIMPLE_SLAB_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/list.h>
#include <linux/jump_label.h>
#include <linux/mm.h>
#include <linux/build_bug.h>
#include <linux/err.h>

#define SCRATCH_8K_ORDER  (PAGE_SHIFT < 13 ? 13 - PAGE_SHIFT : 0)
#define SCRATCH_16K_ORDER (PAGE_SHIFT < 14 ? 14 - PAGE_SHIFT : 0)
#define SCRATCH_32K_ORDER (PAGE_SHIFT < 15 ? 15 - PAGE_SHIFT : 0)
#define SCRATCH_64K_ORDER (PAGE_SHIFT < 16 ? 16 - PAGE_SHIFT : 0)

#ifndef SCRATCH_MAX_ORDER
#define SCRATCH_MAX_ORDER MAX_PAGE_ORDER
#endif

#define SCRATCH_CHECK_ORDER(_order) \
	BUILD_BUG_ON_ZERO((_order) > SCRATCH_MAX_ORDER)

#define SCRATCH_INIT_DWELL 3

#ifndef SCRATCH_DEFAULT_MAX_ORDER
#define SCRATCH_DEFAULT_MAX_ORDER SCRATCH_64K_ORDER
#endif

/*
 * 0. Physical Substrate: In-band chunk header placed at offset 0
 */
struct simple_slab {
	struct list_head link;
	struct page *page;
	unsigned int order;
};
#define sslab simple_slab
#define scratch_chunk simple_slab

enum scratchpad_flags {
	SCRATCH_F_RAW_CHUNKS  = BIT(0),  /* Bare full-page slabs (no in-band header or sp->chunks link) */
	SCRATCH_F_PERMANENT   = BIT(1),  /* Permanent substrate (Lockdep) */
	SCRATCH_F_NO_FALLBACK = BIT(2),  /* Strict allocator, no slab fallback */
};

/*
 * Slab Allocation & Growth Policy Controller
 */
struct ss_growth {
	u8 init_order;   /* Starting slab allocation order */
	u8 chunk_order;  /* Current ramped chunk order */
	u8 max_order;    /* Ceiling allocation order */
	u8 dwell_cur;    /* Allocations at current order before ramp */
	u8 flags;        /* SCRATCH_F_* */
};

/*
 * 1. Monotonic Byte Stream (Linear Bump Allocator)
 */
struct scratchpad {
	struct list_head chunks;
	void *free_ptr;
	size_t remaining;
	void *static_buf;
	size_t static_size;
	struct ss_growth growth;
	struct static_key *key;
};

#define SS_GROWTH_INIT(_min_order, _max_order, _flags) {			\
	.init_order = (_min_order) + SCRATCH_CHECK_ORDER(_min_order),		\
	.chunk_order = (_min_order) + SCRATCH_CHECK_ORDER(_min_order),		\
	.max_order = (_max_order) + SCRATCH_CHECK_ORDER(_max_order),		\
	.dwell_cur = ((_max_order) > (_min_order)) ? SCRATCH_INIT_DWELL : 0,	\
	.flags = (_flags),							\
}

#define __SCRATCHPAD_INIT_FLAGS(name, _buf, _sz, _min_order, _max_order, _flags, _key) { \
	.chunks = LIST_HEAD_INIT((name).chunks),				\
	.free_ptr = (_buf),							\
	.remaining = (_sz),							\
	.static_buf = (_buf),							\
	.static_size = (_sz),							\
	.growth = SS_GROWTH_INIT(_min_order, _max_order, _flags),		\
	.key = (struct static_key *)(_key),					\
}

#define __SCRATCHPAD_INIT(name, _buf, _sz, _min_order, _max_order, _key)	\
	__SCRATCHPAD_INIT_FLAGS(name, _buf, _sz, _min_order, _max_order, 0, _key)

#define __SCRATCH_BUF_SIZE(_arr) \
	__builtin_choose_expr(__same_type((_arr), &(_arr)[0]), 0UL, sizeof(_arr))

#define SCRATCHPAD_INIT(name, _arr, _min_order, _key) \
	__SCRATCHPAD_INIT(name, _arr, __SCRATCH_BUF_SIZE(_arr), \
			  _min_order, SCRATCH_DEFAULT_MAX_ORDER, _key)

#define SCRATCHPAD_INIT_RADIX(name, _order, _key) \
	__SCRATCHPAD_INIT_FLAGS(name, NULL, 0, _order, _order, \
				SCRATCH_F_RAW_CHUNKS | SCRATCH_F_PERMANENT, _key)
#define SCRATCHPAD_INIT_FIXED SCRATCHPAD_INIT_RADIX

static inline bool scratchpad_is_enabled(struct scratchpad *sp)
{
	if (!sp || !sp->key)
		return true;
	return static_key_enabled(sp->key);
}

void *__scratchpad_alloc(struct scratchpad *sp, size_t size,
			 size_t align, gfp_t gfp);
void *scratchpad_alloc_align(struct scratchpad *sp, size_t size,
			     size_t align, gfp_t gfp);

static inline void *scratchpad_alloc(struct scratchpad *sp, size_t size, gfp_t gfp)
{
	return scratchpad_alloc_align(sp, size, sizeof(void *), gfp);
}

void __scratchpad_discard(struct scratchpad *sp, void *ptr, size_t size);
void scratchpad_discard(struct scratchpad *sp, void *ptr, size_t size);
void __scratchpad_trim(struct scratchpad *sp, size_t unused_bytes);
void scratchpad_trim(struct scratchpad *sp, size_t unused_bytes);

void __scratchpad_stats(struct scratchpad *sp, unsigned int *nr_chunks,
			size_t *chunk_size, size_t *tail_used);
void scratchpad_stats(struct scratchpad *sp, unsigned int *nr_chunks,
		      size_t *chunk_size, size_t *tail_used);

static inline size_t scratchpad_avail(struct scratchpad *sp)
{
	return READ_ONCE(sp->remaining);
}

/*
 * 2. Monotonic Ledger (Fixed-Slot Record Pool with Intrusive LIFO Freelist)
 */
struct ledger {
	struct scratchpad base;
	void *freelist;
	size_t elem_size;
	size_t elem_align;
};

#define LEDGER_INIT(name, _arr, _min_order, _key) {				\
	.base = SCRATCHPAD_INIT((name).base, _arr, _min_order, _key),		\
	.freelist = NULL,							\
	.elem_size = sizeof((_arr)[0]) +					\
		     __must_be_array(_arr) +					\
		     BUILD_BUG_ON_ZERO(ARRAY_SIZE(_arr) < 2) +			\
		     BUILD_BUG_ON_ZERO(sizeof((_arr)[0]) < sizeof(void *)),	\
	.elem_align = sizeof(void *),						\
}

#define LEDGER_INIT_RADIX(name, _elem_size, _order, _key) {			\
	.base = SCRATCHPAD_INIT_RADIX((name).base, _order, _key),		\
	.freelist = NULL,							\
	.elem_size = (_elem_size) +						\
		     BUILD_BUG_ON_ZERO((_elem_size) < sizeof(void *)),		\
	.elem_align = sizeof(void *),						\
}

static __always_inline void *__ledger_alloc_fast(struct ledger *l,
						 size_t elem_size,
						 size_t elem_align,
						 gfp_t gfp)
{
	if (unlikely(l->freelist)) {
		void *elem = l->freelist;

		l->freelist = *(void **)elem;
		return elem;
	}

	if (__builtin_constant_p(elem_size) && elem_align <= sizeof(void *)) {
		void *free_p = l->base.free_ptr;

		if (likely(l->base.remaining >= elem_size && free_p)) {
			l->base.free_ptr = (char *)free_p + elem_size;
			l->base.remaining -= elem_size;
			return free_p;
		}
	}

	return __scratchpad_alloc(&l->base, elem_size, elem_align, gfp);
}

static inline void *__ledger_alloc(struct ledger *l, gfp_t gfp)
{
	return __ledger_alloc_fast(l, l->elem_size, l->elem_align, gfp);
}

static inline void *ledger_alloc(struct ledger *l, gfp_t gfp)
{
	return __ledger_alloc(l, gfp);
}

#define __ledger_alloc_type(l, type, gfp) \
	((type *)__ledger_alloc_fast((l), sizeof(type), __alignof__(type), (gfp)))
#define ledger_alloc_type(l, type, gfp) \
	((type *)ledger_alloc((l), (gfp)))

static inline void __ledger_put(struct ledger *l, void *ptr)
{
	if (!ptr)
		return;
	*(void **)ptr = l->freelist;
	l->freelist = ptr;
}

static inline void ledger_put(struct ledger *l, void *ptr)
{
	__ledger_put(l, ptr);
}

static inline void __ledger_stats(struct ledger *l, unsigned int *nr_chunks,
				  size_t *chunk_size, size_t *tail_used)
{
	__scratchpad_stats(&l->base, nr_chunks, chunk_size, tail_used);
}

static inline void ledger_stats(struct ledger *l, unsigned int *nr_chunks,
				size_t *chunk_size, size_t *tail_used)
{
	scratchpad_stats(&l->base, nr_chunks, chunk_size, tail_used);
}

/*
 * 3. Two-Level 2D Radix Matrix of Fixed 64KB Slabs
 */
#define DEFINE_SIMPLE_2D_RADIX(name, type, max_elems, key)			\
	enum {									\
		name##_SHIFT = 16 - ilog2(sizeof(type)),			\
		name##_PER_CHUNK = 1UL << name##_SHIFT,				\
		name##_CHUNK_MASK = name##_PER_CHUNK - 1,			\
		name##_NR_CHUNKS = (max_elems) >> name##_SHIFT			\
	};									\
	static type name##_chunk0[name##_PER_CHUNK];				\
	static type *name##_chunks[name##_NR_CHUNKS] = { name##_chunk0 };	\
	static unsigned int nr_##name##_chunks = 1;				\
	static struct ledger name##_pool =					\
		LEDGER_INIT_RADIX(name##_pool,					\
				  sizeof(type) * name##_PER_CHUNK,		\
				  SCRATCH_64K_ORDER, (key));			\
	type *idx_to_##name(unsigned int idx);					\
	type *idx_to_##name(unsigned int idx)					\
	{									\
		unsigned int chunk = idx >> name##_SHIFT;			\
		unsigned int offset = idx & name##_CHUNK_MASK;			\
		if (unlikely(chunk >= name##_NR_CHUNKS || !name##_chunks[chunk])) \
			return NULL;						\
		return &name##_chunks[chunk][offset];				\
	}
#define DEFINE_SIMPLE_SLAB_TABLE DEFINE_SIMPLE_2D_RADIX
#define DEFINE_SSLAB_TABLE DEFINE_SIMPLE_2D_RADIX
#define DEFINE_LEDGER_TABLE DEFINE_SIMPLE_2D_RADIX

#endif /* _LINUX_SIMPLE_SLAB_H */
