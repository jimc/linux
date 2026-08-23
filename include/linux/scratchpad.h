/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Scoped Lexical Scratchpad Memory Subsystem
 *
 * Unified scoped memory allocator for ephemeral bursts, intra-scope reuse,
 * and bulk O(1) reset. Backed by compound buddy pages (alloc_pages_node).
 *
 * Copyright (C) 2026 Jim Cromie <jim.cromie@gmail.com>
 */
#ifndef _LINUX_SCRATCHPAD_H
#define _LINUX_SCRATCHPAD_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/jump_label.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/cleanup.h>

#define SCRATCHPAD_DEFAULT_ORDER  (PAGE_SHIFT < 16 ? 16 - PAGE_SHIFT : 0) /* 64 KB */
#define SCRATCH_64K_ORDER         SCRATCHPAD_DEFAULT_ORDER
#define SCRATCH_32K_ORDER         (PAGE_SHIFT < 15 ? 15 - PAGE_SHIFT : 0)
#define SCRATCH_16K_ORDER         (PAGE_SHIFT < 14 ? 14 - PAGE_SHIFT : 0)
#define SCRATCH_8K_ORDER          (PAGE_SHIFT < 13 ? 13 - PAGE_SHIFT : 0)

/* Internal chunk header embedded at the start of each compound buddy page */
struct scratchpad_chunk {
	struct list_head	link;
	struct page		*page;
	unsigned int		order;
};

/*
 * The Universal Scoped Scratchpad Descriptor
 */
struct scratchpad {
	struct list_head	chunks;
	void			*free_ptr;
	size_t			remaining;
	void			*static_buf;
	size_t			static_size;
	size_t			elem_size;
	size_t			align_quantum;
	unsigned int		order;
	gfp_t			gfp;
	void			*freelist;
	struct static_key	*key;
};

#define DEFINE_SCRATCHPAD(type, name, ...)				\
	struct scratchpad name = {					\
		.chunks        = LIST_HEAD_INIT((name).chunks),		\
		.elem_size     = sizeof(type),				\
		.align_quantum = __alignof__(type),			\
		.order         = SCRATCHPAD_DEFAULT_ORDER,		\
		.gfp           = GFP_KERNEL,				\
		__VA_ARGS__						\
	}

#define DEFINE_SCRATCHPAD_STATIC(type, name, _buf, ...)			\
	struct scratchpad name = {					\
		.chunks        = LIST_HEAD_INIT((name).chunks),		\
		.free_ptr      = (_buf),				\
		.remaining     = sizeof(_buf),				\
		.static_buf    = (_buf),				\
		.static_size   = sizeof(_buf),				\
		.elem_size     = sizeof(type),				\
		.align_quantum = __alignof__(type),			\
		.order         = SCRATCHPAD_DEFAULT_ORDER,		\
		.gfp           = GFP_KERNEL,				\
		__VA_ARGS__						\
	}

/*
 * Static Branch Parameter Support (A/B Runtime Toggles)
 */
#define __DEFINE_SCRATCH_STATIC_KEY_PARAM(key_name, param_name, init_macro, desc) \
	init_macro(key_name);							\
	static int param_name##_set(const char *val, const struct kernel_param *kp) \
	{									\
		bool enable;							\
		int ret = kstrtobool(val, &enable);				\
		if (ret)							\
			return ret;						\
		if (enable)							\
			static_branch_enable(&key_name);			\
		else								\
			static_branch_disable(&key_name);			\
		return 0;							\
	}									\
	static int param_name##_get(char *buffer, const struct kernel_param *kp)	\
	{									\
		return sprintf(buffer, "%c\n",					\
			       static_key_enabled(&key_name) ? 'Y' : 'N');	\
	}									\
	static const struct kernel_param_ops param_name##_ops = {		\
		.set = param_name##_set,					\
		.get = param_name##_get,					\
	};									\
	module_param_cb(param_name, &param_name##_ops, NULL, 0644);		\
	MODULE_PARM_DESC(param_name, desc)

#define DEFINE_SCRATCHPAD_PARAM(name, desc) \
	__DEFINE_SCRATCH_STATIC_KEY_PARAM(name##_ENABLE_KEY, name##_scratch,	\
					  DEFINE_STATIC_KEY_TRUE, desc)

#define DEFINE_SCRATCHPAD_PARAM_FALSE(name, desc) \
	__DEFINE_SCRATCH_STATIC_KEY_PARAM(name##_ENABLE_KEY, name##_scratch,	\
					  DEFINE_STATIC_KEY_FALSE, desc)

#define DEFINE_SCRATCHPAD_KEY(name) \
	DEFINE_STATIC_KEY_TRUE(name##_ENABLE_KEY)

#define NONE_ENABLE_KEY (*(struct static_key *)NULL)

static inline bool scratchpad_is_enabled(struct scratchpad *sp)
{
	if (!sp || !sp->key)
		return true;
	return static_key_enabled(sp->key);
}

/*
 * Savepoints & Scoped Checkpointing
 */
struct scratchpad_mark {
	void			*free_ptr;
	size_t			remaining;
	struct list_head	*chunk;
};

static inline struct scratchpad_mark scratchpad_mark(struct scratchpad *sp)
{
	return (struct scratchpad_mark){
		.free_ptr  = sp->free_ptr,
		.remaining = sp->remaining,
		.chunk     = sp->chunks.prev,
	};
}

void scratchpad_rewind(struct scratchpad *sp, struct scratchpad_mark mark);

/*
 * Allocations & Deallocations
 */
void *__scratchpad_alloc(struct scratchpad *sp, size_t size,
			 size_t align, gfp_t gfp);
void *scratchpad_alloc_align(struct scratchpad *sp, size_t size,
			     size_t align, gfp_t gfp);

static __always_inline void *scratchpad_alloc_gfp(struct scratchpad *sp,
						   size_t size, gfp_t gfp)
{
	if (unlikely(sp->freelist)) {
		void *elem = sp->freelist;

		sp->freelist = *(void **)elem;
		return elem;
	}

	return scratchpad_alloc_align(sp, size ?: sp->elem_size,
				      sp->align_quantum ?: sizeof(void *),
				      gfp ?: sp->gfp ?: GFP_KERNEL);
}

static __always_inline void *scratchpad_alloc(struct scratchpad *sp,
					      size_t size)
{
	return scratchpad_alloc_gfp(sp, size, sp->gfp ?: GFP_KERNEL);
}

static inline void scratchpad_discard(struct scratchpad *sp, void *ptr, size_t size)
{
	if (unlikely(!ptr))
		return;

	if (!size)
		size = sp->elem_size;

	/* Active tail: Fast 1-cycle stack pop */
	if (likely((char *)ptr + size == sp->free_ptr)) {
		sp->free_ptr = ptr;
		sp->remaining += size;
		return;
	}

	/* Intra-scope reuse: Recycle into LIFO freelist if large enough */
	if (size >= sizeof(void *)) {
		*(void **)ptr = sp->freelist;
		sp->freelist = ptr;
		return;
	}

	/* Interior node: Poison payload in debug mode to catch Use-After-Free */
#ifdef CONFIG_DEBUG_SCRATCHPAD
	memset(ptr, 0x6b, size);
#endif
}

static inline void scratchpad_put(struct scratchpad *sp, void *ptr)
{
	scratchpad_discard(sp, ptr, sp->elem_size);
}

void scratchpad_reset(struct scratchpad *sp);
void scratchpad_free(struct scratchpad *sp);

int __scratchpad_prime(struct scratchpad *sp, gfp_t gfp);
int scratchpad_prime(struct scratchpad *sp, gfp_t gfp);
int __scratchpad_reserve(struct scratchpad *sp, size_t min_bytes, gfp_t gfp);
int scratchpad_reserve(struct scratchpad *sp, size_t min_bytes, gfp_t gfp);

int _scratchpad_init(struct scratchpad *sp, size_t elem_size, size_t elem_align,
		     unsigned int order, struct static_key *key, gfp_t gfp);

#define scratchpad_init(sp, name, gfp) \
	_scratchpad_init((sp), sizeof(void *), __alignof__(void *), 0, \
			 (struct static_key *)&(name##_ENABLE_KEY), (gfp))

#define scratchpad_init_order(sp, name, order, gfp) \
	_scratchpad_init((sp), sizeof(void *), __alignof__(void *), (order), \
			 (struct static_key *)&(name##_ENABLE_KEY), (gfp))

#define scratchpad_init_typed(sp, name, elem_size, order, gfp) \
	_scratchpad_init((sp), (elem_size), sizeof(void *), (order), \
			 (struct static_key *)&(name##_ENABLE_KEY), (gfp))

#define SCRATCHPAD_INIT(name, _arr, _order, _key) {				\
	.chunks        = LIST_HEAD_INIT((name).chunks),				\
	.free_ptr      = (_arr),						\
	.remaining     = sizeof(_arr),						\
	.static_buf    = (_arr),						\
	.static_size   = sizeof(_arr),						\
	.elem_size     = sizeof((_arr)[0]),					\
	.align_quantum = __alignof__((_arr)[0]),				\
	.order         = (_order),						\
	.gfp           = GFP_KERNEL,						\
	.key           = (struct static_key *)(_key),				\
}

/*
 * Fallback-Safe Helper Macros
 */
#ifndef SCRATCHPAD_NO_FALLBACK
#define __SCRATCH_ALLOC(cond, scratch_alloc, slab_fallback) \
	((cond) ? (scratch_alloc) : (slab_fallback))
#define __SCRATCH_FREE(cond, slab_free) \
	do { if (!(cond)) slab_free; } while (0)
#else
#define __SCRATCH_ALLOC(cond, scratch_alloc, slab_fallback) \
	(true ? (scratch_alloc) : ((void)(cond), (slab_fallback)))
#define __SCRATCH_FREE(cond, slab_free) \
	do { if (0 && (cond)) slab_free; } while (0)
#endif

/*
 * Type-First Declarative Allocation API
 */
#define scratchpad_alloc_type(type, sp)						\
	__SCRATCH_ALLOC(scratchpad_is_enabled(sp),				\
			({							\
				type *__p;					\
				__p = ((type *)scratchpad_alloc_align(sp,	\
								      sizeof(type), \
								      __alignof__(type), \
								      (sp)->gfp ?: GFP_KERNEL)); \
				if (__p)					\
					memset(__p, 0, sizeof(type));		\
				__p;						\
			}),							\
			kzalloc(sizeof(type), (sp)->gfp ?: GFP_KERNEL))

#define scratchpad_alloc_obj(type, ptr, member)					\
	__SCRATCH_ALLOC(scratchpad_is_enabled(&(ptr)->member),			\
			({							\
				struct scratchpad *__sp = &(ptr)->member;	\
				type *__p;					\
				__p = ((type *)scratchpad_alloc_align(__sp,	\
								      sizeof(type), \
								      __alignof__(type), \
								      __sp->gfp ?: GFP_KERNEL)); \
				if (__p)					\
					memset(__p, 0, sizeof(type));		\
				__p;						\
			}),							\
			kzalloc(sizeof(type), (&(ptr)->member)->gfp ?: GFP_KERNEL))

#define scratchpad_alloc_objs(type, sp, count)					\
	__SCRATCH_ALLOC(scratchpad_is_enabled(sp),				\
			((type *)scratchpad_alloc_align(sp,			\
							sizeof(type) * (count), \
							__alignof__(type),	\
							(sp)->gfp ?: GFP_KERNEL)), \
			kvmalloc_objs(type, count, (sp)->gfp ?: GFP_KERNEL))

#define scratchpad_alloc_bytes(ptr, member, size)				\
	__SCRATCH_ALLOC(scratchpad_is_enabled(&(ptr)->member),			\
			({							\
				void *__p = scratchpad_alloc(&(ptr)->member, size); \
				if (__p)					\
					memset(__p, 0, size);			\
				__p;						\
			}),							\
			kzalloc(size, (&(ptr)->member)->gfp ?: GFP_KERNEL))

#define scratchpad_alloc_buf(sp, size)						\
	__SCRATCH_ALLOC(scratchpad_is_enabled(sp),				\
			scratchpad_alloc(sp, size),				\
			kvmalloc(size, (sp)->gfp ?: GFP_KERNEL))

#define scratchpad_free_objs(sp, obj)						\
	__SCRATCH_FREE(scratchpad_is_enabled(sp), kvfree(obj))

#define scratchpad_put_obj(ptr, member, obj) scratchpad_put(&(ptr)->member, obj)

#define scratchpad_free_obj(ptr, member, obj)					\
	__SCRATCH_FREE(scratchpad_is_enabled(&(ptr)->member), kfree(obj))

#define scratchpad_free_type(sp, obj)						\
	__SCRATCH_FREE(scratchpad_is_enabled(sp), kfree(obj))

#define scratchpad_free_key(name, obj)						\
	__SCRATCH_FREE(static_key_enabled(&(name##_ENABLE_KEY).key), kfree(obj))

#define scratchpad_free_buf(sp, buf)						\
	__SCRATCH_FREE(scratchpad_is_enabled(sp), kvfree(buf))

#define scratchpad_realloc_bytes(ptr, old_size, new_size, ...)			\
	({									\
		void *__new = kvmalloc(new_size, default_gfp(__VA_ARGS__, GFP_KERNEL)); \
		if (__new && (ptr))						\
			memcpy(__new, (ptr), min((size_t)(old_size), (size_t)(new_size))); \
		__new;								\
	 })

DEFINE_FREE(scratchpad, struct scratchpad *, if (!IS_ERR_OR_NULL(_T)) scratchpad_free(_T))

#endif /* _LINUX_SCRATCHPAD_H */
