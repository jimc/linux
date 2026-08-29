// SPDX-License-Identifier: GPL-2.0+
/*
 * Compact Potted Bonsai Range Tree
 *
 * A specialized 16-bit relative indexed range tree designed for potted
 * scratchpad allocations with 25-way branch fanout and O(1) bulk teardown.
 */

#include <linux/bonsai_tree.h>
#include <linux/slab.h>
#include <linux/export.h>

static union bonsai_node *bonsai_alloc_node(struct bonsai_tree *bt,
					    enum bonsai_node_type type,
					    gfp_t gfp)
{
	union bonsai_node *node;

	if (!bt->pot) {
		if (bonsai_init(bt, bt->pot_order ?: 0, gfp))
			return NULL;
	}

	node = scratchpad_alloc_align(bt->pot, sizeof(union bonsai_node),
				      BONSAI_NODE_SIZE, gfp);
	if (!node) {
		/* Pot full: trigger automatic repotting to double capacity */
		if (bonsai_repot(bt, bt->pot_order + 1, gfp))
			return NULL;
		node = scratchpad_alloc_align(bt->pot, sizeof(union bonsai_node),
					      BONSAI_NODE_SIZE, gfp);
		if (!node)
			return NULL;
	}

	memset(node, 0, sizeof(*node));
	node->m.node_type = type;
	bt->node_count++;
	return node;
}

int bonsai_init(struct bonsai_tree *bt, unsigned int initial_order, gfp_t gfp)
{
	bt->pot = kzalloc_obj(struct scratchpad, gfp);
	if (!bt->pot)
		return -ENOMEM;

	_scratchpad_init(bt->pot, sizeof(union bonsai_node), BONSAI_NODE_SIZE,
			 initial_order, NULL, gfp);
	bt->root_idx = 0;
	bt->height = 0;
	bt->pot_order = initial_order;
	bt->node_count = 0;
	bt->entry_count = 0;
	return 0;
}
EXPORT_SYMBOL_GPL(bonsai_init);

void bonsai_destroy(struct bonsai_tree *bt)
{
	if (!bt || !bt->pot)
		return;

	scratchpad_free(bt->pot);
	kfree(bt->pot);
	bt->pot = NULL;
	bt->root_idx = 0;
	bt->height = 0;
	bt->node_count = 0;
	bt->entry_count = 0;
}
EXPORT_SYMBOL_GPL(bonsai_destroy);

void bonsai_reset(struct bonsai_tree *bt)
{
	if (!bt || !bt->pot)
		return;

	scratchpad_reset(bt->pot);
	bt->root_idx = 0;
	bt->height = 0;
	bt->node_count = 0;
	bt->entry_count = 0;
}
EXPORT_SYMBOL_GPL(bonsai_reset);

int bonsai_repot(struct bonsai_tree *bt, unsigned int new_order, gfp_t gfp)
{
	struct scratchpad *new_pot;
	size_t used_bytes;
	void *old_base, *new_base;

	if (!bt->pot)
		return bonsai_init(bt, new_order, gfp);

	new_pot = kzalloc_obj(struct scratchpad, gfp);
	if (!new_pot)
		return -ENOMEM;

	_scratchpad_init(new_pot, sizeof(union bonsai_node), BONSAI_NODE_SIZE,
			 new_order, NULL, gfp);

	old_base = scratchpad_base(bt->pot);
	new_base = scratchpad_base(new_pot);

	if (bt->node_count > 0 && old_base && new_base) {
		used_bytes = bt->node_count * sizeof(union bonsai_node);
		memcpy(new_base, old_base, used_bytes);
		new_pot->free_ptr = (char *)new_base + used_bytes;
		new_pot->remaining -= used_bytes;
	}

	scratchpad_free(bt->pot);
	kfree(bt->pot);

	bt->pot = new_pot;
	bt->pot_order = new_order;
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

static inline int branch_find_child(const struct bonsai_branch *br, unsigned long index)
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

	while (curr->m.node_type == BONSAI_TYPE_BRANCH) {
		int slot = branch_find_child(&curr->b, index);

		curr_idx = curr->b.child_idx[slot];
		if (!curr_idx)
			return NULL;
		curr = bonsai_node_at(bt, curr_idx);
		if (!curr)
			return NULL;
	}

	if (curr->m.node_type == BONSAI_TYPE_LEAF) {
		int slot = leaf_find_slot(&curr->l, index);

		if (slot < curr->l.meta.num_pivots && curr->l.pivot[slot] == index)
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

int bonsai_store(struct bonsai_tree *bt, unsigned long index, void *val, gfp_t gfp)
{
	return bonsai_store_range(bt, index, index, val, gfp);
}
EXPORT_SYMBOL_GPL(bonsai_store);

int bonsai_store_range(struct bonsai_tree *bt, unsigned long first,
		       unsigned long last, void *val, gfp_t gfp)
{
	union bonsai_node *root, *leaf, *new_leaf, *new_root;
	u16 leaf_idx, new_leaf_idx, new_root_idx;
	int ret, split_mid, i;

	if (!bt->root_idx) {
		root = bonsai_alloc_node(bt, BONSAI_TYPE_LEAF, gfp);
		if (!root)
			return -ENOMEM;
		root->l.pivot[0] = last;
		root->l.slot[0] = val;
		root->l.meta.num_pivots = 1;
		bt->root_idx = bonsai_node_idx(bt, root);
		bt->height = 1;
		bt->entry_count = 1;
		return 0;
	}

	root = bonsai_node_at(bt, bt->root_idx);
	if (!root)
		return -EINVAL;

	/* Height 1 simple leaf insertion */
	if (bt->height == 1) {
		ret = leaf_insert(&root->l, last, val);
		if (ret == 0) {
			bt->entry_count++;
			return 0;
		}

		/* Leaf is full: split root into 2 leaves and create branch root */
		new_leaf = bonsai_alloc_node(bt, BONSAI_TYPE_LEAF, gfp);
		if (!new_leaf)
			return -ENOMEM;

		/* Re-fetch root in case repotting relocated the buffer */
		root = bonsai_node_at(bt, bt->root_idx);
		leaf_idx = bt->root_idx;
		new_leaf_idx = bonsai_node_idx(bt, new_leaf);

		split_mid = root->l.meta.num_pivots / 2;
		for (i = split_mid; i < root->l.meta.num_pivots; i++) {
			new_leaf->l.pivot[i - split_mid] = root->l.pivot[i];
			new_leaf->l.slot[i - split_mid] = root->l.slot[i];
			root->l.pivot[i] = 0;
			root->l.slot[i] = NULL;
		}
		new_leaf->l.meta.num_pivots = root->l.meta.num_pivots - split_mid;
		root->l.meta.num_pivots = split_mid;

		/* Insert new entry into the appropriate half */
		if (last <= root->l.pivot[split_mid - 1])
			leaf_insert(&root->l, last, val);
		else
			leaf_insert(&new_leaf->l, last, val);

		/* Allocate new branch root */
		new_root = bonsai_alloc_node(bt, BONSAI_TYPE_BRANCH, gfp);
		if (!new_root)
			return -ENOMEM;

		/* Re-fetch after possible repotting */
		root = bonsai_node_at(bt, leaf_idx);
		new_leaf = bonsai_node_at(bt, new_leaf_idx);
		new_root_idx = bonsai_node_idx(bt, new_root);

		new_root->b.pivot[0] = root->l.pivot[root->l.meta.num_pivots - 1];
		new_root->b.child_idx[0] = leaf_idx;
		new_root->b.child_idx[1] = new_leaf_idx;
		new_root->b.meta.num_pivots = 1;

		root->l.meta.parent_idx = new_root_idx;
		root->l.meta.parent_slot = 0;
		new_leaf->l.meta.parent_idx = new_root_idx;
		new_leaf->l.meta.parent_slot = 1;

		bt->root_idx = new_root_idx;
		bt->height = 2;
		bt->entry_count++;
		return 0;
	}

	/* Height 2 branch -> leaf insertion */
	if (bt->height == 2) {
		int child_slot = branch_find_child(&root->b, last);
		leaf_idx = root->b.child_idx[child_slot];
		leaf = bonsai_node_at(bt, leaf_idx);
		if (!leaf)
			return -EINVAL;

		ret = leaf_insert(&leaf->l, last, val);
		if (ret == 0) {
			bt->entry_count++;
			return 0;
		}

		/* Leaf full: split leaf under branch root */
		if (root->b.meta.num_pivots < BONSAI_BRANCH_SLOTS - 1) {
			new_leaf = bonsai_alloc_node(bt, BONSAI_TYPE_LEAF, gfp);
			if (!new_leaf)
				return -ENOMEM;

			/* Re-fetch after possible repotting */
			root = bonsai_node_at(bt, bt->root_idx);
			leaf = bonsai_node_at(bt, leaf_idx);
			new_leaf_idx = bonsai_node_idx(bt, new_leaf);

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

			/* Insert new child into branch root */
			for (i = root->b.meta.num_pivots; i > child_slot; i--) {
				root->b.pivot[i] = root->b.pivot[i - 1];
				root->b.child_idx[i + 1] = root->b.child_idx[i];
			}

			root->b.pivot[child_slot] = leaf->l.pivot[leaf->l.meta.num_pivots - 1];
			root->b.child_idx[child_slot + 1] = new_leaf_idx;
			root->b.meta.num_pivots++;

			new_leaf->l.meta.parent_idx = bt->root_idx;
			new_leaf->l.meta.parent_slot = child_slot + 1;

			bt->entry_count++;
			return 0;
		}
	}

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(bonsai_store_range);
