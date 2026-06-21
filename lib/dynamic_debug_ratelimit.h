/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LIB_DYNAMIC_DEBUG_RATELIMIT_H
#define _LIB_DYNAMIC_DEBUG_RATELIMIT_H

#include <linux/dynamic_debug.h>

struct _ddebug;
struct _ddebug_info;

#ifdef CONFIG_DYNAMIC_DEBUG_CORE

bool ddebug_apply_ratelimit_solo(struct _ddebug *desc);
bool ddebug_apply_ratelimit_shared(struct _ddebug *desc);

static inline bool ddebug_apply_ratelimit_all(struct _ddebug *desc)
{
	if (desc->flags & _DPRINTK_FLAGS_RATELIMIT_SOLO) {
		if (!ddebug_apply_ratelimit_solo(desc))
			return false;
	} else if (desc->flags & _DPRINTK_FLAGS_RATELIMIT_SHARED) {
		if (!ddebug_apply_ratelimit_shared(desc))
			return false;
	}
	return true;
}

void ddebug_ratelimit_update(struct _ddebug *dp, unsigned int oldflags, unsigned int newflags);
void ddebug_ratelimit_clear_query(void);
void ddebug_ratelimit_free_info(const struct _ddebug_info *di);

#else

static inline bool ddebug_apply_ratelimit_all(struct _ddebug *desc) { return true; }
static inline void ddebug_ratelimit_update(struct _ddebug *dp, unsigned int oldflags, unsigned int newflags) {}
static inline void ddebug_ratelimit_clear_query(void) {}
static inline void ddebug_ratelimit_free_info(const struct _ddebug_info *di) {}

#endif

#endif /* _LIB_DYNAMIC_DEBUG_RATELIMIT_H */
