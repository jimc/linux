// SPDX-License-Identifier: GPL-2.0

#include "alloc_cache.h"

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "io_uring."

DEFINE_SCRATCHPAD_PARAM(cache, "Toggle io_uring scratchpad alloc cache");

void io_alloc_cache_free(struct io_alloc_cache *cache,
			 void (*free)(const void *))
{
	void *entry;

	if (static_branch_likely(&cache_ENABLE_KEY)) {
		scratchpad_free(&cache->rec);
		return;
	}

	if (!cache->entries)
		return;

	while ((entry = io_alloc_cache_get(cache)) != NULL)
		free(entry);

	kvfree(cache->entries);
	cache->entries = NULL;
}

/* returns false if the cache was initialized properly */
bool io_alloc_cache_init(struct io_alloc_cache *cache,
			 unsigned max_nr, unsigned int size,
			 unsigned int init_bytes)
{
	if (static_branch_likely(&cache_ENABLE_KEY)) {
		_scratchpad_init(&cache->rec, size, sizeof(void *), 0,
				 (struct static_key *)&cache_ENABLE_KEY, GFP_KERNEL);
		cache->entries = NULL;
		cache->nr_cached = 0;
		cache->max_cached = max_nr;
		cache->elem_size = size;
		cache->init_clear = init_bytes;
		return false;
	}

	cache->entries = kvmalloc_array(max_nr, sizeof(void *), GFP_KERNEL);
	if (!cache->entries)
		return true;

	cache->nr_cached = 0;
	cache->max_cached = max_nr;
	cache->elem_size = size;
	cache->init_clear = init_bytes;
	return false;
}

void *io_cache_alloc_new(struct io_alloc_cache *cache, gfp_t gfp)
{
	void *obj;

	obj = kmalloc(cache->elem_size, gfp);
	if (obj && cache->init_clear)
		memset(obj, 0, cache->init_clear);
	return obj;
}
