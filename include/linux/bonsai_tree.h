/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _LINUX_BONSAI_TREE_H
#define _LINUX_BONSAI_TREE_H

#include <linux/kernel.h>
#include <linux/types.h>

#define BONSAI_NODE_SIZE	256
#define BONSAI_BRANCH_SLOTS_8	28	/* 27 pivots + 28 u8 child indices (u8 seedling) */
#define BONSAI_BRANCH_SLOTS_16	25	/* 24 pivots + 25 u16 child indices (u16 expanded) */
#define BONSAI_BRANCH_SLOTS_32	50	/* 49 u32 pivots + 50 u8 child indices (mycelium 32-bit) */
#define BONSAI_LEAF_SLOTS	16	/* 15 pivots + 16 64-bit payload slots */

enum bonsai_node_type {
	BONSAI_TYPE_NONE = 0,
	BONSAI_TYPE_LEAF,
	BONSAI_TYPE_BRANCH_8,
	BONSAI_TYPE_BRANCH_16,
};

/*
 * Common 8-byte metadata located at offset 0 in all 256-byte nodes.
 */
struct bonsai_meta {
	u16 parent_idx;		/* Offset 0..1 */
	u8 parent_slot;		/* Offset 2 */
	u8 node_type;		/* Offset 3 (BONSAI_TYPE_LEAF / BRANCH_8 / BRANCH_16) */
	u8 num_pivots;		/* Offset 4 */
	u8 _pad[3];		/* Offset 5..7 -> Total: 8 bytes */
};

/*
 * Compact 256-byte 28-way branch node (u8 relative indexing).
 */
struct bonsai_branch_8 {
	struct bonsai_meta meta;				/* 8 bytes (0..7) */
	unsigned long pivot[BONSAI_BRANCH_SLOTS_8 - 1];		/* 216 bytes (8..223) */
	u8 child_idx[BONSAI_BRANCH_SLOTS_8];			/* 28 bytes (224..251) */
	u8 _pad[4];						/* 4 bytes (252..255) */
};

/*
 * Compact 256-byte 25-way branch node (u16 relative indexing).
 */
struct bonsai_branch_16 {
	struct bonsai_meta meta;				/* 8 bytes (0..7) */
	unsigned long pivot[BONSAI_BRANCH_SLOTS_16 - 1];	/* 192 bytes (8..199) */
	u16 child_idx[BONSAI_BRANCH_SLOTS_16];			/* 50 bytes (200..249) */
	u8 _pad[6];						/* 6 bytes (250..255) */
};

/*
 * Compact 256-byte leaf node (4 cachelines).
 * Holds 15 range pivots and 16 payload pointers.
 */
struct bonsai_leaf {
	struct bonsai_meta meta;				/* 8 bytes (0..7) */
	unsigned long pivot[BONSAI_LEAF_SLOTS - 1];		/* 120 bytes (8..127) */
	void *slot[BONSAI_LEAF_SLOTS];				/* 128 bytes (128..255) */
};

union bonsai_node {
	struct bonsai_meta m;
	struct bonsai_branch_8 b8;
	struct bonsai_branch_16 b16;
	struct bonsai_leaf l;
	u8 raw[BONSAI_NODE_SIZE];
};

#if defined(CONFIG_64BIT) && !defined(BUILD_VDSO32_64)
static_assert(sizeof(struct bonsai_meta) == 8);
static_assert(sizeof(struct bonsai_branch_8) == BONSAI_NODE_SIZE);
static_assert(sizeof(struct bonsai_branch_16) == BONSAI_NODE_SIZE);
static_assert(sizeof(struct bonsai_leaf) == BONSAI_NODE_SIZE);
static_assert(sizeof(union bonsai_node) == BONSAI_NODE_SIZE);
static_assert(offsetof(struct bonsai_branch_8, meta) == 0);
static_assert(offsetof(struct bonsai_branch_16, meta) == 0);
static_assert(offsetof(struct bonsai_leaf, meta) == 0);
#endif

/*
 * Segmented Scratchpad Slab Configuration:
 * 64 nodes per slab = 16 KiB per slab
 * Up to 16 slabs = 1024 nodes (256 KiB capacity)
 */
#define BONSAI_NODES_PER_SLAB	64
#define BONSAI_SLAB_SHIFT	6
#define BONSAI_SLAB_MASK	0x3F
#define BONSAI_SLAB_SIZE	(BONSAI_NODES_PER_SLAB * sizeof(union bonsai_node))
#define BONSAI_MAX_NODE_SLABS	16

struct bonsai_val_slab {
	struct bonsai_val_slab *next;
	size_t size;
	size_t used;
	char data[] __aligned(8);
};

/*
 * Potted Bonsai Tree Root
 */
struct bonsai_tree {
	void *node_slabs[BONSAI_MAX_NODE_SLABS];/* Array of 16 KiB B-Tree node slabs */
	struct bonsai_val_slab *val_slabs;      /* Linked value/string slabs */
	u8 num_node_slabs;                      /* Number of active node slabs allocated */
	u8 num_val_slabs;                       /* Number of active value slabs allocated */
	u8 height;                              /* 0 = empty, 1 = root leaf, 2 = root branch + leaves */
	u16 root_idx;                           /* 1-based index to root node (0 = empty) */
	bool is_u16;                            /* False = u8 seedling (28-way), True = u16 expanded (25-way) */
	bool is_sealed;                         /* True = frozen/read-only table */
	u32 val_bytes_used;                     /* Total bytes consumed in value slabs */
	unsigned int node_count;                /* Total active nodes allocated */
	unsigned long entry_count;              /* Total range entries stored */
};

#define BONSAI_TREE_INIT { { NULL }, NULL, 0, 0, 0, 0, false, false, 0, 0, 0 }

/* Core Lifecycle API */
void bonsai_init(struct bonsai_tree *bt);
void bonsai_destroy(struct bonsai_tree *bt);
void bonsai_reset(struct bonsai_tree *bt);
void bonsai_seal(struct bonsai_tree *bt);
void *bonsai_alloc_val_space(struct bonsai_tree *bt, size_t len, gfp_t gfp);
const char *bonsai_copy_string_pool(struct bonsai_tree *bt, const char *src, size_t len, gfp_t gfp);

/* Query & Mutation API */
void *bonsai_lookup(const struct bonsai_tree *bt, unsigned long index);
int bonsai_store_range(struct bonsai_tree *bt, unsigned long first,
		       unsigned long last, void *val, gfp_t gfp);
int bonsai_store(struct bonsai_tree *bt, unsigned long index, void *val, gfp_t gfp);
int bonsai_repot(struct bonsai_tree *bt, unsigned int new_order, gfp_t gfp);

/* Maple Tree Graduation & Potting Bridge */
struct maple_tree;
int bonsai_to_maple(const struct bonsai_tree *bt, struct maple_tree *mt, gfp_t gfp);
int maple_to_bonsai(struct maple_tree *mt, struct bonsai_tree *bt, gfp_t gfp);

/* Inline Index Conversion Helpers */
static inline union bonsai_node *bonsai_node_at(const struct bonsai_tree *bt, u16 idx)
{
	unsigned int slab, offset;

	if (!idx || !bt)
		return NULL;

	slab = (idx - 1) >> BONSAI_SLAB_SHIFT;
	offset = (idx - 1) & BONSAI_SLAB_MASK;

	if (slab >= bt->num_node_slabs || !bt->node_slabs[slab])
		return NULL;

	return (union bonsai_node *)((char *)bt->node_slabs[slab] + offset * BONSAI_NODE_SIZE);
}

static inline u16 bonsai_node_idx(const struct bonsai_tree *bt, const union bonsai_node *node)
{
	unsigned int slab;

	if (!node || !bt)
		return 0;

	for (slab = 0; slab < bt->num_node_slabs; slab++) {
		const char *base = (const char *)bt->node_slabs[slab];
		const char *ptr = (const char *)node;
		if (ptr >= base && ptr < base + BONSAI_SLAB_SIZE) {
			unsigned int offset = (ptr - base) / BONSAI_NODE_SIZE;
			return (u16)((slab << BONSAI_SLAB_SHIFT) + offset + 1);
		}
	}
	return 0;
}

#endif /* _LINUX_BONSAI_TREE_H */
