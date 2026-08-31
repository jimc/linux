// SPDX-License-Identifier: GPL-2.0+
/*
 * Compact Potted Bonsai Range Tree
 *
 * A specialized relative-indexed range tree designed for potted
 * scratchpad allocations with dynamic u8 (28-way) -> u16 (25-way)
 * fanout and O(1) bulk teardown.
 */

#include <linux/bonsai_tree.h>
#include <linux/maple_tree.h>
#include <linux/slab.h>
#include <linux/export.h>

static union bonsai_node *bonsai_alloc_node(struct bonsai_tree *bt,
					    enum bonsai_node_type type,
					    gfp_t gfp)
{
	unsigned int slab = bt->node_count >> BONSAI_SLAB_SHIFT;
	unsigned int offset = bt->node_count & BONSAI_SLAB_MASK;
	union bonsai_node *node;

	if (slab >= bt->num_node_slabs) {
		if (slab >= BONSAI_MAX_NODE_SLABS)
			return NULL;
		bt->node_slabs[slab] = kmalloc(BONSAI_SLAB_SIZE, gfp);
		if (!bt->node_slabs[slab])
			return NULL;
		bt->num_node_slabs = slab + 1;
		if (bt->num_node_slabs > 4)
			bt->is_u16 = true;
	}

	node = (union bonsai_node *)((char *)bt->node_slabs[slab] + offset * BONSAI_NODE_SIZE);
	memset(node, 0, sizeof(*node));
	node->m.node_type = type;
	bt->node_count++;
	return node;
}

void bonsai_init(struct bonsai_tree *bt)
{
	struct bonsai_val_slab *vslab;
	int i;

	if (!bt)
		return;

	for (i = 0; i < bt->num_node_slabs; i++) {
		kfree(bt->node_slabs[i]);
		bt->node_slabs[i] = NULL;
	}

	vslab = bt->val_slabs;
	while (vslab) {
		struct bonsai_val_slab *next = vslab->next;
		kfree(vslab);
		vslab = next;
	}

	bt->val_slabs = NULL;
	bt->num_node_slabs = 0;
	bt->num_val_slabs = 0;
	bt->val_bytes_used = 0;
	bt->root_idx = 0;
	bt->height = 0;
	bt->node_count = 0;
	bt->entry_count = 0;
	bt->is_u16 = false;
	bt->is_sealed = false;
}
EXPORT_SYMBOL_GPL(bonsai_init);

void bonsai_destroy(struct bonsai_tree *bt)
{
	struct bonsai_val_slab *vslab;
	int i;

	if (!bt)
		return;

	for (i = 0; i < bt->num_node_slabs; i++) {
		kfree(bt->node_slabs[i]);
		bt->node_slabs[i] = NULL;
	}

	vslab = bt->val_slabs;
	while (vslab) {
		struct bonsai_val_slab *next = vslab->next;
		kfree(vslab);
		vslab = next;
	}

	bt->val_slabs = NULL;
	bt->num_node_slabs = 0;
	bt->num_val_slabs = 0;
	bt->val_bytes_used = 0;
	bt->root_idx = 0;
	bt->height = 0;
	bt->node_count = 0;
	bt->entry_count = 0;
	bt->is_u16 = false;
	bt->is_sealed = false;
}
EXPORT_SYMBOL_GPL(bonsai_destroy);

void *bonsai_alloc_val_space(struct bonsai_tree *bt, size_t len, gfp_t gfp)
{
	struct bonsai_val_slab *slab;
	void *buf;

	if (!bt || !len)
		return NULL;

	len = ALIGN(len, 8);

	slab = bt->val_slabs;
	if (!slab || slab->used + len > slab->size) {
		size_t slab_cap = max_t(size_t, BONSAI_SLAB_SIZE, len);
		struct bonsai_val_slab *new_slab;

		new_slab = kmalloc(sizeof(*new_slab) + slab_cap, gfp);
		if (!new_slab)
			return NULL;

		new_slab->size = slab_cap;
		new_slab->used = 0;
		new_slab->next = bt->val_slabs;
		bt->val_slabs = new_slab;
		bt->num_val_slabs++;
		slab = new_slab;
	}

	buf = slab->data + slab->used;
	slab->used += len;
	bt->val_bytes_used += len;
	return buf;
}
EXPORT_SYMBOL_GPL(bonsai_alloc_val_space);

const char *bonsai_copy_string_pool(struct bonsai_tree *bt, const char *src, size_t len, gfp_t gfp)
{
	char *dst = bonsai_alloc_val_space(bt, len, gfp);

	if (!dst)
		return NULL;
	memcpy(dst, src, len);
	return dst;
}
EXPORT_SYMBOL_GPL(bonsai_copy_string_pool);

void bonsai_seal(struct bonsai_tree *bt)
{
	if (bt)
		bt->is_sealed = true;
}
EXPORT_SYMBOL_GPL(bonsai_seal);

void bonsai_reset(struct bonsai_tree *bt)
{
	if (!bt)
		return;

	bt->root_idx = 0;
	bt->height = 0;
	bt->node_count = 0;
	bt->entry_count = 0;
	bt->is_sealed = false;
}
EXPORT_SYMBOL_GPL(bonsai_reset);

int bonsai_repot(struct bonsai_tree *bt, unsigned int new_order, gfp_t gfp)
{
	/* Dynamic segmented allocator handles growth automatically */
	return 0;
}
EXPORT_SYMBOL_GPL(bonsai_repot);

static inline int leaf_find_slot(const struct bonsai_leaf *leaf, unsigned long index)
{
	int i, count = leaf->meta.num_pivots;

	for (i = 0; i < count; i++) {
		if (index <= leaf->pivot[i])
			return i;
	}
	return count;
}

static inline int branch_8_find_child(const struct bonsai_branch_8 *br, unsigned long index)
{
	int i, count = br->meta.num_pivots;

	for (i = 0; i < count; i++) {
		if (index <= br->pivot[i])
			return i;
	}
	return count;
}

static inline int branch_16_find_child(const struct bonsai_branch_16 *br, unsigned long index)
{
	int i, count = br->meta.num_pivots;

	for (i = 0; i < count; i++) {
		if (index <= br->pivot[i])
			return i;
	}
	return count;
}

void *bonsai_lookup(const struct bonsai_tree *bt, unsigned long index)
{
	union bonsai_node *curr;
	u16 curr_idx;

	if (!bt || !bt->root_idx)
		return NULL;

	curr_idx = bt->root_idx;
	curr = bonsai_node_at(bt, curr_idx);
	if (!curr)
		return NULL;

	while (curr->m.node_type == BONSAI_TYPE_BRANCH_8 ||
	       curr->m.node_type == BONSAI_TYPE_BRANCH_16) {
		int slot;

		if (curr->m.node_type == BONSAI_TYPE_BRANCH_8) {
			slot = branch_8_find_child(&curr->b8, index);
			curr_idx = curr->b8.child_idx[slot];
		} else {
			slot = branch_16_find_child(&curr->b16, index);
			curr_idx = curr->b16.child_idx[slot];
		}

		if (!curr_idx)
			return NULL;
		curr = bonsai_node_at(bt, curr_idx);
		if (!curr)
			return NULL;
	}

	if (curr->m.node_type == BONSAI_TYPE_LEAF) {
		int slot = leaf_find_slot(&curr->l, index);

		if (slot < curr->l.meta.num_pivots && index <= curr->l.pivot[slot])
			return curr->l.slot[slot];
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(bonsai_lookup);

static int leaf_insert(struct bonsai_leaf *leaf, unsigned long index, void *val)
{
	int i, slot = leaf_find_slot(leaf, index);

	/* Exact match overwrite */
	if (slot < leaf->meta.num_pivots && leaf->pivot[slot] == index) {
		leaf->slot[slot] = val;
		return 0;
	}

	/* Check if leaf is full */
	if (leaf->meta.num_pivots >= BONSAI_LEAF_SLOTS - 1)
		return -ENOSPC;

	/* Shift elements right */
	for (i = leaf->meta.num_pivots; i > slot; i--) {
		leaf->pivot[i] = leaf->pivot[i - 1];
		leaf->slot[i] = leaf->slot[i - 1];
	}

	leaf->pivot[slot] = index;
	leaf->slot[slot] = val;
	leaf->meta.num_pivots++;
	return 0;
}

int bonsai_store_range(struct bonsai_tree *bt, unsigned long first,
		       unsigned long last, void *val, gfp_t gfp)
{
	u16 path_idx[8];
	int path_slot[8];
	int depth = 0;
	u16 curr_idx;
	union bonsai_node *curr, *leaf, *new_leaf;
	int ret, split_mid, i, d;
	u16 insert_child_idx;
	unsigned long insert_pivot;

	if (!bt || WARN_ON_ONCE(bt->is_sealed))
		return -EPERM;

	if (!bt->root_idx) {
		curr = bonsai_alloc_node(bt, BONSAI_TYPE_LEAF, gfp);
		if (!curr)
			return -ENOMEM;
		curr->l.pivot[0] = last;
		curr->l.slot[0] = val;
		curr->l.meta.num_pivots = 1;
		bt->root_idx = bonsai_node_idx(bt, curr);
		bt->height = 1;
		bt->entry_count = 1;
		return 0;
	}

	/* 1. Traverse down to the leaf */
	curr_idx = bt->root_idx;
	curr = bonsai_node_at(bt, curr_idx);
	if (!curr)
		return -EINVAL;

	while (curr->m.node_type == BONSAI_TYPE_BRANCH_8 ||
	       curr->m.node_type == BONSAI_TYPE_BRANCH_16) {
		int slot;

		if (curr->m.node_type == BONSAI_TYPE_BRANCH_8) {
			slot = branch_8_find_child(&curr->b8, last);
			path_idx[depth] = curr_idx;
			path_slot[depth] = slot;
			depth++;
			curr_idx = curr->b8.child_idx[slot];
		} else {
			slot = branch_16_find_child(&curr->b16, last);
			path_idx[depth] = curr_idx;
			path_slot[depth] = slot;
			depth++;
			curr_idx = curr->b16.child_idx[slot];
		}

		if (!curr_idx)
			return -EINVAL;
		curr = bonsai_node_at(bt, curr_idx);
		if (!curr)
			return -EINVAL;
	}

	leaf = curr;
	ret = leaf_insert(&leaf->l, last, val);
	if (ret == 0) {
		bt->entry_count++;
		return 0;
	}

	/* 2. Leaf is full: split leaf into 2 leaves */
	new_leaf = bonsai_alloc_node(bt, BONSAI_TYPE_LEAF, gfp);
	if (!new_leaf)
		return -ENOMEM;

	/* Re-fetch after possible repotting */
	leaf = bonsai_node_at(bt, curr_idx);
	u16 new_leaf_idx = bonsai_node_idx(bt, new_leaf);

	if (last > leaf->l.pivot[leaf->l.meta.num_pivots - 1]) {
		/* Append-optimized: keep full leaf, put new entry in new_leaf */
		new_leaf->l.pivot[0] = last;
		new_leaf->l.slot[0] = val;
		new_leaf->l.meta.num_pivots = 1;
	} else {
		/* Random insertion: 50/50 midpoint split */
		split_mid = leaf->l.meta.num_pivots / 2;
		for (i = split_mid; i < leaf->l.meta.num_pivots; i++) {
			new_leaf->l.pivot[i - split_mid] = leaf->l.pivot[i];
			new_leaf->l.slot[i - split_mid] = leaf->l.slot[i];
			leaf->l.pivot[i] = 0;
			leaf->l.slot[i] = NULL;
		}
		new_leaf->l.meta.num_pivots = leaf->l.meta.num_pivots - split_mid;
		leaf->l.meta.num_pivots = split_mid;

		if (last <= leaf->l.pivot[split_mid - 1])
			leaf_insert(&leaf->l, last, val);
		else
			leaf_insert(&new_leaf->l, last, val);
	}

	/* 3. If leaf was root (depth == 0), create a new branch root */
	if (depth == 0) {
		union bonsai_node *new_root;
		u16 new_root_idx;

		new_root = bonsai_alloc_node(bt, bt->is_u16 ? BONSAI_TYPE_BRANCH_16 : BONSAI_TYPE_BRANCH_8, gfp);
		if (!new_root)
			return -ENOMEM;

		leaf = bonsai_node_at(bt, curr_idx);
		new_leaf = bonsai_node_at(bt, new_leaf_idx);
		new_root_idx = bonsai_node_idx(bt, new_root);

		if (!bt->is_u16) {
			new_root->b8.pivot[0] = leaf->l.pivot[leaf->l.meta.num_pivots - 1];
			new_root->b8.child_idx[0] = (u8)curr_idx;
			new_root->b8.child_idx[1] = (u8)new_leaf_idx;
			new_root->b8.meta.num_pivots = 1;
		} else {
			new_root->b16.pivot[0] = leaf->l.pivot[leaf->l.meta.num_pivots - 1];
			new_root->b16.child_idx[0] = curr_idx;
			new_root->b16.child_idx[1] = new_leaf_idx;
			new_root->b16.meta.num_pivots = 1;
		}

		bt->root_idx = new_root_idx;
		bt->height = 2;
		bt->entry_count++;
		return 0;
	}

	/* 4. Propagate splits up the tree */
	insert_child_idx = new_leaf_idx;
	leaf = bonsai_node_at(bt, curr_idx);
	insert_pivot = leaf->l.pivot[leaf->l.meta.num_pivots - 1];

	for (d = depth - 1; d >= 0; d--) {
		u16 node_idx = path_idx[d];
		int slot = path_slot[d];
		union bonsai_node *node = bonsai_node_at(bt, node_idx);
		int max_slots = (node->m.node_type == BONSAI_TYPE_BRANCH_8) ?
				BONSAI_BRANCH_SLOTS_8 : BONSAI_BRANCH_SLOTS_16;

		if (node->m.num_pivots < max_slots - 1) {
			/* Node has space: insert and return */
			if (node->m.node_type == BONSAI_TYPE_BRANCH_8) {
				for (i = node->b8.meta.num_pivots; i > slot; i--) {
					node->b8.pivot[i] = node->b8.pivot[i - 1];
					node->b8.child_idx[i + 1] = node->b8.child_idx[i];
				}
				node->b8.pivot[slot] = insert_pivot;
				node->b8.child_idx[slot + 1] = (u8)insert_child_idx;
				node->b8.meta.num_pivots++;
			} else {
				for (i = node->b16.meta.num_pivots; i > slot; i--) {
					node->b16.pivot[i] = node->b16.pivot[i - 1];
					node->b16.child_idx[i + 1] = node->b16.child_idx[i];
				}
				node->b16.pivot[slot] = insert_pivot;
				node->b16.child_idx[slot + 1] = insert_child_idx;
				node->b16.meta.num_pivots++;
			}
			bt->entry_count++;
			return 0;
		}

		/* Node is full: split node into node and new_branch */
		union bonsai_node *new_branch;
		u16 new_branch_idx;
		int old_pivots = node->m.num_pivots;
		int mid = old_pivots / 2;
		unsigned long sep_pivot;

		new_branch = bonsai_alloc_node(bt, bt->is_u16 ? BONSAI_TYPE_BRANCH_16 : BONSAI_TYPE_BRANCH_8, gfp);
		if (!new_branch)
			return -ENOMEM;

		node = bonsai_node_at(bt, node_idx);
		new_branch_idx = bonsai_node_idx(bt, new_branch);

		if (!bt->is_u16) {
			if (slot == old_pivots) {
				/* Append-optimized branch split */
				sep_pivot = insert_pivot;
				new_branch->b8.child_idx[0] = (u8)insert_child_idx;
				new_branch->b8.meta.num_pivots = 0;
			} else {
				sep_pivot = node->b8.pivot[mid];
				for (i = mid + 1; i < old_pivots; i++) {
					new_branch->b8.pivot[i - (mid + 1)] = node->b8.pivot[i];
					node->b8.pivot[i] = 0;
				}
				for (i = mid + 1; i <= old_pivots; i++) {
					new_branch->b8.child_idx[i - (mid + 1)] = node->b8.child_idx[i];
					node->b8.child_idx[i] = 0;
				}
				node->b8.pivot[mid] = 0;
				new_branch->b8.meta.num_pivots = old_pivots - 1 - mid;
				node->b8.meta.num_pivots = mid;

				if (slot <= mid) {
					for (i = node->b8.meta.num_pivots; i > slot; i--) {
						node->b8.pivot[i] = node->b8.pivot[i - 1];
						node->b8.child_idx[i + 1] = node->b8.child_idx[i];
					}
					node->b8.pivot[slot] = insert_pivot;
					node->b8.child_idx[slot + 1] = (u8)insert_child_idx;
					node->b8.meta.num_pivots++;
				} else {
					int b_slot = slot - (mid + 1);
					for (i = new_branch->b8.meta.num_pivots; i > b_slot; i--) {
						new_branch->b8.pivot[i] = new_branch->b8.pivot[i - 1];
						new_branch->b8.child_idx[i + 1] = new_branch->b8.child_idx[i];
					}
					new_branch->b8.pivot[b_slot] = insert_pivot;
					new_branch->b8.child_idx[b_slot + 1] = (u8)insert_child_idx;
					new_branch->b8.meta.num_pivots++;
				}
			}
		} else {
			if (slot == old_pivots) {
				/* Append-optimized branch split */
				sep_pivot = insert_pivot;
				new_branch->b16.child_idx[0] = insert_child_idx;
				new_branch->b16.meta.num_pivots = 0;
			} else {
				sep_pivot = node->b16.pivot[mid];
				for (i = mid + 1; i < old_pivots; i++) {
					new_branch->b16.pivot[i - (mid + 1)] = node->b16.pivot[i];
					node->b16.pivot[i] = 0;
				}
				for (i = mid + 1; i <= old_pivots; i++) {
					new_branch->b16.child_idx[i - (mid + 1)] = node->b16.child_idx[i];
					node->b16.child_idx[i] = 0;
				}
				node->b16.pivot[mid] = 0;
				new_branch->b16.meta.num_pivots = old_pivots - 1 - mid;
				node->b16.meta.num_pivots = mid;

				if (slot <= mid) {
					for (i = node->b16.meta.num_pivots; i > slot; i--) {
						node->b16.pivot[i] = node->b16.pivot[i - 1];
						node->b16.child_idx[i + 1] = node->b16.child_idx[i];
					}
					node->b16.pivot[slot] = insert_pivot;
					node->b16.child_idx[slot + 1] = insert_child_idx;
					node->b16.meta.num_pivots++;
				} else {
					int b_slot = slot - (mid + 1);
					for (i = new_branch->b16.meta.num_pivots; i > b_slot; i--) {
						new_branch->b16.pivot[i] = new_branch->b16.pivot[i - 1];
						new_branch->b16.child_idx[i + 1] = new_branch->b16.child_idx[i];
					}
					new_branch->b16.pivot[b_slot] = insert_pivot;
					new_branch->b16.child_idx[b_slot + 1] = insert_child_idx;
					new_branch->b16.meta.num_pivots++;
				}
			}
		}

		insert_child_idx = new_branch_idx;
		insert_pivot = sep_pivot;
	}

	/* Split reached above the root: allocate new root */
	union bonsai_node *new_root;
	u16 new_root_idx;

	new_root = bonsai_alloc_node(bt, bt->is_u16 ? BONSAI_TYPE_BRANCH_16 : BONSAI_TYPE_BRANCH_8, gfp);
	if (!new_root)
		return -ENOMEM;

	new_root_idx = bonsai_node_idx(bt, new_root);
	if (!bt->is_u16) {
		new_root->b8.pivot[0] = insert_pivot;
		new_root->b8.child_idx[0] = (u8)bt->root_idx;
		new_root->b8.child_idx[1] = (u8)insert_child_idx;
		new_root->b8.meta.num_pivots = 1;
	} else {
		new_root->b16.pivot[0] = insert_pivot;
		new_root->b16.child_idx[0] = bt->root_idx;
		new_root->b16.child_idx[1] = insert_child_idx;
		new_root->b16.meta.num_pivots = 1;
	}

	bt->root_idx = new_root_idx;
	bt->height++;
	bt->entry_count++;
	return 0;
}
EXPORT_SYMBOL_GPL(bonsai_store_range);

int bonsai_store(struct bonsai_tree *bt, unsigned long index, void *val, gfp_t gfp)
{
	return bonsai_store_range(bt, index, index, val, gfp);
}
EXPORT_SYMBOL_GPL(bonsai_store);

int bonsai_to_maple(const struct bonsai_tree *bt, struct maple_tree *mt, gfp_t gfp)
{
	unsigned int idx;

	if (!bt || !bt->num_node_slabs || !mt)
		return -EINVAL;

	for (idx = 1; idx <= bt->node_count; idx++) {
		union bonsai_node *n = bonsai_node_at(bt, idx);

		if (n && n->m.node_type == BONSAI_TYPE_LEAF) {
			int i, count = n->l.meta.num_pivots;

			for (i = 0; i < count; i++) {
				unsigned long key = n->l.pivot[i];
				void *val = n->l.slot[i];

				if (val)
					mtree_store_range(mt, key, key, val, gfp);
			}
		}
	}
	return 0;
}
EXPORT_SYMBOL_GPL(bonsai_to_maple);

int maple_to_bonsai(struct maple_tree *mt, struct bonsai_tree *bt, gfp_t gfp)
{
	MA_STATE(mas, mt, 0, 0);
	void *entry;
	int ret;

	bonsai_init(bt);

	mas_lock(&mas);
	mas_for_each(&mas, entry, ULONG_MAX) {
		ret = bonsai_store_range(bt, mas.index, mas.last, entry, gfp);
		if (ret) {
			mas_unlock(&mas);
			bonsai_destroy(bt);
			return ret;
		}
	}
	mas_unlock(&mas);
	return 0;
}
EXPORT_SYMBOL_GPL(maple_to_bonsai);
