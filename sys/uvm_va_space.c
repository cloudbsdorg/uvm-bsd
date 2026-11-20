/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_va_space.c - VA spaces, the range tree, GPU and GPU-VA-space and
 * channel registration, external ranges and allocations, semaphore pools,
 * dynamic-parallelism ranges, range groups, and teardown.  uvm-spec r1,
 * 03-va-space-and-ranges.md, with the ioctl semantics of 01 section 5 and
 * the lock rules of 08 (RM is never called with the VA space lock held for
 * write; every operation that needs both runs in phases and re-validates).
 *
 * Phase-1 scope: no managed ranges (mmap of free addresses is a later
 * phase), no peers, one GPU VA space per GPU per VA space, every range
 * type libcuda needs for cudaMalloc, cudaMemcpy and kernel launches.
 */

#include "uvm.h"

#include <sys/refcount.h>

#include "uvm_ext_split.h"
#include "uvm_ext_pte.h"

/* ---- the range tree (03 section 4): the core's interval tree ---- */

NV_STATUS
uvm_range_create(struct uvm_va_space *vas, enum uvm_range_type type, uint64_t start, uint64_t end,
    struct uvm_range **out)
{
	struct uvm_range *r;
	int error;

	sx_assert(&vas->lock, SA_XLOCKED);
	r = malloc(sizeof(*r), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	r->core.start = start;
	r->core.end = end;
	r->type = type;
	r->vas = vas;
	switch (type) {
	case UVM_RANGE_EXTERNAL:
		TAILQ_INIT(&r->u.ext.gpus);
		break;
	case UVM_RANGE_SEMA_POOL:
	case UVM_RANGE_MANAGED:
		TAILQ_INIT(&r->u.sema.gpus);
		uvm_tracker_init(&r->u.sema.tracker);
		break;
	case UVM_RANGE_CHANNEL:
		uvm_tracker_init(&r->u.chan.tracker);
		break;
	default:
		break;
	}
	error = uvm_core_range_insert(&vas->ranges, &r->core);
	if (error != 0) {
		free(r, M_NVIDIA_UVM);
		return (NV_ERR_UVM_ADDRESS_IN_USE);
	}
	*out = r;
	return (NV_OK);
}

/* ---- common checks ---- */

static bool
uvm_interval_ok(uint64_t base, uint64_t length, uint64_t align)
{
	if (length == 0 || (base & (align - 1)) != 0 || (length & (align - 1)) != 0)
		return (false);
	if (base + length - 1 < base)
		return (false);
	return (true);
}

/* the mapping/caching/format/element/compression enumerations of 01 section 2.5 */
static bool
uvm_attrs_valid(const UvmGpuMappingAttributes *a)
{
	return (a->gpuMappingType < UvmGpuMappingTypeCount && a->gpuCachingType < UvmGpuCachingTypeCount &&
	    a->gpuFormatType < UvmGpuFormatTypeCount && a->gpuElementBits < UvmGpuFormatElementBitsCount &&
	    a->gpuElementBits != 3 && a->gpuCompressionType < UvmGpuCompressionTypeCount);
}

/* the registered GPU for a UUID, or NULL; caller holds the VA space lock */
static struct uvm_gpu *
uvm_vas_gpu_locked(struct uvm_va_space *vas, const NvProcessorUuid *uuid)
{
	int i;

	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++)
		if (vas->gpus[i] != NULL && uvm_uuid_eq(&vas->gpus[i]->uuid, uuid))
			return (vas->gpus[i]);
	return (NULL);
}

NV_STATUS
uvm_vas_gpu_registered(struct uvm_va_space *vas, const NvProcessorUuid *uuid, struct uvm_gpu **out)
{
	struct uvm_gpu *gpu;

	sx_slock(&vas->lock);
	gpu = uvm_vas_gpu_locked(vas, uuid);
	sx_sunlock(&vas->lock);
	if (gpu == NULL)
		return (NV_ERR_INVALID_DEVICE);
	if (out != NULL)
		*out = gpu;
	return (NV_OK);
}

/* ---- VA space lifetime (03 section 2) ---- */

NV_STATUS
uvm_va_space_create(uint64_t flags, struct uvm_va_space **out)
{
	struct uvm_va_space *vas;

	if ((flags & ~UVM_INIT_FLAGS_MASK) != 0)
		return (NV_ERR_INVALID_ARGUMENT);
	vas = malloc(sizeof(*vas), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	sx_init(&vas->lock, "nvidia_uvm_va_space");
	sx_init(&vas->serialize, "nvidia_uvm_va_space_ser");
	uvm_core_range_tree_init(&vas->ranges);
	uvm_core_group_set_init(&vas->groups);
	vas->init_flags = flags;
	vas->refs = 1;
	vas->owner_pid = curproc->p_pid;
	if (uvm.enable_va_space_mm) {
		vas->vm = vmspace_acquire_ref(curproc);
		if (vas->vm == NULL) {
			sx_destroy(&vas->serialize);
			sx_destroy(&vas->lock);
			free(vas, M_NVIDIA_UVM);
			return (NV_ERR_PAGE_TABLE_NOT_AVAIL);
		}
	}
	sx_xlock(&uvm.lock);
	TAILQ_INSERT_TAIL(&uvm.va_spaces, vas, link);
	sx_xunlock(&uvm.lock);
	*out = vas;
	return (NV_OK);
}

void
uvm_va_space_ref(struct uvm_va_space *vas)
{
	atomic_add_int(&vas->refs, 1);
}

void
uvm_va_space_unref(struct uvm_va_space *vas)
{
	if (atomic_fetchadd_int(&vas->refs, -1) == 1)
		uvm_va_space_destroy(vas);
}

/* ---- user channels (03 section 3.2) ---- */

struct uvm_user_channel *
uvm_user_channel_ref(struct uvm_user_channel *ch)
{
	atomic_add_int(&ch->refs, 1);
	return (ch);
}

static void uvm_channel_range_unref(struct uvm_range *r);

/* the last reference: the RM retained channel goes after the clear-faulted pushes that name it */
void
uvm_user_channel_unref(struct uvm_user_channel *ch)
{
	if (atomic_fetchadd_int(&ch->refs, -1) != 1)
		return;
	(void)uvm_tracker_wait(&ch->clear_faulted_tracker);
	uvm_tracker_deinit(&ch->clear_faulted_tracker);
	if (ch->rm_retained != NULL)
		nvUvmInterfaceReleaseChannel(ch->rm_retained);
	free(ch->info, M_NVIDIA_UVM);
	free(ch, M_NVIDIA_UVM);
}

/* stop in RM (idle, off the runlist); needs the VA space held for read with serialisation */
static void
uvm_user_channel_stop(struct uvm_user_channel *ch)
{
	if (!ch->stopped && ch->rm_retained != NULL) {
		nvUvmInterfaceStopChannel(ch->rm_retained, NV_FALSE);
		ch->stopped = true;
		ch->bound = false;
	}
}

/* detach: out of the attribution table and the GPU VA space's list; resources dropped.  VA space write-locked. */
static void
uvm_user_channel_detach(struct uvm_user_channel *ch)
{
	struct uvm_gpu_va_space *gvas = ch->gvas;
	unsigned i;

	if (ch->in_instance_table)
		uvm_gpu_instance_remove(gvas->gpu, ch);
	TAILQ_REMOVE(&gvas->channels, ch, link);
	for (i = 0; i < ch->nresources; i++) {
		if (ch->resources[i] != NULL) {
			uvm_channel_range_unref(ch->resources[i]);
			ch->resources[i] = NULL;
		}
	}
	ch->nresources = 0;
}

/* ---- channel resource ranges (03 section 4.3) ---- */

static void
uvm_channel_range_unref(struct uvm_range *r)
{
	struct uvm_gpu_va_space *gvas = r->u.chan.gvas;

	if (--r->u.chan.refs > 0)
		return;
	if (r->u.chan.mapped) {
		(void)uvm_pt_range_vec_clear(gvas->tree, &r->u.chan.ptes, UVM_MEMBAR_GPU, &r->u.chan.tracker);
		(void)uvm_tracker_wait(&r->u.chan.tracker);
		uvm_pt_range_vec_deinit(gvas->tree, &r->u.chan.ptes);
	}
	uvm_tracker_deinit(&r->u.chan.tracker);
	uvm_core_range_remove(&r->vas->ranges, &r->core);
	free(r, M_NVIDIA_UVM);
}

/*
 * uvm_channel_range_map - page tables at the resource's page size and the
 * PTEs RM builds for the context buffer, written in pieces.
 */
static NV_STATUS
uvm_channel_range_map(struct uvm_range *r, const UvmGpuChannelResourceInfo *res)
{
	struct uvm_gpu_va_space *gvas = r->u.chan.gvas;
	UvmGpuExternalMappingInfo info;
	uint64_t page_size = res->resourceInfo.pageSize;
	uint64_t size = r->core.end - r->core.start + 1;
	uint64_t npages, done = 0, offset = 0;
	uint64_t *ptes;
	NV_STATUS status;
	const uint32_t chunk = 512;

	if (page_size != UVM_PAGE_SIZE_4K && page_size != UVM_PAGE_SIZE_64K && page_size != UVM_PAGE_SIZE_2M)
		page_size = UVM_PAGE_SIZE_4K;
	status = uvm_pt_range_vec_init(gvas->tree, r->core.start, size, page_size, true, &r->u.chan.ptes);
	if (status != NV_OK)
		return (status);
	npages = size / page_size;
	/* RM writes 16-byte entries for a 2M page (uvm_ext_pte.h): room for the larger */
	ptes = malloc(uvm_ext_pte_entry_size(page_size) * chunk, M_NVIDIA_UVM, M_WAITOK);
	while (done < npages) {
		uint64_t want = npages - done;
		unsigned i;

		if (want > chunk)
			want = chunk;
		memset(&info, 0, sizeof(info));
		info.pteBufferSize = (uint64_t)want * uvm_ext_pte_entry_size(page_size);
		info.pteBuffer = ptes;
		status = nvUvmInterfaceGetChannelResourcePtes(gvas->rm_as, res->resourceDescriptor, offset,
		    want * page_size, &info);
		if (status != NV_OK)
			break;
		if (!uvm_ext_pte_size_ok(page_size, info.pteSize) || info.numWrittenPtes == 0 ||
		    info.numWrittenPtes > want) {
			status = NV_ERR_INVALID_STATE;
			break;
		}
		uvm_ext_pte_compact(ptes, info.pteSize, info.numWrittenPtes);
		sx_xlock(&gvas->tree->lock);
		for (i = 0; i < r->u.chan.ptes.count && status == NV_OK; i++) {
			struct uvm_pt_range *pr = &r->u.chan.ptes.ranges[i];
			uint64_t pr_first = (pr->va_start - r->core.start) / page_size;
			uint64_t pr_last = pr_first + pr->count - 1;
			uint64_t w_first = done, w_last = done + info.numWrittenPtes - 1;
			uint64_t lo = pr_first > w_first ? pr_first : w_first;
			uint64_t hi = pr_last < w_last ? pr_last : w_last;

			if (lo > hi)
				continue;
			status = uvm_page_tree_write_ptes(gvas->tree, pr, (uint32_t)(lo - pr_first), ptes + (lo - w_first),
			    (uint32_t)(hi - lo + 1), UVM_MEMBAR_GPU, &r->u.chan.tracker);
		}
		sx_xunlock(&gvas->tree->lock);
		if (status != NV_OK)
			break;
		done += info.numWrittenPtes;
		offset += info.numWrittenPtes * page_size;
	}
	free(ptes, M_NVIDIA_UVM);
	if (status == NV_OK)
		status = uvm_tracker_wait(&r->u.chan.tracker);
	if (status != NV_OK) {
		uvm_pt_range_vec_deinit(gvas->tree, &r->u.chan.ptes);
		return (status);
	}
	r->u.chan.mapped = true;
	return (NV_OK);
}

/* first-fit search for `size` bytes aligned to `align` inside [base, base+length) with no range overlap */
static bool
uvm_find_free_va(struct uvm_va_space *vas, uint64_t base, uint64_t length, uint64_t size, uint64_t align,
    uint64_t *out)
{
	return (uvm_core_range_find_free(&vas->ranges, base, length, size, align, out) == 0);
}

static int
uvm_resource_cmp_align(const void *a, const void *b)
{
	const UvmGpuChannelResourceInfo *ra = *(const UvmGpuChannelResourceInfo *const *)a;
	const UvmGpuChannelResourceInfo *rb = *(const UvmGpuChannelResourceInfo *const *)b;

	if (ra->alignment > rb->alignment)
		return (-1);
	return (ra->alignment < rb->alignment);
}

/*
 * uvm_register_channel_retain - phase 1 of channel registration: with the
 * VA space read-locked and RM serialised, RM retains the channel and
 * describes it; the resource placement interval is validated.
 */
static NV_STATUS
uvm_register_channel_retain(struct uvm_va_space *vas, struct uvm_user_channel *ch,
    const UVM_REGISTER_CHANNEL_PARAMS *p, struct uvm_gpu **gpu_out)
{
	struct uvm_gpu *gpu;
	struct uvm_gpu_va_space *gvas;
	NV_STATUS status;

	sx_xlock(&vas->serialize);
	sx_slock(&vas->lock);
	gpu = uvm_vas_gpu_locked(vas, &p->gpuUuid);
	gvas = gpu != NULL ? vas->gvas[gpu->id] : NULL;
	if (gvas == NULL || gvas->dead || gvas->disallow_new_channels) {
		status = NV_ERR_INVALID_DEVICE;
		goto out;
	}
	if (vas->user_channels_stopped) {
		status = NV_ERR_INVALID_STATE;
		goto out;
	}
	ch->gvas = gvas;
	status = nvUvmInterfaceRetainChannel(gvas->rm_as, p->hClient, p->hChannel, &ch->rm_retained, ch->info);
	if (status != NV_OK) {
		if (status == NV_ERR_INVALID_ARGUMENT || status == NV_ERR_OBJECT_NOT_FOUND)
			status = NV_ERR_INVALID_CHANNEL;
		goto out;
	}
	ch->inst_phys = ch->info->base;
	ch->inst_aperture = ch->info->sysmem ? UVM_APERTURE_SYS : UVM_APERTURE_VID;
	if (ch->info->resourceCount > UVM_GPU_CHANNEL_MAX_RESOURCES)
		status = NV_ERR_INVALID_STATE;
	else if (ch->info->resourceCount > 0 && !uvm_interval_ok(p->base, p->length, PAGE_SIZE))
		status = NV_ERR_INVALID_ADDRESS;
	else if (ch->info->resourceCount > 0 && p->base + p->length - 1 >= gpu->max_channel_va)
		status = NV_ERR_OUT_OF_RANGE;
	*gpu_out = gpu;
out:
	sx_sunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	return (status);
}

/*
 * uvm_channel_range_shared - the channel range TSG siblings already hold
 * for a resource: same GPU VA space, same TSG and runlist, same RM
 * descriptor (03 section 3.2).  Caller holds the VA space lock.
 * Time:  O(ranges).
 */
static struct uvm_range *
uvm_channel_range_shared(struct uvm_va_space *vas, const struct uvm_user_channel *ch,
    const UvmGpuChannelResourceInfo *res)
{
	struct uvm_range *cand;

	if (!ch->info->bTsgChannel)
		return (NULL);
	for (cand = uvm_range_first(vas); cand != NULL; cand = uvm_range_next(vas, cand)) {
		if (cand->type == UVM_RANGE_CHANNEL && cand->u.chan.gvas == ch->gvas && cand->u.chan.tsg_channel &&
		    cand->u.chan.tsg_id == ch->info->tsgId && cand->u.chan.runlist_id == ch->info->runlistId &&
		    cand->u.chan.descriptor == res->resourceDescriptor)
			return (cand);
	}
	return (NULL);
}

/*
 * uvm_register_channel_place - phase 2: under the write lock, re-validate,
 * refuse a duplicate handle pair, and give every resource a channel range
 * (a shared one for TSG siblings, else the first fit by decreasing
 * alignment).  On success the VA space stays locked for write.
 */
static NV_STATUS
uvm_register_channel_place(struct uvm_va_space *vas, struct uvm_user_channel *ch,
    const UVM_REGISTER_CHANNEL_PARAMS *p, const UvmGpuChannelResourceInfo **sorted,
    UvmGpuChannelResourceBindParams *bind)
{
	struct uvm_gpu_va_space *gvas = ch->gvas;
	struct uvm_user_channel *other;
	unsigned i;

	sx_xlock(&vas->lock);
	if (gvas->dead || vas->gvas[gvas->gpu->id] != gvas || gvas->disallow_new_channels)
		return (NV_ERR_INVALID_DEVICE);
	if (vas->user_channels_stopped)
		return (NV_ERR_INVALID_STATE);
	TAILQ_FOREACH(other, &gvas->channels, link)
		if (other->hClient == p->hClient && other->hChannel == p->hChannel)
			return (NV_ERR_INVALID_CHANNEL);
	for (i = 0; i < ch->info->resourceCount; i++) {
		const UvmGpuChannelResourceInfo *res = sorted[i];
		uint64_t page_size = res->resourceInfo.pageSize != 0 ? res->resourceInfo.pageSize : PAGE_SIZE;
		uint64_t align = res->alignment > page_size ? res->alignment : page_size;
		uint64_t size = roundup2(res->resourceInfo.size, page_size);
		struct uvm_range *r;
		uint64_t va;
		NV_STATUS status;

		r = uvm_channel_range_shared(vas, ch, res);
		if (r == NULL) {
			if (!uvm_find_free_va(vas, p->base, p->length, size, align, &va))
				return (NV_ERR_INVALID_ADDRESS);
			status = uvm_range_create(vas, UVM_RANGE_CHANNEL, va, va + size - 1, &r);
			if (status != NV_OK)
				return (status);
			r->u.chan.gvas = gvas;
			r->u.chan.descriptor = res->resourceDescriptor;
			r->u.chan.resource_id = res->resourceId;
			r->u.chan.runlist_id = ch->info->runlistId;
			r->u.chan.tsg_id = ch->info->tsgId;
			r->u.chan.tsg_channel = ch->info->bTsgChannel;
			r->u.chan.aperture = res->resourceInfo.sysmem ? UVM_APERTURE_SYS : UVM_APERTURE_VID;
		}
		r->u.chan.refs++;
		ch->resources[ch->nresources++] = r;
		bind[i].resourceId = res->resourceId;
		bind[i].resourceVa = r->core.start;
	}
	TAILQ_INSERT_TAIL(&gvas->channels, ch, link);
	return (NV_OK);
}

/* undo of a failed registration after the channel was linked: stop, detach, flush, drop */
static void
uvm_register_channel_undo(struct uvm_va_space *vas, struct uvm_user_channel *ch, struct uvm_gpu *gpu)
{
	sx_xlock(&vas->serialize);
	sx_slock(&vas->lock);
	uvm_user_channel_stop(ch);
	sx_sunlock(&vas->lock);
	sx_xlock(&vas->lock);
	uvm_user_channel_detach(ch);
	sx_xunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	uvm_gpu_fault_buffer_flush(gpu);
	uvm_user_channel_unref(ch);
}

/*
 * uvm_vas_register_channel - 01 section 5.4 / 03 section 3.2: retain in RM
 * (read lock), place the resources (write lock), map them with RM's PTEs
 * (read lock), attribute the instance pointer, bind in RM.
 */
NV_STATUS
uvm_vas_register_channel(struct uvm_va_space *vas, UVM_REGISTER_CHANNEL_PARAMS *p)
{
	struct uvm_gpu *gpu = NULL;
	struct uvm_user_channel *ch;
	const UvmGpuChannelResourceInfo *sorted[UVM_GPU_CHANNEL_MAX_RESOURCES];
	UvmGpuChannelResourceBindParams bind[UVM_GPU_CHANNEL_MAX_RESOURCES];
	NV_STATUS status;
	unsigned i;

	ch = malloc(sizeof(*ch), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	ch->info = malloc(sizeof(*ch->info), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	ch->hClient = p->hClient;
	ch->hChannel = p->hChannel;
	ch->refs = 1;
	uvm_tracker_init(&ch->clear_faulted_tracker);

	status = uvm_register_channel_retain(vas, ch, p, &gpu);
	if (status != NV_OK) {
		uvm_user_channel_unref(ch);
		return (status);
	}
	for (i = 0; i < ch->info->resourceCount; i++)
		sorted[i] = &ch->info->resourceInfo[i];
	qsort(sorted, ch->info->resourceCount, sizeof(sorted[0]), uvm_resource_cmp_align);

	sx_xlock(&vas->serialize);
	status = uvm_register_channel_place(vas, ch, p, sorted, bind);
	if (status != NV_OK) {
		for (i = 0; i < ch->nresources; i++)
			uvm_channel_range_unref(ch->resources[i]);
		ch->nresources = 0;
		sx_xunlock(&vas->lock);
		sx_xunlock(&vas->serialize);
		uvm_user_channel_unref(ch);
		return (status);
	}
	sx_downgrade(&vas->lock);

	/* phase 3: map the resources (RM under the read lock), attribute, bind */
	for (i = 0; i < ch->nresources && status == NV_OK; i++)
		if (!ch->resources[i]->u.chan.mapped)
			status = uvm_channel_range_map(ch->resources[i], sorted[i]);
	if (status == NV_OK) {
		uvm_gpu_instance_add(gpu, ch);
		if (ch->info->resourceCount > 0)
			status = nvUvmInterfaceBindChannelResources(ch->rm_retained, bind);
	}
	sx_sunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	if (status != NV_OK) {
		uvm_register_channel_undo(vas, ch, gpu);
		return (status);
	}
	ch->bound = true;
	return (NV_OK);
}

NV_STATUS
uvm_vas_unregister_channel(struct uvm_va_space *vas, UVM_UNREGISTER_CHANNEL_PARAMS *p)
{
	struct uvm_user_channel *ch = NULL, *c;
	struct uvm_gpu *gpu = NULL;
	int i;

	sx_xlock(&vas->serialize);
	sx_slock(&vas->lock);
	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST && ch == NULL; i++) {
		if (vas->gvas[i] == NULL)
			continue;
		TAILQ_FOREACH(c, &vas->gvas[i]->channels, link) {
			if (c->hClient == p->hClient && c->hChannel == p->hChannel) {
				ch = uvm_user_channel_ref(c);
				gpu = vas->gvas[i]->gpu;
				break;
			}
		}
	}
	if (ch == NULL) {
		sx_sunlock(&vas->lock);
		sx_xunlock(&vas->serialize);
		return (NV_ERR_INVALID_CHANNEL);
	}
	uvm_user_channel_stop(ch);
	sx_sunlock(&vas->lock);
	sx_xlock(&vas->lock);
	uvm_user_channel_detach(ch);
	sx_xunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	/* stale entries with this instance pointer must not be attributed to a successor */
	uvm_gpu_fault_buffer_flush(gpu);
	uvm_user_channel_unref(ch);	/* the list's reference */
	uvm_user_channel_unref(ch);	/* ours */
	return (NV_OK);
}

/* ---- semaphore pool ranges (03 section 4.4) ---- */

static NV_STATUS uvm_sema_pool_map_gpu(struct uvm_range *r, struct uvm_gpu_va_space *gvas);

static void
uvm_sema_pool_unmap_gpu(struct uvm_range *r, struct uvm_gpu *gpu)
{
	struct uvm_sema_gpu_map *m;

	TAILQ_FOREACH(m, &r->u.sema.gpus, link) {
		if (m->gpu == gpu)
			break;
	}
	if (m == NULL)
		return;
	TAILQ_REMOVE(&r->u.sema.gpus, m, link);
	if (r->vas->gvas[gpu->id] != NULL) {
		(void)uvm_pt_range_vec_clear(r->vas->gvas[gpu->id]->tree, &m->ptes, UVM_MEMBAR_SYS, &r->u.sema.tracker);
		(void)uvm_tracker_wait(&r->u.sema.tracker);
		uvm_pt_range_vec_deinit(r->vas->gvas[gpu->id]->tree, &m->ptes);
	}
	free(m, M_NVIDIA_UVM);
}

static void
uvm_sema_pool_destroy(struct uvm_range *r)
{
	struct uvm_sema_gpu_map *m;

	while ((m = TAILQ_FIRST(&r->u.sema.gpus)) != NULL)
		uvm_sema_pool_unmap_gpu(r, m->gpu);
	(void)uvm_tracker_wait(&r->u.sema.tracker);
	uvm_tracker_deinit(&r->u.sema.tracker);
	uvm_sema_mem_unref(r->u.sema.mem);
	r->u.sema.mem = NULL;
}

/* map the pool's pages read/write/atomic (uncached unless the GPU asked for cached) in a GPU VA space */
static NV_STATUS
uvm_sema_pool_map_gpu(struct uvm_range *r, struct uvm_gpu_va_space *gvas)
{
	struct uvm_sema_gpu_map *m;
	uint64_t size = r->core.end - r->core.start + 1, npages = size / PAGE_SIZE, i;
	uint64_t *ptes;
	bool cached = uvm_gpu_mask_test(&r->u.sema.cached_gpus, gvas->gpu->id);
	NV_STATUS status;

	TAILQ_FOREACH(m, &r->u.sema.gpus, link)
		if (m->gpu == gvas->gpu)
			return (NV_OK);
	m = malloc(sizeof(*m), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	m->gpu = gvas->gpu;
	m->cached = cached;
	status = uvm_pt_range_vec_init(gvas->tree, r->core.start, size, UVM_PAGE_SIZE_4K, true, &m->ptes);
	if (status != NV_OK) {
		free(m, M_NVIDIA_UVM);
		return (status);
	}
	ptes = malloc(sizeof(uint64_t) * npages, M_NVIDIA_UVM, M_WAITOK);
	for (i = 0; i < npages; i++) {
		uint64_t addr = uvm_gpu_dma_address(gvas->gpu, r->u.sema.mem->bus + i * PAGE_SIZE);

		ptes[i] = uvm_page_tree_pte(gvas->tree, addr, UVM_APERTURE_SYS, UVM_PROT_READ_WRITE_ATOMIC, !cached);
	}
	status = uvm_pt_range_vec_write(gvas->tree, &m->ptes, ptes, UVM_MEMBAR_SYS, &r->u.sema.tracker);
	free(ptes, M_NVIDIA_UVM);
	if (status == NV_OK)
		status = uvm_tracker_wait(&r->u.sema.tracker);
	if (status != NV_OK) {
		uvm_pt_range_vec_deinit(gvas->tree, &m->ptes);
		free(m, M_NVIDIA_UVM);
		return (status);
	}
	TAILQ_INSERT_TAIL(&r->u.sema.gpus, m, link);
	return (NV_OK);
}

/* the pool's memory: shared by the range and the user mapping's pager object */
struct uvm_sema_mem *
uvm_sema_mem_ref(struct uvm_sema_mem *mem)
{
	refcount_acquire(&mem->refs);
	return (mem);
}

void
uvm_sema_mem_unref(struct uvm_sema_mem *mem)
{
	if (mem == NULL || !refcount_release(&mem->refs))
		return;
	bus_dmamap_unload(mem->tag, mem->map);
	bus_dmamem_free(mem->tag, mem->kva, mem->map);
	if (mem->own_tag)
		bus_dma_tag_destroy(mem->tag);
	free(mem, M_NVIDIA_UVM);
}

static void
uvm_sema_dma_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	bus_addr_t *out = arg;

	*out = (error == 0 && nseg == 1) ? segs[0].ds_addr : 0;
}

/* contiguous, zeroed DMA memory for a pool, within the first registered GPU's window (or any) */
static struct uvm_sema_mem *
uvm_sema_mem_alloc(struct uvm_gpu *first_gpu, size_t bytes)
{
	struct uvm_sema_mem *mem;
	bus_dma_tag_t parent = NULL;
	bus_addr_t limit = BUS_SPACE_MAXADDR;
	int error;

	mem = malloc(sizeof(*mem), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	refcount_init(&mem->refs, 1);
	mem->bytes = bytes;
	if (first_gpu != NULL) {
		parent = bus_get_dma_tag((device_t)first_gpu->platform.dev);
		limit = first_gpu->platform.dma_addressable_limit;
	}
	error = bus_dma_tag_create(parent, PAGE_SIZE, 0, limit, BUS_SPACE_MAXADDR, NULL, NULL, bytes, 1, bytes, 0,
	    NULL, NULL, &mem->tag);
	if (error != 0) {
		free(mem, M_NVIDIA_UVM);
		return (NULL);
	}
	mem->own_tag = true;
	error = bus_dmamem_alloc(mem->tag, &mem->kva, BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, &mem->map);
	if (error != 0) {
		bus_dma_tag_destroy(mem->tag);
		free(mem, M_NVIDIA_UVM);
		return (NULL);
	}
	error = bus_dmamap_load(mem->tag, mem->map, mem->kva, bytes, uvm_sema_dma_cb, &mem->bus, BUS_DMA_NOWAIT);
	if (error != 0 || mem->bus == 0) {
		bus_dmamem_free(mem->tag, mem->kva, mem->map);
		bus_dma_tag_destroy(mem->tag);
		free(mem, M_NVIDIA_UVM);
		return (NULL);
	}
	return (mem);
}

/* validate the per-GPU attributes of a pool: registered GPUs, valid enumerations, at most one cached */
static NV_STATUS
uvm_sema_pool_check_attrs(struct uvm_va_space *vas, const UVM_ALLOC_SEMAPHORE_POOL_PARAMS *p,
    struct uvm_gpu_mask *cached, struct uvm_gpu **first_gpu)
{
	struct uvm_gpu *gpu;
	unsigned ncached = 0;
	uint64_t i;
	int j;

	memset(cached, 0, sizeof(*cached));
	*first_gpu = NULL;
	sx_slock(&vas->lock);
	for (i = 0; i < p->gpuAttributesCount; i++) {
		const UvmGpuMappingAttributes *a = &p->perGpuAttributes[i];

		if (!uvm_attrs_valid(a)) {
			sx_sunlock(&vas->lock);
			return (NV_ERR_INVALID_ARGUMENT);
		}
		gpu = uvm_vas_gpu_locked(vas, &a->gpuUuid);
		if (gpu == NULL) {
			sx_sunlock(&vas->lock);
			return (NV_ERR_INVALID_DEVICE);
		}
		if (a->gpuCachingType == UvmGpuCachingTypeForceCached) {
			uvm_gpu_mask_set(cached, gpu->id);
			ncached++;
		}
	}
	for (j = UVM_ID_GPU_FIRST; j <= UVM_ID_GPU_LAST && *first_gpu == NULL; j++)
		*first_gpu = vas->gpus[j];
	sx_sunlock(&vas->lock);
	return (ncached > 1 ? NV_ERR_INVALID_ARGUMENT : NV_OK);
}

/*
 * uvm_vas_alloc_semaphore_pool - 01 section 5.6: zeroed system memory,
 * mapped on every registered GPU that has a GPU VA space, mappable on the
 * CPU through mmap of exactly this range.
 */
NV_STATUS
uvm_vas_alloc_semaphore_pool(struct uvm_va_space *vas, UVM_ALLOC_SEMAPHORE_POOL_PARAMS *p)
{
	struct uvm_range *r;
	struct uvm_sema_mem *mem;
	struct uvm_gpu_mask cached;
	struct uvm_gpu *first_gpu;
	NV_STATUS status;
	int j;

	if (!uvm_interval_ok(p->base, p->length, PAGE_SIZE))
		return (NV_ERR_INVALID_ADDRESS);
	if (p->gpuAttributesCount > UVM_MAX_GPUS)
		return (NV_ERR_INVALID_ARGUMENT);
	status = uvm_sema_pool_check_attrs(vas, p, &cached, &first_gpu);
	if (status != NV_OK)
		return (status);
	mem = uvm_sema_mem_alloc(first_gpu, p->length);
	if (mem == NULL)
		return (NV_ERR_NO_MEMORY);

	sx_xlock(&vas->lock);
	status = uvm_range_create(vas, UVM_RANGE_SEMA_POOL, p->base, p->base + p->length - 1, &r);
	if (status != NV_OK) {
		sx_xunlock(&vas->lock);
		uvm_sema_mem_unref(mem);
		return (status);
	}
	r->u.sema.mem = mem;
	r->u.sema.cached_gpus = cached;
	sx_downgrade(&vas->lock);
	/* map on every GPU with a GPU VA space; later GPU VA spaces map it when they register */
	for (j = UVM_ID_GPU_FIRST; j <= UVM_ID_GPU_LAST && status == NV_OK; j++) {
		if (vas->gvas[j] != NULL && !vas->gvas[j]->dead)
			status = uvm_sema_pool_map_gpu(r, vas->gvas[j]);
	}
	sx_sunlock(&vas->lock);
	if (status != NV_OK) {
		sx_xlock(&vas->lock);
		uvm_sema_pool_destroy(r);
		uvm_core_range_remove(&vas->ranges, &r->core);
		sx_xunlock(&vas->lock);
		free(r, M_NVIDIA_UVM);
	}
	return (status);
}

static void uvm_range_teardown(struct uvm_range *r);

/*
 * ---- managed ranges (01 section 4.1, 03 section 4.1) ----
 *
 * First step of managed memory: a range that is CPU resident and mapped on
 * every GPU, backed by zeroed system memory like a semaphore pool.  No
 * fault is ever taken on the GPU side; the CPU side is the device pager.
 */

/*
 * uvm_vas_create_managed - the range [start, start+size) with its memory
 * and its pager handle.  The memory has two references, the range's and the
 * handle's.  NV_ERR_UVM_ADDRESS_IN_USE when the interval overlaps a range.
 */
NV_STATUS
uvm_vas_create_managed(struct uvm_va_space *vas, uint64_t start, uint64_t size, struct uvm_managed_obj **out)
{
	struct uvm_managed_obj *obj;
	struct uvm_sema_mem *mem;
	struct uvm_range *r;
	struct uvm_gpu *first_gpu = NULL;
	NV_STATUS status;
	int j;

	if (uvm_managed_mmap_check(start, size, PAGE_SIZE) != NV_OK)
		return (NV_ERR_INVALID_ADDRESS);
	sx_slock(&vas->lock);
	for (j = UVM_ID_GPU_FIRST; j <= UVM_ID_GPU_LAST && first_gpu == NULL; j++)
		first_gpu = vas->gpus[j];
	/* the interval must be free before any memory is spent on it */
	if (uvm_range_find_overlap(vas, start, start + size - 1) != NULL) {
		sx_sunlock(&vas->lock);
		return (NV_ERR_UVM_ADDRESS_IN_USE);
	}
	sx_sunlock(&vas->lock);

	mem = uvm_sema_mem_alloc(first_gpu, size);
	if (mem == NULL)
		return (NV_ERR_NO_MEMORY);
	mem->user_start = start;
	obj = malloc(sizeof(*obj), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	obj->mem = uvm_sema_mem_ref(mem);
	obj->start = start;
	obj->state = UVM_MOBJ_LINKED;

	sx_xlock(&vas->lock);
	status = uvm_range_create(vas, UVM_RANGE_MANAGED, start, start + size - 1, &r);
	if (status != NV_OK) {	/* lost a race with another mapping of the same addresses */
		sx_xunlock(&vas->lock);
		uvm_sema_mem_unref(obj->mem);
		uvm_sema_mem_unref(mem);
		free(obj, M_NVIDIA_UVM);
		return (status);
	}
	r->u.sema.mem = mem;
	r->u.sema.obj = obj;
	obj->range = r;
	atomic_add_int(&uvm.managed_objs, 1);
	sx_downgrade(&vas->lock);
	for (j = UVM_ID_GPU_FIRST; j <= UVM_ID_GPU_LAST && status == NV_OK; j++) {
		if (vas->gvas[j] != NULL && !vas->gvas[j]->dead)
			status = uvm_sema_pool_map_gpu(r, vas->gvas[j]);
	}
	sx_sunlock(&vas->lock);
	if (status != NV_OK) {
		uvm_managed_obj_release(obj);	/* destroys the range and frees the handle */
		return (status);
	}
	*out = obj;
	return (NV_OK);
}

/*
 * uvm_managed_obj_release - the pager object is gone (or its creation
 * failed): destroy the range if its VA space is still there, then free the
 * handle.  GPU work that UVM itself pushed on the range (its PTE writes) is
 * waited for by the range's teardown (the tracker wait of every unmap); the
 * application's own GPU work on the memory is its to finish before munmap,
 * as cudaFree does.  The managed lock is held across the destroy so that a
 * VA space destroy cannot free the range under us, and the VA space cannot
 * be freed while it holds a range we found linked.
 */
void
uvm_managed_obj_release(struct uvm_managed_obj *obj)
{
	enum uvm_mobj_state next_state;
	struct uvm_range *r;
	struct uvm_va_space *vas;

	sx_xlock(&uvm.managed_lock);
	switch (uvm_mobj_event_decide(obj->state, UVM_MOBJ_PAGER_DTOR, &next_state)) {
	case UVM_MOBJ_DO_DESTROY_RANGE_AND_FREE:
		r = obj->range;
		KASSERT(r != NULL && r->type == UVM_RANGE_MANAGED && r->u.sema.obj == obj, ("managed obj %p range %p", obj, r));
		vas = r->vas;
		sx_xlock(&vas->serialize);
		sx_slock(&vas->lock);
		uvm_range_teardown(r);
		sx_sunlock(&vas->lock);
		sx_xlock(&vas->lock);
		uvm_core_range_remove(&vas->ranges, &r->core);
		sx_xunlock(&vas->lock);
		sx_xunlock(&vas->serialize);
		free(r, M_NVIDIA_UVM);
		obj->range = NULL;
		break;
	case UVM_MOBJ_DO_FREE:
		KASSERT(obj->range == NULL, ("detached managed obj %p has a range", obj));
		break;
	default:
		panic("uvm managed obj %p: unexpected decision", obj);
	}
	sx_xunlock(&uvm.managed_lock);
	uvm_sema_mem_unref(obj->mem);
	free(obj, M_NVIDIA_UVM);
	atomic_subtract_int(&uvm.managed_objs, 1);
}

/* ---- external ranges and mappings (03 section 4.2, 01 section 5.5) ---- */

static struct uvm_ext_gpu_state *
uvm_ext_gpu_state(struct uvm_range *r, struct uvm_gpu *gpu, bool create)
{
	struct uvm_ext_gpu_state *s;

	TAILQ_FOREACH(s, &r->u.ext.gpus, link)
		if (s->gpu == gpu)
			return (s);
	if (!create)
		return (NULL);
	s = malloc(sizeof(*s), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	s->gpu = gpu;
	sx_init(&s->lock, "nvidia_uvm_ext_gpu");
	TAILQ_INIT(&s->mappings);
	TAILQ_INSERT_TAIL(&r->u.ext.gpus, s, link);
	return (s);
}

/*
 * Drop one reference on the shared RM handle; the handle is freed with RM
 * when the last part of the mapping is gone (refcount_release() returns
 * true when the count it drops was the last one).
 */
static void
uvm_ext_handle_unref(struct uvm_gpu *gpu, struct uvm_ext_handle *h)
{
	if (h == NULL)
		return;
	if (refcount_release(&h->refs)) {
		nvUvmInterfaceFreeDupedHandle(gpu->rm_device, h->dup);
		free(h, M_NVIDIA_UVM);
	}
}

/* clear the PTEs of a mapping, wait, release its page tables and RM handle; caller holds s->lock */
static void
uvm_ext_mapping_destroy(struct uvm_ext_gpu_state *s, struct uvm_ext_mapping *m, struct uvm_gpu_va_space *gvas)
{
	TAILQ_REMOVE(&s->mappings, m, link);
	if (gvas != NULL && m->ptes.ranges != NULL) {
		uvm_membar_t membar = m->is_sysmem ? UVM_MEMBAR_SYS : UVM_MEMBAR_GPU;

		(void)uvm_pt_range_vec_clear(gvas->tree, &m->ptes, membar, &m->tracker);
		if (m->l2_invalidate_at_unmap) {
			struct uvm_push push;

			if (uvm_push_begin(gvas->gpu, &push, &m->tracker) == NV_OK) {
				uint32_t *w = uvm_push_space(&push, UVM_HOST_L2_INVALIDATE_MAX_WORDS);

				push.next = w + uvm_host_l2_invalidate(w, 0);
				uvm_push_end(&push);
				uvm_tracker_add(&m->tracker, push.channel, push.value);
			}
		}
		(void)uvm_tracker_wait(&m->tracker);
		uvm_pt_range_vec_deinit(gvas->tree, &m->ptes);
	}
	uvm_tracker_deinit(&m->tracker);
	uvm_ext_handle_unref(s->gpu, m->handle);
	free(m, M_NVIDIA_UVM);
}

/* the largest GPU page size dividing every argument, capped (05: 512M is unused by this phase) */
static uint64_t
uvm_ext_page_size(uint64_t alloc_page_size, uint64_t base, uint64_t length, uint64_t offset, uint64_t cap)
{
	static const uint64_t sizes[] = { UVM_PAGE_SIZE_2M, UVM_PAGE_SIZE_64K, UVM_PAGE_SIZE_4K };
	unsigned i;

	for (i = 0; i < nitems(sizes); i++) {
		uint64_t ps = sizes[i];

		if (ps > cap)
			continue;
		if (alloc_page_size % ps == 0 && base % ps == 0 && length % ps == 0 && offset % ps == 0)
			return (ps);
	}
	return (0);
}

/* fetch RM's PTEs for [offset, offset+length) of the duplicated allocation and write them */
static NV_STATUS
uvm_ext_mapping_write(struct uvm_gpu_va_space *gvas, struct uvm_ext_mapping *m, const UvmGpuMappingAttributes *a,
    uint64_t offset, uint64_t length, uint64_t page_size)
{
	UvmGpuExternalMappingInfo info;
	uint64_t npages = length / page_size, done = 0;
	uint64_t *ptes;
	NV_STATUS status = NV_OK;
	const uint32_t chunk = 512;

	/* RM writes 16-byte entries for a 2M page (uvm_ext_pte.h): room for the larger */
	ptes = malloc(uvm_ext_pte_entry_size(page_size) * chunk, M_NVIDIA_UVM, M_WAITOK);
	while (done < npages) {
		uint64_t want = npages - done;
		unsigned i;

		if (want > chunk)
			want = chunk;
		memset(&info, 0, sizeof(info));
		info.cachingType = a->gpuCachingType;
		info.mappingType = a->gpuMappingType;
		info.formatType = a->gpuFormatType;
		info.elementBits = a->gpuElementBits;
		info.compressionType = a->gpuCompressionType;
		info.pteBufferSize = (uint64_t)want * uvm_ext_pte_entry_size(page_size);
		info.pteBuffer = ptes;
		info.mappingPageSize = page_size;
		status = nvUvmInterfaceGetExternalAllocPtes(gvas->rm_as, m->handle->dup, offset + done * page_size,
		    want * page_size, &info);
		if (status != NV_OK)
			break;
		if (!uvm_ext_pte_size_ok(page_size, info.pteSize) || info.numWrittenPtes == 0 ||
		    info.numWrittenPtes > want) {
			status = NV_ERR_INVALID_STATE;
			break;
		}
		uvm_ext_pte_compact(ptes, info.pteSize, info.numWrittenPtes);
		m->l2_invalidate_at_unmap |= info.bNeedL2InvalidateAtUnmap != 0;
		sx_xlock(&gvas->tree->lock);
		for (i = 0; i < m->ptes.count && status == NV_OK; i++) {
			struct uvm_pt_range *pr = &m->ptes.ranges[i];
			uint64_t pr_first = (pr->va_start - m->start) / page_size;
			uint64_t pr_last = pr_first + pr->count - 1;
			uint64_t w_last = done + info.numWrittenPtes - 1;
			uint64_t lo = pr_first > done ? pr_first : done;
			uint64_t hi = pr_last < w_last ? pr_last : w_last;

			if (lo > hi)
				continue;
			status = uvm_page_tree_write_ptes(gvas->tree, pr, (uint32_t)(lo - pr_first), ptes + (lo - done),
			    (uint32_t)(hi - lo + 1), m->is_sysmem ? UVM_MEMBAR_SYS : UVM_MEMBAR_GPU, &m->tracker);
		}
		sx_xunlock(&gvas->tree->lock);
		if (status != NV_OK)
			break;
		done += info.numWrittenPtes;
	}
	free(ptes, M_NVIDIA_UVM);
	return (status);
}

/*
 * The part of the mapping that the unmapped interval [start, end] covers,
 * rounded out to the mapping's page size: one PTE covers one whole page, so
 * a page the interval only partly covers is removed as a whole.  The parts of
 * the mapping that stay are therefore page aligned.
 */
static void
uvm_ext_overlap_pages(const struct uvm_ext_mapping *m, uint64_t start, uint64_t end, uint64_t *ov_start,
    uint64_t *ov_end)
{
	uint64_t ps = m->ptes.page_size;
	uint64_t lo = start > m->start ? start : m->start;
	uint64_t hi = end < m->end ? end : m->end;
	uint64_t excl;

	lo &= ~(ps - 1);
	excl = hi + 1;
	if ((excl & (ps - 1)) != 0)
		excl += ps - (excl & (ps - 1));
	*ov_start = lo < m->start ? m->start : lo;
	*ov_end = excl - 1 < m->end ? excl - 1 : m->end;
}

/*
 * Clear the PTEs of the removed part of a mapping, invalidate the L2 when
 * the mapping asked for it and wait for the GPU work; the pages outside the
 * part keep their PTEs.
 */
static NV_STATUS
uvm_ext_mapping_remove_part(struct uvm_ext_mapping *m, struct uvm_gpu_va_space *gvas, uint64_t start, uint64_t end)
{
	uvm_membar_t membar = m->is_sysmem ? UVM_MEMBAR_SYS : UVM_MEMBAR_GPU;
	NV_STATUS status;

	status = uvm_pt_range_vec_clear_sub(gvas->tree, &m->ptes, start, end - start + 1, membar, &m->tracker);
	if (status != NV_OK)
		return (status);
	if (m->l2_invalidate_at_unmap) {
		struct uvm_push push;

		if (uvm_push_begin(gvas->gpu, &push, &m->tracker) == NV_OK) {
			uint32_t *w = uvm_push_space(&push, UVM_HOST_L2_INVALIDATE_MAX_WORDS);

			push.next = w + uvm_host_l2_invalidate(w, 0);
			uvm_push_end(&push);
			uvm_tracker_add(&m->tracker, push.channel, push.value);
		}
	}
	return (uvm_tracker_wait(&m->tracker));
}

/*
 * Make the part [start, end] of a mapping that survives the unmap into a
 * mapping of its own: the same GPU, the same memory and the same shared RM
 * handle (one more reference), with its own page-table range vector, which
 * re-references the page tables of the part and writes no PTE, so the pages
 * of the part stay mapped the whole time.
 */
static NV_STATUS
uvm_ext_mapping_split_part(struct uvm_ext_mapping *m, struct uvm_gpu_va_space *gvas, uint64_t start, uint64_t end,
    struct uvm_ext_mapping **out)
{
	struct uvm_ext_mapping *p;
	NV_STATUS status;

	p = malloc(sizeof(*p), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	p->start = start;
	p->end = end;
	p->gpu = m->gpu;
	p->handle = m->handle;
	p->is_sysmem = m->is_sysmem;
	p->l2_invalidate_at_unmap = m->l2_invalidate_at_unmap;
	if (p->handle != NULL)
		refcount_acquire(&p->handle->refs);
	uvm_tracker_init(&p->tracker);
	status = uvm_pt_range_vec_init(gvas->tree, p->start, end - start + 1, m->ptes.page_size, false, &p->ptes);
	if (status != NV_OK) {
		uvm_tracker_deinit(&p->tracker);
		uvm_ext_handle_unref(p->gpu, p->handle);
		free(p, M_NVIDIA_UVM);
		return (status);
	}
	*out = p;
	return (NV_OK);
}

/* drop a part that was made but not inserted into the list */
static void
uvm_ext_mapping_free_part(struct uvm_ext_mapping *p, struct uvm_gpu_va_space *gvas)
{
	uvm_pt_range_vec_deinit(gvas->tree, &p->ptes);
	uvm_tracker_deinit(&p->tracker);
	uvm_ext_handle_unref(p->gpu, p->handle);
	free(p, M_NVIDIA_UVM);
}

/* unmap the parts of this GPU's mappings that overlap [start, end]; caller holds s->lock and the VA space read lock */
static NV_STATUS
uvm_ext_unmap_overlap(struct uvm_range *r, struct uvm_ext_gpu_state *s, struct uvm_gpu_va_space *gvas,
    uint64_t start, uint64_t end)
{
	struct uvm_ext_mapping *m, *next;
	NV_STATUS status = NV_OK;

	(void)r;
	TAILQ_FOREACH_SAFE(m, &s->mappings, link, next) {
		struct uvm_ext_split_plan plan;
		struct uvm_ext_mapping *left = NULL, *right = NULL;
		uint64_t ov_start, ov_end;

		if (m->end < start || m->start > end)
			continue;
		if (gvas == NULL || m->ptes.page_size == 0) {
			/* without a page tree the mapping's PTEs cannot be split */
			uvm_ext_mapping_destroy(s, m, gvas);
			continue;
		}
		uvm_ext_overlap_pages(m, start, end, &ov_start, &ov_end);
		uvm_ext_split_plan(m->start, m->end, ov_start, ov_end, &plan);
		if (!plan.keep_left && !plan.keep_right) {
			/* the whole mapping is covered: it goes as a whole */
			uvm_ext_mapping_destroy(s, m, gvas);
			continue;
		}
		/*
		 * A partial overlap: the spec splits the mapping and removes the
		 * overlap only (03 section 4.2).  The PTEs of the overlap are
		 * cleared, the parts that stay become mappings of their own and
		 * share the RM handle of the mapping they come from.
		 */
		status = uvm_ext_mapping_remove_part(m, gvas, ov_start, ov_end);
		if (status == NV_OK && plan.keep_left)
			status = uvm_ext_mapping_split_part(m, gvas, plan.left_start, plan.left_end, &left);
		if (status == NV_OK && plan.keep_right)
			status = uvm_ext_mapping_split_part(m, gvas, plan.right_start, plan.right_end, &right);
		if (status != NV_OK) {
			/* the split failed: the whole mapping goes, as it did before */
			uvm_log_ratelimited("external unmap of [%#jx, %#jx] cannot split mapping [%#jx, %#jx]: "
			    "the whole mapping is removed", (uintmax_t)start, (uintmax_t)end, (uintmax_t)m->start,
			    (uintmax_t)m->end);
			if (left != NULL)
				uvm_ext_mapping_free_part(left, gvas);
			uvm_ext_mapping_destroy(s, m, gvas);
			continue;
		}
		if (left != NULL)
			TAILQ_INSERT_BEFORE(m, left, link);
		if (right != NULL)
			TAILQ_INSERT_BEFORE(m, right, link);
		/* the old mapping goes; its page tables and handle reference go with it */
		uvm_pt_range_vec_deinit(gvas->tree, &m->ptes);
		uvm_tracker_deinit(&m->tracker);
		uvm_ext_handle_unref(m->gpu, m->handle);
		TAILQ_REMOVE(&s->mappings, m, link);
		free(m, M_NVIDIA_UVM);
	}
	return (status);
}

static NV_STATUS
uvm_ext_map_one(struct uvm_va_space *vas, struct uvm_range *r, const UvmGpuMappingAttributes *a,
    const UVM_MAP_EXTERNAL_ALLOCATION_PARAMS *p)
{
	struct uvm_gpu *gpu;
	struct uvm_gpu_va_space *gvas;
	struct uvm_ext_gpu_state *s;
	struct uvm_ext_mapping *m;
	struct uvm_ext_handle *h;
	UvmGpuMemoryInfo meminfo;
	NvHandle dup = 0;
	uint64_t page_size, cap;
	NV_STATUS status;

	if (!uvm_attrs_valid(a))
		return (NV_ERR_INVALID_ARGUMENT);
	gpu = uvm_vas_gpu_locked(vas, &a->gpuUuid);
	gvas = gpu != NULL ? vas->gvas[gpu->id] : NULL;
	if (gvas == NULL || gvas->dead)
		return (NV_ERR_INVALID_DEVICE);
	if (p->base + p->length - 1 >= gpu->max_channel_va)
		return (NV_ERR_OUT_OF_RANGE);
	status = nvUvmInterfaceDupMemory(gpu->rm_device, p->hClient, p->hMemory, &dup, &meminfo);
	if (status != NV_OK)
		return (status);
	if (p->offset + p->length > meminfo.size || p->offset + p->length < p->offset) {
		nvUvmInterfaceFreeDupedHandle(gpu->rm_device, dup);
		return (NV_ERR_INVALID_OFFSET);
	}
	if (!meminfo.sysmem && meminfo.deviceDescendant && !uvm_uuid_eq(&meminfo.uuid, &gpu->uuid)) {
		nvUvmInterfaceFreeDupedHandle(gpu->rm_device, dup);
		return (NV_ERR_INVALID_DEVICE);	/* peer memory: a later phase */
	}
	cap = UVM_PAGE_SIZE_2M;
	if (!meminfo.sysmem && gpu->fb.maxVidmemPageSize != 0 && gpu->fb.maxVidmemPageSize < cap)
		cap = gpu->fb.maxVidmemPageSize;
	page_size = uvm_ext_page_size(meminfo.pageSize != 0 ? meminfo.pageSize : UVM_PAGE_SIZE_4K, p->base,
	    p->length, p->offset, cap);
	if (page_size == 0) {
		nvUvmInterfaceFreeDupedHandle(gpu->rm_device, dup);
		return (NV_ERR_INVALID_ADDRESS);
	}
	s = uvm_ext_gpu_state(r, gpu, true);
	sx_xlock(&s->lock);
	uvm_ext_unmap_overlap(r, s, gvas, p->base, p->base + p->length - 1);
	m = malloc(sizeof(*m), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	m->start = p->base;
	m->end = p->base + p->length - 1;
	m->gpu = gpu;
	h = malloc(sizeof(*h), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	h->dup = dup;
	refcount_init(&h->refs, 1);
	m->handle = h;
	m->is_sysmem = meminfo.sysmem != 0;
	uvm_tracker_init(&m->tracker);
	status = uvm_pt_range_vec_init(gvas->tree, m->start, p->length, page_size, true, &m->ptes);
	if (status == NV_OK)
		status = uvm_ext_mapping_write(gvas, m, a, p->offset, p->length, page_size);
	if (status == NV_OK)
		status = uvm_tracker_wait(&m->tracker);
	if (status != NV_OK) {
		TAILQ_INSERT_TAIL(&s->mappings, m, link);
		uvm_ext_mapping_destroy(s, m, gvas);
		sx_xunlock(&s->lock);
		return (status);
	}
	TAILQ_INSERT_TAIL(&s->mappings, m, link);
	sx_xunlock(&s->lock);
	return (NV_OK);
}

NV_STATUS
uvm_vas_create_external_range(struct uvm_va_space *vas, uint64_t base, uint64_t length)
{
	struct uvm_range *r;
	NV_STATUS status;

	if (!uvm_interval_ok(base, length, UVM_PAGE_SIZE_4K))
		return (NV_ERR_INVALID_ADDRESS);
	sx_xlock(&vas->lock);
	status = uvm_range_create(vas, UVM_RANGE_EXTERNAL, base, base + length - 1, &r);
	sx_xunlock(&vas->lock);
	return (status);
}

/* the external range holding [base, base+length-1] whole, or NULL; caller holds the VA space lock */
static struct uvm_range *
uvm_ext_range_for(struct uvm_va_space *vas, uint64_t base, uint64_t length)
{
	struct uvm_range *r = uvm_range_find(vas, base);

	if (r == NULL || r->type != UVM_RANGE_EXTERNAL || r->core.end < base + length - 1)
		return (NULL);
	return (r);
}

NV_STATUS
uvm_vas_map_external_allocation(struct uvm_va_space *vas, UVM_MAP_EXTERNAL_ALLOCATION_PARAMS *p)
{
	struct uvm_range *r;
	NV_STATUS status = NV_OK;
	uint64_t i;

	if (p->gpuAttributesCount == 0 || p->gpuAttributesCount > UVM_MAX_GPUS)
		return (NV_ERR_INVALID_ARGUMENT);
	if (!uvm_interval_ok(p->base, p->length, UVM_PAGE_SIZE_4K))
		return (NV_ERR_INVALID_ADDRESS);
	sx_xlock(&vas->serialize);
	sx_slock(&vas->lock);
	r = uvm_ext_range_for(vas, p->base, p->length);
	if (r == NULL) {
		status = NV_ERR_INVALID_ADDRESS;
		goto out;
	}
	for (i = 0; i < p->gpuAttributesCount; i++) {
		status = uvm_ext_map_one(vas, r, &p->perGpuAttributes[i], p);
		if (status != NV_OK) {
			/* undo this call's mappings on the GPUs done so far */
			uint64_t j;

			for (j = 0; j < i; j++) {
				struct uvm_gpu *gpu = uvm_vas_gpu_locked(vas, &p->perGpuAttributes[j].gpuUuid);
				struct uvm_ext_gpu_state *s = gpu != NULL ? uvm_ext_gpu_state(r, gpu, false) : NULL;

				if (s != NULL) {
					sx_xlock(&s->lock);
					uvm_ext_unmap_overlap(r, s, vas->gvas[gpu->id], p->base, p->base + p->length - 1);
					sx_xunlock(&s->lock);
				}
			}
			break;
		}
	}
out:
	sx_sunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	return (status);
}

NV_STATUS
uvm_vas_map_external_sparse(struct uvm_va_space *vas, UVM_MAP_EXTERNAL_SPARSE_PARAMS *p)
{
	struct uvm_range *r;
	struct uvm_gpu *gpu;
	struct uvm_gpu_va_space *gvas;
	struct uvm_ext_gpu_state *s;
	struct uvm_ext_mapping *m;
	NV_STATUS status;

	if (!uvm_interval_ok(p->base, p->length, UVM_PAGE_SIZE_64K))
		return (NV_ERR_INVALID_ADDRESS);
	sx_xlock(&vas->serialize);
	sx_slock(&vas->lock);
	r = uvm_ext_range_for(vas, p->base, p->length);
	if (r == NULL) {
		status = NV_ERR_INVALID_ADDRESS;
		goto out;
	}
	gpu = uvm_vas_gpu_locked(vas, &p->gpuUuid);
	gvas = gpu != NULL ? vas->gvas[gpu->id] : NULL;
	if (gvas == NULL || gvas->dead) {
		status = NV_ERR_INVALID_DEVICE;
		goto out;
	}
	s = uvm_ext_gpu_state(r, gpu, true);
	sx_xlock(&s->lock);
	uvm_ext_unmap_overlap(r, s, gvas, p->base, p->base + p->length - 1);
	m = malloc(sizeof(*m), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	m->start = p->base;
	m->end = p->base + p->length - 1;
	m->gpu = gpu;
	uvm_tracker_init(&m->tracker);
	status = uvm_pt_range_vec_init(gvas->tree, m->start, p->length, UVM_PAGE_SIZE_64K, true, &m->ptes);
	if (status == NV_OK)
		status = uvm_pt_range_vec_write_same(gvas->tree, &m->ptes, uvm_page_tree_sparse_pte(gvas->tree),
		    UVM_MEMBAR_GPU, &m->tracker);
	if (status == NV_OK)
		status = uvm_tracker_wait(&m->tracker);
	TAILQ_INSERT_TAIL(&s->mappings, m, link);
	if (status != NV_OK)
		uvm_ext_mapping_destroy(s, m, gvas);
	sx_xunlock(&s->lock);
out:
	sx_sunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	return (status);
}

NV_STATUS
uvm_vas_unmap_external(struct uvm_va_space *vas, UVM_UNMAP_EXTERNAL_PARAMS *p)
{
	struct uvm_range *r;
	struct uvm_gpu *gpu;
	struct uvm_ext_gpu_state *s;
	NV_STATUS status = NV_OK;

	if (!uvm_interval_ok(p->base, p->length, UVM_PAGE_SIZE_4K))
		return (NV_ERR_INVALID_ADDRESS);
	sx_xlock(&vas->serialize);
	sx_slock(&vas->lock);
	r = uvm_ext_range_for(vas, p->base, p->length);
	if (r == NULL) {
		status = NV_ERR_INVALID_ADDRESS;
		goto out;
	}
	gpu = uvm_vas_gpu_locked(vas, &p->gpuUuid);
	if (gpu == NULL) {
		status = NV_ERR_INVALID_DEVICE;
		goto out;
	}
	s = uvm_ext_gpu_state(r, gpu, false);
	if (s != NULL) {
		sx_xlock(&s->lock);
		status = uvm_ext_unmap_overlap(r, s, vas->gvas[gpu->id], p->base, p->base + p->length - 1);
		sx_xunlock(&s->lock);
	}
out:
	sx_sunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	return (status);
}

/* remove every mapping of an external range on one GPU (unregister, free); VA space read-locked */
static void
uvm_ext_range_unmap_gpu(struct uvm_range *r, struct uvm_gpu *gpu)
{
	struct uvm_ext_gpu_state *s = uvm_ext_gpu_state(r, gpu, false);
	struct uvm_ext_mapping *m;

	if (s == NULL)
		return;
	sx_xlock(&s->lock);
	while ((m = TAILQ_FIRST(&s->mappings)) != NULL)
		uvm_ext_mapping_destroy(s, m, r->vas->gvas[gpu->id]);
	sx_xunlock(&s->lock);
	TAILQ_REMOVE(&r->u.ext.gpus, s, link);
	sx_destroy(&s->lock);
	free(s, M_NVIDIA_UVM);
}

static void
uvm_ext_range_destroy(struct uvm_range *r)
{
	struct uvm_ext_gpu_state *s;

	while ((s = TAILQ_FIRST(&r->u.ext.gpus)) != NULL)
		uvm_ext_range_unmap_gpu(r, s->gpu);
}

/* ---- dynamic-parallelism ranges (03 section 4.5) ---- */

NV_STATUS
uvm_vas_map_dynamic_parallelism(struct uvm_va_space *vas, UVM_MAP_DYNAMIC_PARALLELISM_REGION_PARAMS *p)
{
	struct uvm_range *r;
	struct uvm_gpu *gpu;
	struct uvm_gpu_va_space *gvas;
	struct uvm_tracker tracker;
	NV_STATUS status;
	uint64_t page_size = uvm_sked_page_size(p->base, p->length);

	if (page_size == 0 || !uvm_interval_ok(p->base, p->length, UVM_PAGE_SIZE_4K))
		return (NV_ERR_INVALID_ADDRESS);
	sx_xlock(&vas->serialize);
	sx_xlock(&vas->lock);
	gpu = uvm_vas_gpu_locked(vas, &p->gpuUuid);
	gvas = gpu != NULL ? vas->gvas[gpu->id] : NULL;
	if (gvas == NULL || gvas->dead) {
		status = NV_ERR_INVALID_DEVICE;
		goto out_write;
	}
	if (p->base + p->length - 1 >= gpu->max_channel_va) {
		status = NV_ERR_OUT_OF_RANGE;
		goto out_write;
	}
	status = uvm_range_create(vas, UVM_RANGE_SKED, p->base, p->base + p->length - 1, &r);
	if (status != NV_OK)
		goto out_write;
	r->u.sked.gpu = gpu;
	sx_downgrade(&vas->lock);
	uvm_tracker_init(&tracker);
	status = uvm_pt_range_vec_init(gvas->tree, p->base, p->length, page_size, true, &r->u.sked.ptes);
	if (status == NV_OK)
		status = uvm_pt_range_vec_write_same(gvas->tree, &r->u.sked.ptes, uvm_page_tree_sked_pte(gvas->tree),
		    UVM_MEMBAR_GPU, &tracker);
	if (status == NV_OK)
		status = uvm_tracker_wait(&tracker);
	uvm_tracker_deinit(&tracker);
	if (status != NV_OK) {
		uvm_pt_range_vec_deinit(gvas->tree, &r->u.sked.ptes);
		sx_sunlock(&vas->lock);
		sx_xlock(&vas->lock);
		uvm_core_range_remove(&vas->ranges, &r->core);
		free(r, M_NVIDIA_UVM);
		goto out_write;
	}
	sx_sunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	return (NV_OK);
out_write:
	sx_xunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	return (status);
}

static void
uvm_sked_range_destroy(struct uvm_range *r)
{
	struct uvm_gpu_va_space *gvas = r->vas->gvas[r->u.sked.gpu->id];
	struct uvm_tracker tracker;

	if (gvas != NULL && r->u.sked.ptes.ranges != NULL) {
		uvm_tracker_init(&tracker);
		(void)uvm_pt_range_vec_clear(gvas->tree, &r->u.sked.ptes, UVM_MEMBAR_GPU, &tracker);
		(void)uvm_tracker_wait(&tracker);
		uvm_tracker_deinit(&tracker);
		uvm_pt_range_vec_deinit(gvas->tree, &r->u.sked.ptes);
	}
}

/* ---- range destruction, free, validate ---- */

/* destroy a non-channel range; the VA space is held for read (RM and GPU work happen) */
static void
uvm_range_teardown(struct uvm_range *r)
{
	switch (r->type) {
	case UVM_RANGE_EXTERNAL:
		uvm_ext_range_destroy(r);
		break;
	case UVM_RANGE_SEMA_POOL:
	case UVM_RANGE_MANAGED:
		uvm_sema_pool_destroy(r);
		break;
	case UVM_RANGE_SKED:
		uvm_sked_range_destroy(r);
		break;
	default:
		break;
	}
}

void
uvm_range_destroy(struct uvm_range *r)
{
	uvm_range_teardown(r);
	sx_xlock(&r->vas->lock);
	uvm_core_range_remove(&r->vas->ranges, &r->core);
	sx_xunlock(&r->vas->lock);
	free(r, M_NVIDIA_UVM);
}

NV_STATUS
uvm_vas_free(struct uvm_va_space *vas, uint64_t base, uint64_t length)
{
	struct uvm_range *r;
	NV_STATUS status = NV_OK;

	if (length == 0 || base + length - 1 < base)
		return (NV_ERR_INVALID_ADDRESS);
	sx_xlock(&vas->serialize);
	sx_slock(&vas->lock);
	r = uvm_range_find(vas, base);
	if (r == NULL || r->core.start != base || r->core.end != base + length - 1 ||
	    (r->type != UVM_RANGE_EXTERNAL && r->type != UVM_RANGE_SEMA_POOL && r->type != UVM_RANGE_SKED)) {
		status = NV_ERR_INVALID_ADDRESS;
		sx_sunlock(&vas->lock);
		goto out;
	}
	uvm_range_teardown(r);
	sx_sunlock(&vas->lock);
	sx_xlock(&vas->lock);
	uvm_core_range_remove(&vas->ranges, &r->core);
	sx_xunlock(&vas->lock);
	free(r, M_NVIDIA_UVM);
out:
	sx_xunlock(&vas->serialize);
	return (status);
}

NV_STATUS
uvm_vas_validate(struct uvm_va_space *vas, uint64_t base, uint64_t length)
{
	struct uvm_range *r;
	NV_STATUS status;

	if (length == 0 || base + length - 1 < base)
		return (NV_ERR_INVALID_ADDRESS);
	sx_slock(&vas->lock);
	r = uvm_range_find(vas, base);
	status = (r != NULL && r->core.start == base && r->core.end == base + length - 1) ? NV_OK : NV_ERR_INVALID_ADDRESS;
	sx_sunlock(&vas->lock);
	return (status);
}

static bool
uvm_range_is_managed(const struct uvm_core_range *cr)
{
	return (__containerof(cr, struct uvm_range, core)->type == UVM_RANGE_MANAGED);
}

/*
 * The policy and migration ioctls need an interval fully covered by managed
 * ranges with no gap (01 section 5.7).  The policy itself is not kept yet:
 * every managed range is CPU resident and mapped on every GPU, which is the
 * state a preferred location of the CPU with the GPUs accessing it asks for.
 */
NV_STATUS
uvm_vas_check_managed_interval(struct uvm_va_space *vas, uint64_t base, uint64_t length)
{
	bool covered;

	if (!uvm_interval_ok(base, length, PAGE_SIZE))
		return (NV_ERR_INVALID_ADDRESS);
	sx_slock(&vas->lock);
	covered = uvm_core_range_covered(&vas->ranges, base, base + length - 1, uvm_range_is_managed);
	sx_sunlock(&vas->lock);
	return (covered ? NV_OK : NV_ERR_INVALID_ADDRESS);
}

/* ---- GPU VA spaces (03 section 3.1) ---- */

static void
uvm_gpu_va_space_free(struct uvm_gpu_va_space *gvas)
{
	if (gvas->page_dir_set) {
		(void)nvUvmInterfaceUnsetPageDirectory(gvas->rm_as);
		gvas->page_dir_set = false;
	}
	if (gvas->tree != NULL)
		uvm_page_tree_destroy(gvas->tree);
	if (gvas->rm_as != NULL)
		nvUvmInterfaceAddressSpaceDestroy(gvas->rm_as);
	free(gvas, M_NVIDIA_UVM);
}

/*
 * uvm_gvas_remove_locked - detach channels, unmap every range on the GPU,
 * mark dead and unlink; the VA space is write-locked.  The RM releases
 * (channels, page directory, address space) happen afterwards without the
 * lock through uvm_gpu_va_space_free and the channel references.
 */
static void
uvm_gvas_remove_locked(struct uvm_va_space *vas, struct uvm_gpu_va_space *gvas, struct uvm_user_channel **detached,
    unsigned *ndetached)
{
	struct uvm_user_channel *ch;
	struct uvm_range *r, *next;
	struct uvm_sema_gpu_map *m;

	while ((ch = TAILQ_FIRST(&gvas->channels)) != NULL) {
		uvm_user_channel_detach(ch);
		if (detached != NULL)
			detached[(*ndetached)++] = ch;
		else
			uvm_user_channel_unref(ch);
	}
	for (r = uvm_range_first(vas); r != NULL; r = next) {
		next = uvm_range_next(vas, r);
		switch (r->type) {
		case UVM_RANGE_EXTERNAL:
			uvm_ext_range_unmap_gpu(r, gvas->gpu);
			break;
		case UVM_RANGE_SEMA_POOL:
		case UVM_RANGE_MANAGED:
			TAILQ_FOREACH(m, &r->u.sema.gpus, link)
				if (m->gpu == gvas->gpu)
					break;
			if (m != NULL)
				uvm_sema_pool_unmap_gpu(r, gvas->gpu);
			break;
		case UVM_RANGE_SKED:
			if (r->u.sked.gpu == gvas->gpu) {
				uvm_sked_range_destroy(r);
				uvm_core_range_remove(&vas->ranges, &r->core);
				free(r, M_NVIDIA_UVM);
			}
			break;
		default:
			break;
		}
	}
	gvas->dead = true;
	vas->gvas[gvas->gpu->id] = NULL;
}

/*
 * uvm_vas_map_sema_pools - the semaphore pools of the VA space map
 * themselves on the new GPU VA space.
 * Time: O(ranges).  Space: O(1).
 */
static NV_STATUS
uvm_vas_map_sema_pools(struct uvm_va_space *vas, struct uvm_gpu_va_space *gvas)
{
	struct uvm_range *r;
	NV_STATUS status = NV_OK;

	for (r = uvm_range_first(vas); r != NULL; r = uvm_range_next(vas, r)) {
		if (r->type != UVM_RANGE_SEMA_POOL && r->type != UVM_RANGE_MANAGED)
			continue;
		status = uvm_sema_pool_map_gpu(r, gvas);
		if (status != NV_OK)
			break;
	}
	return (status);
}

NV_STATUS
uvm_vas_register_gpu_va_space(struct uvm_va_space *vas, UVM_REGISTER_GPU_VASPACE_PARAMS *p)
{
	struct uvm_gpu *gpu;
	struct uvm_gpu_va_space *gvas;
	NV_STATUS status;
	NvU64 dma = 0;
	uint32_t root_entries;

	sx_slock(&vas->lock);
	gpu = uvm_vas_gpu_locked(vas, &p->gpuUuid);
	if (gpu == NULL || vas->gvas[gpu->id] != NULL || uvm_gpu_mask_test(&vas->unregister_in_progress, gpu->id)) {
		sx_sunlock(&vas->lock);
		return (NV_ERR_INVALID_DEVICE);
	}
	if (vas->disallow_new_registers || vas->vm == NULL) {
		sx_sunlock(&vas->lock);
		return (NV_ERR_PAGE_TABLE_NOT_AVAIL);
	}
	sx_sunlock(&vas->lock);

	gvas = malloc(sizeof(*gvas), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	gvas->vas = vas;
	gvas->gpu = gpu;
	TAILQ_INIT(&gvas->channels);
	status = nvUvmInterfaceDupAddressSpace(gpu->rm_device, p->hClient, p->hVaSpace, &gvas->rm_as, &gvas->info);
	if (status != NV_OK) {
		free(gvas, M_NVIDIA_UVM);
		return (status);
	}
	if (gvas->info.atsEnabled || gvas->info.bigPageSize != UVM_PAGE_SIZE_64K) {
		uvm_gpu_va_space_free(gvas);
		return (NV_ERR_INVALID_FLAGS);
	}
	status = uvm_page_tree_create(gpu, true, &gvas->tree);
	if (status != NV_OK) {
		uvm_gpu_va_space_free(gvas);
		return (status);
	}

	/* claim the slot under the write lock, then hand the directory over (RM, no lock) */
	sx_xlock(&vas->lock);
	if (vas->gpus[gpu->id] != gpu || vas->gvas[gpu->id] != NULL ||
	    uvm_gpu_mask_test(&vas->unregister_in_progress, gpu->id) || vas->disallow_new_registers) {
		sx_xunlock(&vas->lock);
		uvm_gpu_va_space_free(gvas);
		return (NV_ERR_INVALID_DEVICE);
	}
	vas->gvas[gpu->id] = gvas;
	sx_xunlock(&vas->lock);

	root_entries = uvm_mmu_entries(gpu->mmu_version, 0, 0);
	status = nvUvmInterfaceSetPageDirectory(gvas->rm_as, pmap_kextract((vm_offset_t)gvas->tree->root->kva),
	    root_entries, NV_FALSE, 0xFFFFFFFFu, &dma);
	if (status != NV_OK) {
		if (status == NV_ERR_NOT_SUPPORTED)
			status = NV_ERR_INVALID_DEVICE;
		sx_xlock(&vas->lock);
		vas->gvas[gpu->id] = NULL;
		sx_xunlock(&vas->lock);
		uvm_gpu_va_space_free(gvas);
		return (status);
	}
	gvas->page_dir_set = true;
	uvm_page_tree_record_rm_root(gvas->tree);
	if (dma != uvm_page_tree_pdb(gvas->tree).phys)
		uvm_log("gpu%u: RM DMA-mapped the page directory at %#jx; UVM's bus address is %#jx", gpu->id,
		    (uintmax_t)dma, (uintmax_t)uvm_page_tree_pdb(gvas->tree).phys);

	/* existing semaphore pools map themselves on the new GPU VA space */
	sx_xlock(&vas->serialize);
	sx_slock(&vas->lock);
	status = uvm_vas_map_sema_pools(vas, gvas);
	sx_sunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	if (status != NV_OK) {
		(void)uvm_vas_unregister_gpu_va_space(vas, &p->gpuUuid);
		return (status);
	}
	uvm_log("gpu%u: GPU VA space registered for pid %d (RM client %#x, VA space %#x, %u subcontexts)", gpu->id,
	    vas->owner_pid, p->hClient, p->hVaSpace, gvas->info.maxSubctxCount);
	return (NV_OK);
}

NV_STATUS
uvm_vas_unregister_gpu_va_space(struct uvm_va_space *vas, const NvProcessorUuid *uuid)
{
	struct uvm_gpu *gpu;
	struct uvm_gpu_va_space *gvas;
	struct uvm_user_channel *ch, **detached;
	unsigned ndetached = 0, i, nch = 0;

	sx_xlock(&vas->serialize);
	sx_slock(&vas->lock);
	gpu = uvm_vas_gpu_locked(vas, uuid);
	gvas = gpu != NULL ? vas->gvas[gpu->id] : NULL;
	if (gvas == NULL || gvas->dead) {
		sx_sunlock(&vas->lock);
		sx_xunlock(&vas->serialize);
		return (NV_ERR_INVALID_DEVICE);
	}
	gvas->disallow_new_channels = true;
	TAILQ_FOREACH(ch, &gvas->channels, link) {
		uvm_user_channel_stop(ch);
		nch++;
	}
	sx_sunlock(&vas->lock);
	detached = malloc(sizeof(*detached) * (nch + 1), M_NVIDIA_UVM, M_WAITOK);
	sx_xlock(&vas->lock);
	if (!gvas->dead)
		uvm_gvas_remove_locked(vas, gvas, detached, &ndetached);
	sx_xunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	uvm_gpu_fault_buffer_flush(gpu);
	for (i = 0; i < ndetached; i++)
		uvm_user_channel_unref(detached[i]);
	free(detached, M_NVIDIA_UVM);
	uvm_gpu_va_space_free(gvas);
	return (NV_OK);
}

/* ---- GPU registration (03 section 3, 01 section 5.2) ---- */

NV_STATUS
uvm_vas_register_gpu(struct uvm_va_space *vas, UVM_REGISTER_GPU_PARAMS *p)
{
	struct uvm_gpu *gpu;
	struct uvm_core_device *dev;
	UvmGpuClientInfo client = { p->hClient, p->hSmcPartRef };
	NV_STATUS status;
	int error;

	status = uvm_gpu_retain_by_uuid_client(&p->gpu_uuid, &client, &gpu);
	if (status != NV_OK)
		return (status);
	/*
	 * Two references: the one above carries the caller's RM client
	 * handles (backend-side; a second vendor has none), the core's
	 * device reference below makes the GPU a device of the core.  The
	 * unregister path drops both.
	 */
	error = uvm_core_device_probe(&p->gpu_uuid, sizeof(p->gpu_uuid), &dev);
	if (error != 0) {
		uvm_gpu_release(gpu);
		return (uvm_nvidia_core_status(error));
	}
	sx_xlock(&vas->lock);
	if (vas->gpus[gpu->id] != NULL) {
		sx_xunlock(&vas->lock);
		uvm_core_device_release(dev);
		uvm_gpu_release(gpu);
		return (NV_ERR_INVALID_DEVICE);
	}
	if (vas->disallow_new_registers) {
		sx_xunlock(&vas->lock);
		uvm_core_device_release(dev);
		uvm_gpu_release(gpu);
		return (NV_ERR_PAGE_TABLE_NOT_AVAIL);
	}
	vas->gpus[gpu->id] = gpu;
	uvm_gpu_mask_set(&vas->registered, gpu->id);
	uvm_gpu_mask_set(&vas->sys_wide_atomics, gpu->id);
	sx_xunlock(&vas->lock);
	p->numaEnabled = NV_FALSE;
	p->numaNodeId = -1;
	return (NV_OK);
}

NV_STATUS
uvm_vas_unregister_gpu(struct uvm_va_space *vas, const NvProcessorUuid *uuid)
{
	struct uvm_gpu *gpu;
	struct uvm_gpu_va_space *gvas;
	struct uvm_range *r, *next;

	sx_xlock(&vas->lock);
	gpu = uvm_vas_gpu_locked(vas, uuid);
	if (gpu == NULL || uvm_gpu_mask_test(&vas->unregister_in_progress, gpu->id)) {
		sx_xunlock(&vas->lock);
		return (NV_ERR_INVALID_DEVICE);
	}
	uvm_gpu_mask_set(&vas->unregister_in_progress, gpu->id);
	sx_xunlock(&vas->lock);

	gvas = vas->gvas[gpu->id];
	if (gvas != NULL)
		(void)uvm_vas_unregister_gpu_va_space(vas, uuid);

	sx_xlock(&vas->serialize);
	sx_slock(&vas->lock);
	for (r = uvm_range_first(vas); r != NULL; r = next) {
		next = uvm_range_next(vas, r);
		if (r->type == UVM_RANGE_EXTERNAL)
			uvm_ext_range_unmap_gpu(r, gpu);
	}
	sx_sunlock(&vas->lock);
	sx_xlock(&vas->lock);
	vas->gpus[gpu->id] = NULL;
	uvm_gpu_mask_clear(&vas->registered, gpu->id);
	uvm_gpu_mask_clear(&vas->sys_wide_atomics, gpu->id);
	uvm_gpu_mask_clear(&vas->unregister_in_progress, gpu->id);
	sx_xunlock(&vas->lock);
	sx_xunlock(&vas->serialize);
	/* the core device reference the registration took, then the RM one */
	if (gpu->core_dev != NULL)
		uvm_core_device_release(gpu->core_dev);
	uvm_gpu_release(gpu);
	return (NV_OK);
}

NV_STATUS
uvm_vas_set_sys_wide_atomics(struct uvm_va_space *vas, const NvProcessorUuid *uuid, bool enable)
{
	struct uvm_gpu *gpu;

	sx_xlock(&vas->lock);
	gpu = uvm_vas_gpu_locked(vas, uuid);
	if (gpu == NULL) {
		sx_xunlock(&vas->lock);
		return (NV_ERR_INVALID_DEVICE);
	}
	/* bookkeeping only: no managed mappings exist whose atomic permission could change */
	if (enable)
		uvm_gpu_mask_set(&vas->sys_wide_atomics, gpu->id);
	else
		uvm_gpu_mask_clear(&vas->sys_wide_atomics, gpu->id);
	sx_xunlock(&vas->lock);
	return (NV_OK);
}

NV_STATUS
uvm_vas_pageable_on_gpu(struct uvm_va_space *vas, const NvProcessorUuid *uuid, NvBool *out)
{
	NV_STATUS status = uvm_vas_gpu_registered(vas, uuid, NULL);

	*out = NV_FALSE;	/* no HMM or ATS on this platform (11 sections 1, 2) */
	return (status);
}

/* ---- range groups (01 section 5.8): bookkeeping; no managed intervals exist yet ---- */

NV_STATUS
uvm_vas_create_range_group(struct uvm_va_space *vas, uint64_t *id)
{
	struct uvm_core_group *g;

	g = malloc(sizeof(*g), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	g->migratable = true;
	sx_xlock(&vas->lock);
	uvm_core_group_create(&vas->groups, g);
	sx_xunlock(&vas->lock);
	*id = g->id;
	return (NV_OK);
}

NV_STATUS
uvm_vas_destroy_range_group(struct uvm_va_space *vas, uint64_t id)
{
	struct uvm_core_group *g = NULL;
	int error;

	sx_xlock(&vas->lock);
	error = uvm_core_group_lookup(&vas->groups, id, &g);
	if (error == 0)
		error = uvm_core_group_remove(&vas->groups, g);
	sx_xunlock(&vas->lock);
	if (error != 0)
		return (NV_ERR_OBJECT_NOT_FOUND);
	free(g, M_NVIDIA_UVM);
	return (NV_OK);
}

NV_STATUS
uvm_vas_range_group_lookup(struct uvm_va_space *vas, uint64_t id)
{
	int error;

	sx_slock(&vas->lock);
	error = uvm_core_group_lookup(&vas->groups, id, NULL);
	sx_sunlock(&vas->lock);
	return (error == 0 ? NV_OK : NV_ERR_OBJECT_NOT_FOUND);
}

/* ---- the processor UUID table (01 section 5.11) ---- */

NV_STATUS
uvm_vas_processor_uuid_table(struct uvm_va_space *vas, NvU64 user_ptr, bool v2)
{
	unsigned entries = v2 ? UVM_MAX_GPUS + 1 : UVM_MAX_GPUS_V1 + 1;
	NvProcessorUuid *table;
	unsigned i;
	int error;

	table = malloc(sizeof(*table) * entries, M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	table[0] = NV_PROCESSOR_UUID_CPU_DEFAULT;
	sx_slock(&vas->lock);
	for (i = 1; i < entries; i++)
		if (vas->gpus[i] != NULL)
			table[i] = vas->gpus[i]->uuid;
	sx_sunlock(&vas->lock);
	error = copyout(table, (void *)(uintptr_t)user_ptr, sizeof(*table) * entries);
	free(table, M_NVIDIA_UVM);
	return (error == 0 ? NV_OK : NV_ERR_INVALID_ADDRESS);
}

/* ---- the bound memory map: MM fd binding and the mm shutdown (01 section 3.4, 03 section 2.1) ---- */

/*
 * uvm_va_space_mm_bind - UVM_MM_INITIALIZE on an uninitialised fd: take the
 * users reference on the bound map and mark the binding ALIVE.  NV_OK
 * means the caller's fd becomes an MM fd that owns the reference; it must
 * call uvm_va_space_mm_release when that fd is released.
 *
 * FreeBSD: vm_refcnt is the users count (vmspace_free drops it, the last
 * drop tears the map down).  The exit path closes the process's files
 * (fdescfree, kern_exit.c) before it releases the vmspace (vmspace_exit),
 * so the MM fd's release callback runs while the map still exists; our own
 * reference keeps it past that point even for a closer in another process.
 */
NV_STATUS
uvm_va_space_mm_bind(struct uvm_va_space *vas, enum uvm_mm_caller caller)
{
	NV_STATUS status;

	sx_xlock(&vas->lock);
	status = uvm_mm_initialize_decide(true, caller, vas->vm != NULL, vas->mm_state);
	if (status == NV_OK) {
		if (!refcount_acquire_if_not_zero(&vas->vm->vm_refcnt)) {
			status = NV_ERR_PAGE_TABLE_NOT_AVAIL;
		} else {
			vas->mm_users_vm = vas->vm;
			vas->mm_state = UVM_MM_ALIVE;
		}
	}
	sx_xunlock(&vas->lock);
	return (status);
}

/*
 * uvm_va_space_mm_shutdown - the mm shutdown of 03 section 2.1, in its
 * order: refuse new registrations; stop the user channels; detach them;
 * flush the replayable fault buffer of every GPU with a GPU VA space (so no
 * stale fault is attributed to a reused address); unset each page
 * directory; mark the binding RELEASED; release the detached channels.
 * After it no GPU issues accesses in this VA space's address spaces.  Runs
 * once: from the MM fd's release, or from destroy when no MM fd was used.
 * There are no retainers of the map yet (the fault servicing of managed
 * memory is the first), so there is nothing to wait for.
 */
static void
uvm_va_space_mm_shutdown(struct uvm_va_space *vas)
{
	struct uvm_user_channel *ch, **detached;
	unsigned nch = 0, ndetached = 0, i;

	sx_xlock(&vas->serialize);
	sx_xlock(&vas->lock);
	if (!uvm_mm_shutdown_needed(vas->mm_state)) {
		sx_xunlock(&vas->lock);
		sx_xunlock(&vas->serialize);
		return;
	}
	vas->disallow_new_registers = true;
	sx_downgrade(&vas->lock);
	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++) {
		if (vas->gvas[i] == NULL)
			continue;
		vas->gvas[i]->disallow_new_channels = true;
		TAILQ_FOREACH(ch, &vas->gvas[i]->channels, link) {
			uvm_user_channel_stop(ch);
			nch++;
		}
	}
	vas->user_channels_stopped = true;
	sx_sunlock(&vas->lock);

	detached = malloc(sizeof(*detached) * (nch + 1), M_NVIDIA_UVM, M_WAITOK);
	sx_xlock(&vas->lock);
	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++) {
		if (vas->gvas[i] == NULL)
			continue;
		while (ndetached < nch && (ch = TAILQ_FIRST(&vas->gvas[i]->channels)) != NULL) {
			uvm_user_channel_detach(ch);
			detached[ndetached++] = ch;
		}
	}
	sx_xunlock(&vas->lock);

	/* the serialisation lock keeps every GPU VA space in place across the RM calls */
	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++) {
		if (vas->gvas[i] != NULL && !vas->gvas[i]->dead)
			uvm_gpu_fault_buffer_flush(vas->gvas[i]->gpu);
	}
	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++) {
		struct uvm_gpu_va_space *gvas = vas->gvas[i];

		if (gvas != NULL && gvas->page_dir_set) {
			(void)nvUvmInterfaceUnsetPageDirectory(gvas->rm_as);
			gvas->page_dir_set = false;
		}
	}
	sx_xlock(&vas->lock);
	vas->mm_state = UVM_MM_RELEASED;
	sx_xunlock(&vas->lock);
	sx_xunlock(&vas->serialize);

	for (i = 0; i < ndetached; i++)
		uvm_user_channel_unref(detached[i]);
	free(detached, M_NVIDIA_UVM);
}

/* the MM fd is released: shut down, then give back the users reference */
void
uvm_va_space_mm_release(struct uvm_va_space *vas)
{
	struct vmspace *vm;

	uvm_va_space_mm_shutdown(vas);
	sx_xlock(&vas->lock);
	vm = vas->mm_users_vm;
	vas->mm_users_vm = NULL;
	sx_xunlock(&vas->lock);
	if (vm != NULL)
		vmspace_free(vm);
}

/* ---- destroy (03 section 2.2, 01 section 3.5) ---- */

void
uvm_va_space_destroy(struct uvm_va_space *vas)
{
	struct uvm_range *r, *next;
	struct uvm_core_group *g;
	int i;

	sx_xlock(&uvm.lock);
	TAILQ_REMOVE(&uvm.va_spaces, vas, link);
	sx_xunlock(&uvm.lock);

	/*
	 * Managed ranges: the pager objects that map them may outlive this VA
	 * space (at process exit the fd closes before the map is torn down).
	 * Detach them all under the managed lock, which a pager dtor holds
	 * while it destroys its range, so a range is torn down here or there
	 * and never both; the teardown loop below then owns every range.
	 */
	sx_xlock(&uvm.managed_lock);
	for (r = uvm_range_first(vas); r != NULL; r = uvm_range_next(vas, r)) {
		enum uvm_mobj_state next_state;
		struct uvm_managed_obj *obj;

		if (r->type != UVM_RANGE_MANAGED)
			continue;
		obj = r->u.sema.obj;
		KASSERT(obj != NULL && obj->range == r && obj->state == UVM_MOBJ_LINKED, ("managed range %p unlinked", r));
		if (uvm_mobj_event_decide(obj->state, UVM_MOBJ_VAS_DESTROY, &next_state) == UVM_MOBJ_DO_DETACH) {
			obj->range = NULL;
			obj->state = next_state;
			r->u.sema.obj = NULL;
		}
	}
	sx_xunlock(&uvm.managed_lock);

	/* the fd is closed: nothing else reaches this VA space except fault bottom halves via channels */
	uvm_va_space_mm_shutdown(vas);

	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++) {
		if (vas->gpus[i] != NULL)
			(void)uvm_vas_unregister_gpu(vas, &vas->gpus[i]->uuid);
	}
	for (r = uvm_range_first(vas); r != NULL; r = next) {
		next = uvm_range_next(vas, r);
		uvm_range_teardown(r);
		uvm_core_range_remove(&vas->ranges, &r->core);
		free(r, M_NVIDIA_UVM);
	}
	while ((g = LIST_FIRST(&vas->groups.list)) != NULL) {
		uvm_core_group_remove(&vas->groups, g);
		free(g, M_NVIDIA_UVM);
	}
	if (vas->vm != NULL)
		vmspace_free(vas->vm);
	sx_destroy(&vas->serialize);
	sx_destroy(&vas->lock);
	free(vas, M_NVIDIA_UVM);
}
