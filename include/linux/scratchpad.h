/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Scratchpad & Scratchrec Ephemeral Memory Subsystem
 *
 * Transactional and epoch-scoped memory allocators (Micro-Stack and Micro-Heap)
 * built on top of the Simple-Slab engine (<linux/simple_slab.h>).
 *
 * Provides instant bulk reset (scratchpad_reset), teardown (scratchpad_free),
 * static-branch keyed slab fallbacks, and RAII cleanup macros.
 *
 * Copyright (C) 2026 Jim Cromie <jim.cromie@gmail.com>
 */
#ifndef _LINUX_SCRATCHPAD_H
#define _LINUX_SCRATCHPAD_H

#include <linux/simple_slab.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

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

#define SCRATCHPAD_INIT(name, _arr, _min_order, _key) \
	__SCRATCHPAD_INIT(name, _arr, __SCRATCH_BUF_SIZE(_arr), \
			  _min_order, SCRATCH_DEFAULT_MAX_ORDER, _key)

#define __SCRATCH_BUF_SIZE(_arr) \
	__builtin_choose_expr(__same_type((_arr), &(_arr)[0]), 0UL, sizeof(_arr))

int _scratchpad_init(struct scratchpad *sp, unsigned int order,
		     struct static_key *key, gfp_t gfp);

#define scratchpad_init(sp, name, gfp) \
	_scratchpad_init((sp), 0, (struct static_key *)&(name##_ENABLE_KEY), (gfp))

#define scratchpad_init_order(sp, name, order, gfp) \
	_scratchpad_init((sp), (order), (struct static_key *)&(name##_ENABLE_KEY), (gfp))

#define scratchpad_init_key(sp, order, key, gfp) \
	_scratchpad_init((sp), (order), (struct static_key *)(key), (gfp))

int __scratchpad_reserve(struct scratchpad *sp, size_t min_bytes, gfp_t gfp);
int scratchpad_reserve(struct scratchpad *sp, size_t min_bytes, gfp_t gfp);
void __scratchpad_reset(struct scratchpad *sp);
void scratchpad_reset(struct scratchpad *sp);
void __scratchpad_free(struct scratchpad *sp);
void scratchpad_free(struct scratchpad *sp);
int __scratchpad_prime(struct scratchpad *sp, gfp_t gfp);
int scratchpad_prime(struct scratchpad *sp, gfp_t gfp);

static inline bool scratchpad_has_space(struct scratchpad *sp,
					size_t bytes, size_t align)
{
	void *free_p = READ_ONCE(sp->free_ptr);
	size_t rem = READ_ONCE(sp->remaining);
	void *aligned;
	size_t pad;

	if (!free_p)
		return false;

	if (!align)
		align = __alignof__(unsigned long);

	aligned = PTR_ALIGN(free_p, align);
	pad = (size_t)(aligned - free_p);

	return rem >= (bytes + pad);
}

DEFINE_FREE(scratchpad, struct scratchpad *, if (!IS_ERR_OR_NULL(_T)) scratchpad_free(_T))

#define scratchpad_discard_obj(sp, ptr) \
	scratchpad_discard(sp, ptr, sizeof(*(ptr)))

#define __scratchpad_discard_obj(sp, ptr) \
	__scratchpad_discard(sp, ptr, sizeof(*(ptr)))

#define __scratchpad_alloc_obj(ptr, member, type, gfp)				\
	((type *)__scratchpad_alloc(&(ptr)->member,				\
				    sizeof(type),				\
				    __alignof__(type),				\
				    gfp))

#define __scratchpad_alloc_bytes(ptr, member, size, align, gfp)			\
	__scratchpad_alloc(&(ptr)->member, size, align, gfp)

#define __scratchpad_alloc_type(sp, type, gfp)					\
	((type *)__scratchpad_alloc(sp,						\
				    sizeof(type),				\
				    __alignof__(type),				\
				    gfp))

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

#define scratchpad_alloc_obj(ptr, member, type, gfp)				\
	__SCRATCH_ALLOC(scratchpad_is_enabled(&(ptr)->member),			\
			({							\
				type *__p = ((type *)scratchpad_alloc_align(&(ptr)->member, \
									    sizeof(type), \
									    __alignof__(type), \
									    gfp)); \
				if (__p)					\
					memset(__p, 0, sizeof(type));		\
				__p;						\
			}),							\
			kzalloc(sizeof(type), gfp))

#define scratchpad_alloc_bytes(ptr, member, size, gfp)				\
	__SCRATCH_ALLOC(scratchpad_is_enabled(&(ptr)->member),			\
			({							\
				void *__p = scratchpad_alloc(&(ptr)->member, size, gfp); \
				if (__p)					\
					memset(__p, 0, size);			\
				__p;						\
			}),							\
			kzalloc(size, gfp))

#define scratchpad_alloc_type(sp, type, gfp)					\
	__SCRATCH_ALLOC(scratchpad_is_enabled(sp),				\
			({							\
				type *__p = ((type *)scratchpad_alloc_align(sp,	\
									    sizeof(type), \
									    __alignof__(type), \
									    gfp)); \
				if (__p)					\
					memset(__p, 0, sizeof(type));		\
				__p;						\
			}),							\
			kzalloc(sizeof(type), gfp))

#define scratchpad_alloc_objs(sp, P, count, ...)				\
	__SCRATCH_ALLOC(scratchpad_is_enabled(sp),				\
			((typeof(P) *)scratchpad_alloc(sp, sizeof(P) * (count), \
						       default_gfp(__VA_ARGS__))), \
			kvmalloc_objs(P, count, ##__VA_ARGS__))

#define scratchpad_free_objs(sp, obj)						\
	__SCRATCH_FREE(scratchpad_is_enabled(sp), kvfree(obj))

#define scratchpad_alloc_buf(sp, size, gfp)					\
	__SCRATCH_ALLOC(scratchpad_is_enabled(sp),				\
			scratchpad_alloc(sp, size, gfp),			\
			kvmalloc(size, gfp))

#define scratchpad_free_buf(sp, buf)						\
	__SCRATCH_FREE(scratchpad_is_enabled(sp), kvfree(buf))

#define scratchpad_free_obj(ptr, member, obj)					\
	__SCRATCH_FREE(scratchpad_is_enabled(&(ptr)->member), kfree(obj))

#define scratchpad_free_type(sp, obj)						\
	__SCRATCH_FREE(scratchpad_is_enabled(sp), kfree(obj))

#define scratchpad_free_key(name, obj)						\
	__SCRATCH_FREE(static_key_enabled(&(name##_ENABLE_KEY).key), kfree(obj))

#define scratchpad_realloc_bytes(ptr, old_size, new_size, gfp)			\
	({									\
		void *__new = kvmalloc(new_size, gfp);				\
		if (__new && (ptr))						\
			memcpy(__new, (ptr), min((size_t)(old_size), (size_t)(new_size))); \
		__new;								\
	 })

/*
 * Ephemeral Micro-Heap (struct scratchrec with LIFO freelist & reset)
 */
struct scratchrec {
	struct scratchpad base;
	void *freelist;
	size_t elem_size;
	size_t elem_align;
};

#define SCRATCHREC_INIT(name, _arr, _min_order, _key) {				\
	.base = SCRATCHPAD_INIT((name).base, _arr, _min_order, _key),		\
	.freelist = NULL,							\
	.elem_size = sizeof((_arr)[0]) +					\
		     __must_be_array(_arr) +					\
		     BUILD_BUG_ON_ZERO(ARRAY_SIZE(_arr) < 2) +			\
		     BUILD_BUG_ON_ZERO(sizeof((_arr)[0]) < sizeof(void *)),	\
	.elem_align = sizeof(void *),						\
}

#define SCRATCHREC_INIT_RADIX(name, _elem_size, _order, _key) {			\
	.base = SCRATCHPAD_INIT_RADIX((name).base, _order, _key),		\
	.freelist = NULL,							\
	.elem_size = (_elem_size) +						\
		     BUILD_BUG_ON_ZERO((_elem_size) < sizeof(void *)),		\
	.elem_align = sizeof(void *),						\
}

int _scratchrec_init(struct scratchrec *sr, size_t elem_size, unsigned int order,
		      struct static_key *key, gfp_t gfp);

#define scratchrec_init(sr, name, elem_size, gfp) \
	_scratchrec_init((sr), (elem_size), 0, (struct static_key *)&(name##_ENABLE_KEY), (gfp))

#define scratchrec_init_order(sr, name, elem_size, order, gfp) \
	_scratchrec_init((sr), (elem_size), (order), \
			 (struct static_key *)&(name##_ENABLE_KEY), (gfp))

#define scratchrec_init_key(sr, elem_size, order, key, gfp) \
	_scratchrec_init((sr), (elem_size), (order), (struct static_key *)(key), (gfp))

int _scratchrec_init_align(struct scratchrec *sr, size_t elem_size,
			    size_t elem_align, unsigned int order,
			    struct static_key *key, gfp_t gfp);
#define scratchrec_init_align(sr, elem_size, elem_align, order, key, gfp) \
	_scratchrec_init_align((sr), (elem_size), (elem_align), (order), \
			       (struct static_key *)(key), (gfp))

static __always_inline void *__scratchrec_alloc_fast(struct scratchrec *sr,
						     size_t elem_size,
						     size_t elem_align,
						     gfp_t gfp)
{
	if (likely(sr->freelist)) {
		void *elem = sr->freelist;

		sr->freelist = *(void **)elem;
		return elem;
	}

	if (elem_align <= sizeof(void *)) {
		void *free_p = sr->base.free_ptr;

		if (likely(sr->base.remaining >= elem_size && free_p)) {
			sr->base.free_ptr = (char *)free_p + elem_size;
			sr->base.remaining -= elem_size;
			return free_p;
		}
	}

	return __scratchpad_alloc(&sr->base, elem_size, elem_align, gfp);
}

static inline void *__scratchrec_alloc(struct scratchrec *sr, gfp_t gfp)
{
	return __scratchrec_alloc_fast(sr, sr->elem_size, sr->elem_align, gfp);
}

static inline void *scratchrec_alloc(struct scratchrec *sr, gfp_t gfp)
{
	return __scratchrec_alloc(sr, gfp);
}

static inline void __scratchrec_put(struct scratchrec *sr, void *ptr)
{
	if (!ptr)
		return;
	*(void **)ptr = sr->freelist;
	sr->freelist = ptr;
}

static inline void *__scratchrec_get(struct scratchrec *sr)
{
	void *elem = sr->freelist;

	if (likely(elem)) {
		sr->freelist = *(void **)elem;
		return elem;
	}
	return NULL;
}

static inline void *scratchrec_get(struct scratchrec *sr)
{
	return __scratchrec_get(sr);
}

static inline void scratchrec_put(struct scratchrec *sr, void *ptr)
{
	__scratchrec_put(sr, ptr);
}

int __scratchrec_prime(struct scratchrec *sr, gfp_t gfp);
int scratchrec_prime(struct scratchrec *sr, gfp_t gfp);

static inline size_t scratchrec_avail(struct scratchrec *sr)
{
	return scratchpad_avail(&sr->base);
}

static inline unsigned int scratchrec_avail_objs(struct scratchrec *sr)
{
	return sr->elem_size ? (scratchpad_avail(&sr->base) / sr->elem_size) : 0;
}

static inline bool scratchrec_has_space(struct scratchrec *sr, unsigned int nr_elems)
{
	return scratchpad_has_space(&sr->base, nr_elems * sr->elem_size, sr->elem_align);
}

static inline void scratchrec_free(struct scratchrec *sr)
{
	sr->freelist = NULL;
	scratchpad_free(&sr->base);
}

static inline void scratchrec_reset(struct scratchrec *sr)
{
	sr->freelist = NULL;
	scratchpad_reset(&sr->base);
}

static inline void __scratchrec_stats(struct scratchrec *sr, unsigned int *nr_chunks,
				      size_t *chunk_size, size_t *tail_used)
{
	__scratchpad_stats(&sr->base, nr_chunks, chunk_size, tail_used);
}

static inline void scratchrec_stats(struct scratchrec *sr, unsigned int *nr_chunks,
				    size_t *chunk_size, size_t *tail_used)
{
	scratchpad_stats(&sr->base, nr_chunks, chunk_size, tail_used);
}

DEFINE_FREE(scratchrec, struct scratchrec *, if (!IS_ERR_OR_NULL(_T)) scratchrec_free(_T))

#define __scratchrec_alloc_obj(ptr, member, type, gfp)				\
	((type *)__scratchrec_alloc_fast(&(ptr)->member, sizeof(type), __alignof__(type), gfp))

#define scratchrec_alloc_obj(ptr, member, type, gfp)				\
	__SCRATCH_ALLOC(scratchpad_is_enabled(&(ptr)->member.base),		\
			({							\
				type *__p = ((type *)scratchrec_alloc(&(ptr)->member, gfp)); \
				if (__p)					\
					memset(__p, 0, sizeof(type));		\
				__p;						\
			}),							\
			kzalloc(sizeof(type), gfp))

#define scratchrec_put_obj(ptr, member, obj)					\
	scratchrec_put(&(ptr)->member, obj)

#define __scratchrec_put_obj(ptr, member, obj)					\
	__scratchrec_put(&(ptr)->member, obj)

#define __scratchrec_alloc_type(sr, type, gfp)					\
	((type *)__scratchrec_alloc_fast((sr), sizeof(type), __alignof__(type), (gfp)))

#define scratchrec_alloc_type(sr, type, gfp)					\
	__SCRATCH_ALLOC(scratchpad_is_enabled(&(sr)->base),			\
			({							\
				type *__p = ((type *)scratchrec_alloc(sr, gfp)); \
				if (__p)					\
					memset(__p, 0, sizeof(type));		\
				__p;						\
			}),							\
			kzalloc(sizeof(type), gfp))

#define scratchrec_free_obj(ptr, member, obj)					\
	__SCRATCH_FREE(scratchpad_is_enabled(&(ptr)->member.base), kfree(obj))

#define scratchrec_free_type(sr, obj)						\
	__SCRATCH_FREE(scratchpad_is_enabled(&(sr)->base), kfree(obj))

#endif /* _LINUX_SCRATCHPAD_H */
