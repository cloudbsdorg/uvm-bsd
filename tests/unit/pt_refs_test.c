/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * pt_refs_test.c - the page-tree reference rule (uvm_pt_refs.h): a model
 * tree that takes references the way uvm_page_tree_get_ptes does and drops
 * them with the rule the module's uvm_pt_put uses.  After every PTE range
 * is put back the root must be down to the tree's own reference; the
 * module logged "page tree destroyed with 2 references on the root" when
 * a freed root child left its reference on the root.
 */

#include <stdio.h>
#include <stdlib.h>

#include "uvm_pt_refs.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

#define	DEPTH	4

struct node {
	unsigned refs;
	struct node *parent;
	struct node *child[2];
};

static struct node *root;

/* uvm_page_tree_get_ptes: reference the path, create tables, hand out a PTE range reference on the leaf */
static struct node *
get(int path)
{
	struct node *t = root, *c;
	int d;

	t->refs++;
	for (d = 0; d < DEPTH; d++) {
		int idx = (path >> (DEPTH - 1 - d)) & 1;

		c = t->child[idx];
		if (c == NULL) {
			c = calloc(1, sizeof(*c));
			c->parent = t;
			t->child[idx] = c;
			t->refs++;		/* the child's existence */
		}
		c->refs++;
		t->refs--;			/* carried by the child now */
		t = c;
	}
	return (t);
}

static int freed;

/* uvm_pt_put */
static void
put(struct node *t)
{
	while (t != NULL) {
		struct node *parent;

		if (!uvm_pt_ref_release(&t->refs, t == root))
			return;
		parent = t->parent;
		parent->child[parent->child[0] == t ? 0 : 1] = NULL;
		free(t);
		freed++;
		t = parent;
	}
}

int
main(void)
{
	struct node *a, *b, *c;

	root = calloc(1, sizeof(*root));
	root->refs = 1;

	a = get(0x0);
	b = get(0x1);	/* same root child as a */
	CHECK(root->refs == 2, "one live root child: root refs %u, want 2", root->refs);
	c = get(0x8);	/* a second root child */
	CHECK(root->refs == 3, "two live root children: root refs %u, want 3", root->refs);
	put(a);
	CHECK(root->refs == 3, "a still shares its root child: root refs %u, want 3", root->refs);
	put(b);
	CHECK(root->refs == 2, "first root child freed: root refs %u, want 2", root->refs);
	put(c);
	CHECK(root->refs == 1, "all ranges put back: root refs %u, want the tree's own 1", root->refs);
	CHECK(root->child[0] == NULL && root->child[1] == NULL, "no table left under the root");
	CHECK(freed == 9, "freed %d tables, want 9", freed);

	/* the error path of get_ptes puts the root itself: it counts down and is never freed */
	root->refs++;
	put(root);
	CHECK(root->refs == 1, "root put on the error path: refs %u, want 1", root->refs);

	/* the root is never freed even by a stray last drop */
	CHECK(!uvm_pt_ref_release(&(unsigned){1}, true), "root with no references left is still not freed");

	if (fails == 0)
		printf("pt_refs_test: all passed\n");
	return (fails != 0);
}
