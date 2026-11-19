/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_page_tree.c - GPU page trees: directories, PTE ranges, and the
 * copy-engine writes that fill them.  uvm-spec r1, 05-page-tables.md
 * section 2 (page tree object), with the entry encoders of uvm_pte.h.
 *
 * Phase-1 shape: every table lives in system memory (the spec's
 * `uvm_page_table_location` fallback), allocated through the GPU's DMA tag
 * so the entries hold bus addresses, and every table is written by the copy
 * engine, never by the CPU: a new table is zeroed by a CE memset, PTEs
 * arrive as inline pushbuffer data copied to the table, and each write ends
 * with a system membar and a TLB invalidate (04 section 1.5).  Tables are
 * one 4K DMA page each, whatever their size (a 64K-page PTE table is 256
 * bytes; the rest of its page is unused), which keeps one allocation shape.
 *
 * Reference counts: a table holds one reference per child table and per
 * PTE range handed out; when the last goes, the parent's entry is written
 * invalid, the TLB is invalidated at the parent's level, and the memory is
 * freed after that push completes.
 */

#include "uvm.h"

static inline uvm_aperture_t
uvm_pt_aperture(void)
{
	return (UVM_APERTURE_SYS);
}

/* the GPU-visible address of a table's memory */
static inline uint64_t
uvm_pt_gpu_addr(const struct uvm_page_tree *tree, const struct uvm_page_table *t)
{
	return (uvm_gpu_dma_address(tree->gpu, t->bus));
}

uint64_t
uvm_page_tree_invalid_pte(const struct uvm_page_tree *tree, uint64_t page_size)
{
	if (page_size == UVM_PAGE_SIZE_64K && tree->mmu_version == 2)
		return (uvm_pte2_pte_invalid());
	return (0);
}

uint64_t
uvm_page_tree_sparse_pte(const struct uvm_page_tree *tree)
{
	return (tree->mmu_version == 3 ? uvm_pte3_pte_sparse() : uvm_pte2_pte_sparse());
}

uint64_t
uvm_page_tree_sked_pte(const struct uvm_page_tree *tree)
{
	return (tree->mmu_version == 3 ? uvm_pte3_pte_sked() : uvm_pte2_pte_sked());
}

uint64_t
uvm_page_tree_pte(const struct uvm_page_tree *tree, uint64_t addr, uvm_aperture_t ap, uvm_prot_t prot,
    bool uncached)
{
	if (tree->mmu_version == 3)
		return (uvm_pte3_pte(addr, ap, prot, uncached, 0));
	return (uvm_pte2_pte(addr, ap, prot, uncached));
}

struct uvm_pdb
uvm_page_tree_pdb(const struct uvm_page_tree *tree)
{
	struct uvm_pdb pdb;

	pdb.phys = uvm_pt_gpu_addr(tree, tree->root);
	pdb.vidmem = 0;
	return (pdb);
}

/* the TLB invalidate that follows a table write: the level, the span, the membar */
struct uvm_pt_inval {
	uint32_t	depth;
	uint64_t	va;
	uint64_t	size;		/* 0: everything below the root */
	uvm_membar_t	membar;
};

/*
 * uvm_pt_write - copy `bytes` of entry data to offset `off` of a table
 * through the copy engine (inline pushbuffer data, physical system-memory
 * destination), then the TLB invalidate `inv` describes.  Adds the push to
 * the tree's tracker and the caller's.
 */
static NV_STATUS
uvm_pt_write(struct uvm_page_tree *tree, struct uvm_page_table *t, uint32_t off, const void *data,
    size_t bytes, const struct uvm_pt_inval *inv, struct uvm_tracker *tracker)
{
	uint32_t inval_depth = inv->depth;
	uint64_t va = inv->va, va_size = inv->size;
	uvm_membar_t membar = inv->membar;

	struct uvm_push push;
	struct uvm_gpu_address dst, src;
	struct uvm_pdb pdb = uvm_page_tree_pdb(tree);
	NV_STATUS status;
	uint32_t *w;
	uint64_t src_va;

	KASSERT(bytes <= UVM_PUSH_INLINE_MAX_WORDS * 4, ("page table write too large"));
	status = uvm_push_begin(tree->gpu, &push, &tree->tracker);
	if (status != NV_OK)
		return (status);
	uvm_push_inline_data(&push, data, bytes, &src_va);
	src.address = src_va;
	src.is_virtual = 1;
	src.aperture = UVM_APERTURE_SYS;
	dst.address = uvm_pt_gpu_addr(tree, t) + off;
	dst.is_virtual = 0;
	dst.aperture = uvm_pt_aperture();
	w = uvm_push_space(&push, uvm_ce_memcopy_words(bytes));
	push.next = w + uvm_ce_memcopy(w, &dst, &src, bytes, UVM_MEMBAR_SYS, 0);
	/* the MMU reads page tables through L2; the CE wrote around it: WFI + membar before the invalidate */
	UVM_PUSH(&push, uvm_host_wfi);
	UVM_PUSH(&push, uvm_host_membar, UVM_MEMBAR_SYS);
	if (va_size == 0)
		UVM_PUSH(&push, uvm_host_tlb_invalidate_all, &pdb, inval_depth, membar, tree->mmu_version);
	else
		UVM_PUSH(&push, uvm_host_tlb_invalidate_va, &pdb, inval_depth, va, va_size, membar,
		    tree->mmu_version);
	uvm_push_end(&push);
	uvm_tracker_add(&tree->tracker, push.channel, push.value);
	if (tracker != NULL)
		uvm_tracker_add(tracker, push.channel, push.value);
	return (NV_OK);
}

/* zero a new table with the copy engine so no stale L2 line can describe it */
static NV_STATUS
uvm_pt_zero(struct uvm_page_tree *tree, struct uvm_page_table *t)
{
	struct uvm_push push;
	struct uvm_gpu_address dst;
	NV_STATUS status;
	uint32_t *w;

	status = uvm_push_begin(tree->gpu, &push, &tree->tracker);
	if (status != NV_OK)
		return (status);
	dst.address = uvm_pt_gpu_addr(tree, t);
	dst.is_virtual = 0;
	dst.aperture = uvm_pt_aperture();
	w = uvm_push_space(&push, UVM_CE_MEMSET_MAX_WORDS(PAGE_SIZE));
	push.next = w + uvm_ce_memset_8(w, &dst, 0, PAGE_SIZE, UVM_MEMBAR_SYS);
	UVM_PUSH(&push, uvm_host_wfi);
	UVM_PUSH(&push, uvm_host_membar, UVM_MEMBAR_SYS);
	uvm_push_end(&push);
	uvm_tracker_add(&tree->tracker, push.channel, push.value);
	return (NV_OK);
}

static struct uvm_page_table *
uvm_pt_alloc(struct uvm_page_tree *tree, uint32_t depth, uint64_t page_size, struct uvm_page_table *parent,
    uint32_t parent_index, int half, uint64_t va_base)
{
	struct uvm_page_table *t;
	NV_STATUS status;
	unsigned nchildren;

	t = malloc(sizeof(*t), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	t->depth = depth;
	t->page_size = page_size;
	t->entries = uvm_mmu_entries(tree->mmu_version, depth, page_size);
	t->bytes = uvm_mmu_table_bytes(tree->mmu_version, depth, page_size);
	t->parent = parent;
	t->parent_index = parent_index;
	t->parent_half = half;
	t->va_base = va_base;
	if (depth < uvm_mmu_pte_depth(tree->mmu_version, UVM_PAGE_SIZE_4K)) {
		nchildren = t->entries * (depth == uvm_mmu_dual_depth(tree->mmu_version) ? 2 : 1);
		t->children = malloc(sizeof(*t->children) * nchildren, M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	}
	status = uvm_gpu_sysmem_alloc(tree->gpu, PAGE_SIZE, &t->kva, &t->map, &t->bus);
	if (status != NV_OK) {
		free(t->children, M_NVIDIA_UVM);
		free(t, M_NVIDIA_UVM);
		return (NULL);
	}
	if (uvm_pt_zero(tree, t) != NV_OK) {
		uvm_gpu_sysmem_free(tree->gpu, t->kva, t->map, PAGE_SIZE);
		free(t->children, M_NVIDIA_UVM);
		free(t, M_NVIDIA_UVM);
		return (NULL);
	}
	return (t);
}

static void
uvm_pt_free_memory(struct uvm_page_tree *tree, struct uvm_page_table *t)
{
	uvm_gpu_sysmem_free(tree->gpu, t->kva, t->map, PAGE_SIZE);
	free(t->children, M_NVIDIA_UVM);
	free(t, M_NVIDIA_UVM);
}

/*
 * uvm_pt_write_pde - write the parent's entry for `child` (or an invalid
 * entry when child is NULL): a single 8-byte PDE, or the 16-byte dual PDE
 * whose two halves come from the parent's children array.
 */
static NV_STATUS
uvm_pt_write_pde(struct uvm_page_tree *tree, struct uvm_page_table *parent, uint32_t index, uvm_membar_t membar)
{
	uint64_t entry[2];
	uint32_t esize = uvm_mmu_entry_size(tree->mmu_version, parent->depth);
	uint64_t span = uvm_mmu_entry_span(tree->mmu_version, parent->depth);
	uint64_t va = parent->va_base + (uint64_t)index * span;

	if (esize == 16) {
		struct uvm_page_table *big = parent->children[2 * index];
		struct uvm_page_table *small = parent->children[2 * index + 1];
		uint64_t big_addr = big != NULL ? uvm_pt_gpu_addr(tree, big) : 0;
		uint64_t small_addr = small != NULL ? uvm_pt_gpu_addr(tree, small) : 0;

		if (tree->mmu_version == 3)
			uvm_pte3_dual_pde(entry, big_addr, uvm_pt_aperture(), small_addr, uvm_pt_aperture());
		else
			uvm_pte2_dual_pde(entry, big_addr, uvm_pt_aperture(), small_addr, uvm_pt_aperture());
	} else {
		struct uvm_page_table *child = parent->children[index];

		if (child == NULL)
			entry[0] = 0;
		else if (tree->mmu_version == 3)
			entry[0] = uvm_pte3_pde(uvm_pt_gpu_addr(tree, child), uvm_pt_aperture());
		else
			entry[0] = uvm_pte2_pde(uvm_pt_gpu_addr(tree, child), uvm_pt_aperture());
	}
	struct uvm_pt_inval inv = { parent->depth, va, span, membar };

	return (uvm_pt_write(tree, parent, index * esize, entry, esize, &inv, NULL));
}

/*
 * uvm_pt_child - the table under `parent` at `index` (and half, at the dual
 * level) covering `va`, created on demand with its PDE written after the
 * table is initialised.  Takes one reference on the child for the caller.
 */
static struct uvm_page_table *
uvm_pt_child(struct uvm_page_tree *tree, struct uvm_page_table *parent, uint32_t index, int half,
    uint64_t page_size, NV_STATUS *status)
{
	struct uvm_page_table **slot;
	struct uvm_page_table *child;
	uint32_t child_depth = parent->depth + 1;
	uint64_t child_page_size = child_depth > uvm_mmu_dual_depth(tree->mmu_version) ? page_size : 0;
	uint64_t va = parent->va_base + (uint64_t)index * uvm_mmu_entry_span(tree->mmu_version, parent->depth);

	if (parent->depth == 0 && (tree->root_rm_mask & (1ULL << index)) != 0) {
		/* RM filled this root entry at the hand-over: not ours to replace */
		uvm_log_ratelimited("page tree: root entry %u belongs to RM; VA %#jx cannot be mapped", index,
		    (uintmax_t)va);
		*status = NV_ERR_INVALID_ADDRESS;
		return (NULL);
	}
	slot = &parent->children[half >= 0 ? 2 * index + half : index];
	child = *slot;
	if (child == NULL) {
		child = uvm_pt_alloc(tree, child_depth, child_page_size, parent, index, half, va);
		if (child == NULL) {
			*status = NV_ERR_NO_MEMORY;
			return (NULL);
		}
		*slot = child;
		parent->refs++;
		*status = uvm_pt_write_pde(tree, parent, index, UVM_MEMBAR_SYS);
		if (*status != NV_OK) {
			*slot = NULL;
			parent->refs--;
			uvm_tracker_wait(&tree->tracker);
			uvm_pt_free_memory(tree, child);
			return (NULL);
		}
	}
	child->refs++;
	*status = NV_OK;
	return (child);
}

/* drop one reference; a table with none left is unlinked, its PDE cleared, and freed */
static void
uvm_pt_put(struct uvm_page_tree *tree, struct uvm_page_table *t)
{
	struct uvm_page_table *parent;

	while (t != NULL && t != tree->root) {
		if (--t->refs > 0)
			return;
		parent = t->parent;
		parent->children[t->parent_half >= 0 ? 2 * t->parent_index + t->parent_half : t->parent_index] =
		    NULL;
		(void)uvm_pt_write_pde(tree, parent, t->parent_index, UVM_MEMBAR_SYS);
		(void)uvm_tracker_wait(&tree->tracker);
		uvm_pt_free_memory(tree, t);
		t = parent;
	}
}

NV_STATUS
uvm_page_tree_create(struct uvm_gpu *gpu, bool is_user, struct uvm_page_tree **out)
{
	struct uvm_page_tree *tree;

	tree = malloc(sizeof(*tree), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	tree->gpu = gpu;
	tree->mmu_version = gpu->mmu_version;
	tree->is_user = is_user;
	sx_init(&tree->lock, "nvidia_uvm_page_tree");
	uvm_tracker_init(&tree->tracker);
	tree->root = uvm_pt_alloc(tree, 0, 0, NULL, 0, -1, 0);
	if (tree->root == NULL) {
		uvm_tracker_deinit(&tree->tracker);
		sx_destroy(&tree->lock);
		free(tree, M_NVIDIA_UVM);
		return (NV_ERR_NO_MEMORY);
	}
	tree->root->refs = 1;
	if (uvm_tracker_wait(&tree->tracker) != NV_OK) {
		uvm_page_tree_destroy(tree);
		return (uvm_global_status());
	}
	*out = tree;
	return (NV_OK);
}

/*
 * uvm_page_tree_record_rm_root - after the page directory hand-over RM has
 * copied its own top-level entries into our root (02 section 3.5); read
 * them back and mark their indexes as RM's.
 */
NV_STATUS
uvm_page_tree_record_rm_root(struct uvm_page_tree *tree)
{
	const volatile uint64_t *root = tree->root->kva;
	uint32_t i, esize = uvm_mmu_entry_size(tree->mmu_version, 0) / 8;

	tree->root_rm_mask = 0;
	for (i = 0; i < tree->root->entries; i++) {
		if (root[i * esize] != 0 && tree->root->children[i] == NULL)
			tree->root_rm_mask |= 1ULL << i;
	}
	if (tree->root_rm_mask != 0)
		uvm_log("page tree: RM owns root entries %#jx after the hand-over", (uintmax_t)tree->root_rm_mask);
	return (NV_OK);
}

void
uvm_page_tree_destroy(struct uvm_page_tree *tree)
{
	if (tree == NULL)
		return;
	(void)uvm_tracker_wait(&tree->tracker);
	if (tree->root != NULL) {
		if (tree->root->refs != 1)
			uvm_log("page tree destroyed with %u references on the root", tree->root->refs);
		uvm_pt_free_memory(tree, tree->root);
	}
	uvm_tracker_deinit(&tree->tracker);
	sx_destroy(&tree->lock);
	free(tree, M_NVIDIA_UVM);
}

/*
 * uvm_page_tree_get_ptes - the PTE table and entry run covering [va,
 * va+size) at `page_size`; the run must lie within one table.  Directories
 * on the path are created as needed.  Caller holds tree->lock.
 */
NV_STATUS
uvm_page_tree_get_ptes(struct uvm_page_tree *tree, uint64_t page_size, uint64_t va, uint64_t size,
    struct uvm_pt_range *range)
{
	struct uvm_page_table *t = tree->root, *child;
	uint32_t pte_depth = uvm_mmu_pte_depth(tree->mmu_version, page_size);
	uint32_t dual = uvm_mmu_dual_depth(tree->mmu_version);
	uint32_t depth, index, first, last;
	NV_STATUS status;

	if (size == 0 || (va & (page_size - 1)) != 0 || (size & (page_size - 1)) != 0)
		return (NV_ERR_INVALID_ADDRESS);
	sx_assert(&tree->lock, SA_XLOCKED);
	t->refs++;
	for (depth = 0; depth < pte_depth; depth++) {
		int half = -1;

		index = uvm_mmu_index(tree->mmu_version, depth, va, page_size);
		if (depth == dual)
			half = page_size == UVM_PAGE_SIZE_64K ? 0 : 1;
		child = uvm_pt_child(tree, t, index, half, page_size, &status);
		if (child == NULL) {
			uvm_pt_put(tree, t);
			return (status);
		}
		/* the parent's reference is now carried by the child's existence */
		if (t != tree->root)
			t->refs--;
		else
			tree->root->refs--;
		t = child;
	}
	first = uvm_mmu_index(tree->mmu_version, pte_depth, va, page_size);
	last = uvm_mmu_index(tree->mmu_version, pte_depth, va + size - 1, page_size);
	if (last < first || last >= t->entries) {
		uvm_pt_put(tree, t);
		return (NV_ERR_INVALID_ADDRESS);
	}
	range->tree = tree;
	range->table = t;
	range->start_index = first;
	range->count = last - first + 1;
	range->page_size = page_size;
	range->va_start = va;
	return (NV_OK);
}

void
uvm_page_tree_put_ptes(struct uvm_page_tree *tree, struct uvm_pt_range *range)
{
	sx_assert(&tree->lock, SA_XLOCKED);
	if (range->table == NULL)
		return;
	uvm_pt_put(tree, range->table);
	range->table = NULL;
}

/*
 * uvm_page_tree_write_ptes - write `count` entries starting at index
 * `first` of the range's table.  At the dual level each PTE occupies a
 * 16-byte entry (the PTE in the big half, the small half 0).
 */
NV_STATUS
uvm_page_tree_write_ptes(struct uvm_page_tree *tree, struct uvm_pt_range *range, uint32_t first,
    const uint64_t *ptes, uint32_t count, uvm_membar_t membar, struct uvm_tracker *tracker)
{
	struct uvm_page_table *t = range->table;
	uint32_t esize = uvm_mmu_entry_size(tree->mmu_version, t->depth);
	uint32_t index = range->start_index + first;
	uint64_t va = range->va_start + (uint64_t)first * range->page_size;
	uint64_t *buf;
	NV_STATUS status;
	uint32_t i, chunk, done = 0;
	const uint32_t max_entries = (UVM_PUSH_INLINE_MAX_WORDS * 4) / 16;

	sx_assert(&tree->lock, SA_XLOCKED);
	if (first + count > range->count)
		return (NV_ERR_INVALID_ARGUMENT);
	buf = malloc(max_entries * esize, M_NVIDIA_UVM, M_WAITOK);
	while (done < count) {
		chunk = count - done;
		if (chunk > max_entries)
			chunk = max_entries;
		for (i = 0; i < chunk; i++) {
			if (esize == 16) {
				buf[2 * i] = ptes[done + i];
				buf[2 * i + 1] = 0;
			} else {
				buf[i] = ptes[done + i];
			}
		}
		struct uvm_pt_inval inv = { t->depth, va + (uint64_t)done * range->page_size,
		    (uint64_t)chunk * range->page_size, membar };

		status = uvm_pt_write(tree, t, (index + done) * esize, buf, (size_t)chunk * esize, &inv, tracker);
		if (status != NV_OK) {
			free(buf, M_NVIDIA_UVM);
			return (status);
		}
		done += chunk;
	}
	free(buf, M_NVIDIA_UVM);
	return (NV_OK);
}

NV_STATUS
uvm_page_tree_clear_ptes(struct uvm_page_tree *tree, struct uvm_pt_range *range, uvm_membar_t membar,
    struct uvm_tracker *tracker)
{
	uint64_t *ptes;
	uint64_t inv = uvm_page_tree_invalid_pte(tree, range->page_size);
	NV_STATUS status;
	uint32_t i;

	ptes = malloc(sizeof(uint64_t) * range->count, M_NVIDIA_UVM, M_WAITOK);
	for (i = 0; i < range->count; i++)
		ptes[i] = inv;
	status = uvm_page_tree_write_ptes(tree, range, 0, ptes, range->count, membar, tracker);
	free(ptes, M_NVIDIA_UVM);
	return (status);
}

/* ---- range vectors ---- */

/*
 * uvm_pt_range_vec_init - the PTE ranges covering [va, va+size) at one page
 * size, one per table the run touches (a table covers 2M of 4K or 64K
 * pages, 512M of 2M pages).
 */
NV_STATUS
uvm_pt_range_vec_init(struct uvm_page_tree *tree, uint64_t va, uint64_t size, uint64_t page_size,
    struct uvm_pt_range_vec *vec)
{
	uint64_t table_span = page_size == UVM_PAGE_SIZE_2M ? (1ULL << 29) : (1ULL << 21);
	uint64_t cur = va, end = va + size;
	unsigned n = 0, i;
	NV_STATUS status;

	memset(vec, 0, sizeof(*vec));
	if (size == 0 || end < va)
		return (NV_ERR_INVALID_ADDRESS);
	vec->va = va;
	vec->size = size;
	vec->page_size = page_size;
	vec->count = (unsigned)(((end - 1) / table_span) - (va / table_span) + 1);
	vec->ranges = malloc(sizeof(*vec->ranges) * vec->count, M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	sx_xlock(&tree->lock);
	while (cur < end) {
		uint64_t table_end = (cur / table_span + 1) * table_span;
		uint64_t piece = (table_end < end ? table_end : end) - cur;

		status = uvm_page_tree_get_ptes(tree, page_size, cur, piece, &vec->ranges[n]);
		if (status != NV_OK) {
			for (i = 0; i < n; i++)
				uvm_page_tree_put_ptes(tree, &vec->ranges[i]);
			sx_xunlock(&tree->lock);
			free(vec->ranges, M_NVIDIA_UVM);
			vec->ranges = NULL;
			vec->count = 0;
			return (status);
		}
		n++;
		cur += piece;
	}
	sx_xunlock(&tree->lock);
	return (NV_OK);
}

void
uvm_pt_range_vec_deinit(struct uvm_page_tree *tree, struct uvm_pt_range_vec *vec)
{
	unsigned i;

	if (vec->ranges == NULL)
		return;
	sx_xlock(&tree->lock);
	for (i = 0; i < vec->count; i++)
		uvm_page_tree_put_ptes(tree, &vec->ranges[i]);
	sx_xunlock(&tree->lock);
	free(vec->ranges, M_NVIDIA_UVM);
	vec->ranges = NULL;
	vec->count = 0;
}

/* write the PTEs of the whole vector from a flat array (one per page) */
NV_STATUS
uvm_pt_range_vec_write(struct uvm_page_tree *tree, struct uvm_pt_range_vec *vec, const uint64_t *ptes,
    uvm_membar_t membar, struct uvm_tracker *tracker)
{
	unsigned i;
	uint64_t done = 0;
	NV_STATUS status = NV_OK;

	sx_xlock(&tree->lock);
	for (i = 0; i < vec->count && status == NV_OK; i++) {
		status = uvm_page_tree_write_ptes(tree, &vec->ranges[i], 0, ptes + done, vec->ranges[i].count,
		    membar, tracker);
		done += vec->ranges[i].count;
	}
	sx_xunlock(&tree->lock);
	return (status);
}

NV_STATUS
uvm_pt_range_vec_write_same(struct uvm_page_tree *tree, struct uvm_pt_range_vec *vec, uint64_t pte,
    uvm_membar_t membar, struct uvm_tracker *tracker)
{
	uint64_t *ptes;
	uint64_t n = vec->size / vec->page_size, i;
	NV_STATUS status;

	ptes = malloc(sizeof(uint64_t) * n, M_NVIDIA_UVM, M_WAITOK);
	for (i = 0; i < n; i++)
		ptes[i] = pte;
	status = uvm_pt_range_vec_write(tree, vec, ptes, membar, tracker);
	free(ptes, M_NVIDIA_UVM);
	return (status);
}

NV_STATUS
uvm_pt_range_vec_clear(struct uvm_page_tree *tree, struct uvm_pt_range_vec *vec, uvm_membar_t membar,
    struct uvm_tracker *tracker)
{
	unsigned i;
	NV_STATUS status = NV_OK;

	sx_xlock(&tree->lock);
	for (i = 0; i < vec->count && status == NV_OK; i++)
		status = uvm_page_tree_clear_ptes(tree, &vec->ranges[i], membar, tracker);
	sx_xunlock(&tree->lock);
	return (status);
}
