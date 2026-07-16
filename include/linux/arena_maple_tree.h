/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _LINUX_ARENA_MAPLE_TREE_H
#define _LINUX_ARENA_MAPLE_TREE_H

#include <linux/maple_tree.h>

#ifdef CONFIG_DYNAMIC_DEBUG

extern struct maple_node *ddebug_arena_alloc(void *arena, gfp_t gfp);
extern void arena_maple_tree_init(void);

extern void arena_mas_init(struct ma_state *mas, struct maple_tree *mt, unsigned long index);
extern void *arena_mas_walk(struct ma_state *mas);
extern void *arena_mas_prev(struct ma_state *mas, unsigned long min);
extern void *arena_mas_next(struct ma_state *mas, unsigned long max);
extern void arena_mas_lock(struct ma_state *mas);
extern void arena_mas_unlock(struct ma_state *mas);
extern void *arena_mas_erase(struct ma_state *mas);
extern int arena_mtree_store_range(struct maple_tree *mt, unsigned long start, unsigned long end, void *entry, gfp_t gfp);
extern void arena___mt_destroy(struct maple_tree *mt);
extern void arena_mtree_destroy(struct maple_tree *mt);
extern void arena_mas_destroy(struct ma_state *mas);
extern void *arena_mas_next_range(struct ma_state *mas, unsigned long max);
extern void arena_mas_pause(struct ma_state *mas);
extern void *arena_mas_find(struct ma_state *mas, unsigned long max);
extern void *arena_mtree_load(struct maple_tree *mt, unsigned long index);
extern int arena_mas_empty_area(struct ma_state *mas, unsigned long min, unsigned long max, unsigned long size);
extern int arena_mas_empty_area_rev(struct ma_state *mas, unsigned long min, unsigned long max, unsigned long size);

#endif /* CONFIG_DYNAMIC_DEBUG */

#endif /* _LINUX_ARENA_MAPLE_TREE_H */
