/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * Copyright (c) 2026 SkunkWerks, GmbH
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_gpu.c - the module-wide GPU object: creation on the first
 * registration of a UUID (the RM call sequence of uvm-spec r1,
 * 02-rm-interface.md section 4), reference counting, the instance-pointer
 * attribution table, DMA memory for the GPU, suspend and resume.
 *
 * Phase-1 shape.  The GPU's internal page tree (flat identity mappings for
 * copies by virtual address) is not created: on Blackwell the copy engine
 * addresses video and system memory physically (04 section 3), the
 * pushbuffer and semaphores are addressed through RM's own mappings in the
 * internal address space, and page tables are written physically.  RM's
 * page directory for the internal address space therefore stays in place.
 * The GPU memory manager (PMM) does not exist yet either: nothing in phase
 * 1 allocates dedicated memory, so the PMA eviction callbacks registered
 * here answer that they have nothing to give.  A GPU with unified memory
 * (the GB10 SoC: no dedicated memory, shared system memory) has no PMA at
 * all: RM answers NV_ERR_OBJECT_NOT_FOUND for the PMA object there,
 * gpu->pma stays NULL and no callback is registered (uvm_fb_state.h has
 * the rule).
 */

#include "uvm.h"

/* ---- DMA memory the GPU reads: page tables, semaphore pools ---- */

static void
uvm_dma_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	bus_addr_t *out = arg;

	*out = (error == 0 && nseg == 1) ? segs[0].ds_addr : 0;
}

/*
 * uvm_gpu_sysmem_alloc - `bytes` of zeroed, physically contiguous system
 * memory within the GPU's DMA window, with its bus address.
 */
NV_STATUS
uvm_gpu_sysmem_alloc(struct uvm_gpu *gpu, size_t bytes, void **kva, bus_dmamap_t *map, bus_addr_t *bus)
{
	int error;

	if (!uvm_sysmem_size_ok(bytes))
		return (NV_ERR_INVALID_ARGUMENT);
	error = bus_dmamem_alloc(gpu->dmat, kva, BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, map);
	if (error != 0)
		return (NV_ERR_NO_MEMORY);
	*bus = 0;
	error = bus_dmamap_load(gpu->dmat, *map, *kva, bytes, uvm_dma_cb, bus, BUS_DMA_NOWAIT);
	if (error != 0 || *bus == 0) {
		bus_dmamem_free(gpu->dmat, *kva, *map);
		*kva = NULL;
		return (NV_ERR_NO_MEMORY);
	}
	if (*bus + bytes - 1 > gpu->platform.dma_addressable_limit) {
		bus_dmamap_unload(gpu->dmat, *map);
		bus_dmamem_free(gpu->dmat, *kva, *map);
		*kva = NULL;
		return (NV_ERR_NO_MEMORY);
	}
	return (NV_OK);
}

void
uvm_gpu_sysmem_free(struct uvm_gpu *gpu, void *kva, bus_dmamap_t map, size_t bytes)
{
	(void)bytes;
	if (kva == NULL)
		return;
	bus_dmamap_unload(gpu->dmat, map);
	bus_dmamem_free(gpu->dmat, kva, map);
}

/* the address the GPU puts on the bus: the bus address minus RM's DMA window start (09 section 2.4) */
uint64_t
uvm_gpu_dma_address(const struct uvm_gpu *gpu, bus_addr_t bus)
{
	return ((uint64_t)bus - gpu->platform.dma_addressable_start);
}

/* ---- the instance pointer table (03 section 3.2) ---- */

void
uvm_gpu_instance_add(struct uvm_gpu *gpu, struct uvm_user_channel *ch)
{
	struct uvm_instance_entry *e;

	e = malloc(sizeof(*e), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	e->inst_phys = ch->inst_phys;
	e->aperture = ch->inst_aperture;
	e->channel = ch;
	mtx_lock(&gpu->instance_lock);
	TAILQ_INSERT_TAIL(&gpu->instances, e, link);
	ch->in_instance_table = true;
	mtx_unlock(&gpu->instance_lock);
}

void
uvm_gpu_instance_remove(struct uvm_gpu *gpu, struct uvm_user_channel *ch)
{
	struct uvm_instance_entry *e, *found = NULL;

	mtx_lock(&gpu->instance_lock);
	TAILQ_FOREACH(e, &gpu->instances, link) {
		if (e->channel == ch) {
			found = e;
			break;
		}
	}
	if (found != NULL)
		TAILQ_REMOVE(&gpu->instances, found, link);
	ch->in_instance_table = false;
	mtx_unlock(&gpu->instance_lock);
	free(found, M_NVIDIA_UVM);
}

/* the user channel behind an instance pointer, with a reference, or NULL for a stale entry */
struct uvm_user_channel *
uvm_gpu_instance_lookup(struct uvm_gpu *gpu, uint64_t inst_phys, uvm_aperture_t ap)
{
	struct uvm_instance_entry *e;
	struct uvm_user_channel *ch = NULL;

	mtx_lock(&gpu->instance_lock);
	TAILQ_FOREACH(e, &gpu->instances, link) {
		if (e->inst_phys == inst_phys && e->aperture == ap) {
			ch = uvm_user_channel_ref(e->channel);
			break;
		}
	}
	mtx_unlock(&gpu->instance_lock);
	return (ch);
}

/* ---- PMA eviction callbacks (02 section 3.9): no dedicated memory is held in phase 1 ---- */

static NV_STATUS
uvm_pma_evict_pages(void *data, NvU64 page_size, NvU64 *pages, NvU32 count, NvU64 phys_begin, NvU64 phys_end,
    NvU32 mem_type)
{
	(void)data; (void)page_size; (void)pages; (void)count; (void)phys_begin; (void)phys_end; (void)mem_type;
	return (NV_ERR_NO_MEMORY);
}

static NV_STATUS
uvm_pma_evict_range(void *data, NvU64 phys_begin, NvU64 phys_end, NvU32 mem_type)
{
	(void)data; (void)phys_begin; (void)phys_end; (void)mem_type;
	return (NV_OK);	/* nothing of ours lies in any range */
}

/* ---- GPU lookup ---- */

struct uvm_gpu *
uvm_gpu_lookup_uuid_locked(const NvProcessorUuid *uuid)
{
	int i;

	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++)
		if (uvm.gpus[i] != NULL && uvm_uuid_eq(&uvm.gpus[i]->uuid, uuid))
			return (uvm.gpus[i]);
	return (NULL);
}

/* ---- GPU add and remove (02 section 4) ---- */

static void uvm_gpu_remove(struct uvm_gpu *gpu);

static NV_STATUS
uvm_gpu_check_info(struct uvm_gpu *gpu)
{
	const UvmGpuInfo *info = &gpu->info;

	gpu->gen = uvm_hal_generation_by_arch(info->gpuArch);
	if (gpu->gen == NULL) {
		uvm_log("%s: architecture %#x is not in the HAL table", gpu->name, info->gpuArch);
		return (NV_ERR_NOT_SUPPORTED);
	}
	if (!gpu->gen->supported) {
		uvm_log("%s: %s (arch %#x) has no HAL in this build yet; only Blackwell GB20x is driven",
		    gpu->name, gpu->gen->name, info->gpuArch);
		return (NV_ERR_NOT_SUPPORTED);
	}
	if (!uvm_hal_classes_supported(info->gpuArch, info->hostClass, info->ceClass)) {
		uvm_log("%s: host class %#x / copy class %#x not supported for %s", gpu->name, info->hostClass,
		    info->ceClass, gpu->gen->name);
		return (NV_ERR_NOT_SUPPORTED);
	}
	if (info->subdeviceCount != 1) {
		uvm_log("%s: SLI (%u subdevices) is not supported", gpu->name, info->subdeviceCount);
		return (NV_ERR_NOT_SUPPORTED);
	}
	if (info->virtMode != UVM_VIRT_MODE_NONE) {
		uvm_log("%s: virtualisation mode %u is not supported", gpu->name, info->virtMode);
		return (NV_ERR_NOT_SUPPORTED);
	}
	if (info->gpuConfComputeCaps.bConfComputingEnabled) {
		uvm_log("%s: confidential computing mode is not supported", gpu->name);
		return (NV_ERR_NOT_SUPPORTED);
	}
	if (info->smcEnabled) {
		uvm_log("%s: MIG partitions are not supported", gpu->name);
		return (NV_ERR_NOT_SUPPORTED);
	}
	gpu->mmu_version = gpu->gen->mmu_version;
	gpu->max_channel_va = gpu->gen->max_channel_va;
	return (NV_OK);
}

/*
 * uvm_gpu_add - create the module-wide GPU object for a UUID: the RM
 * sequence of 02 section 4 in order, undone in reverse on any failure.
 * Caller holds uvm.lock exclusively.
 */
/* #lizard forgives -- the RM registration sequence of 02 section 4 is one ordered list of calls, each with its own failure exit; splitting it would separate a step from its unwinding */
static NV_STATUS
uvm_gpu_add(const NvProcessorUuid *uuid, const UvmGpuClientInfo *client, struct uvm_gpu **out)
{
	struct uvm_gpu *gpu;
	NV_STATUS status;
	struct uvm_fb_pma_verdict pma_verdict;
	int i, error;

	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++)
		if (uvm.gpus[i] == NULL)
			break;
	if (i > UVM_ID_GPU_LAST)
		return (NV_ERR_INSUFFICIENT_RESOURCES);

	gpu = malloc(sizeof(*gpu), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	gpu->id = (uvm_gpu_id_t)i;
	gpu->uuid = *uuid;
	gpu->refs = 1;
	mtx_init(&gpu->isr_lock, "nvidia_uvm_isr", NULL, MTX_DEF);
	mtx_init(&gpu->instance_lock, "nvidia_uvm_instances", NULL, MTX_DEF);
	TAILQ_INIT(&gpu->instances);
	uvm_gpu_fault_buffers_init(gpu);

	/* 1. the glue: find the device, reference it, get the DMA window */
	status = nvUvmInterfaceRegisterGpu(uuid, &gpu->platform);
	if (status != NV_OK) {
		uvm_log_slow("GPU lookup by UUID failed: %s", uvm_status_name(status));
		goto fail_free;
	}
	/* 2. describe it, select the HAL */
	status = nvUvmInterfaceGetGpuInfo(uuid, client, &gpu->info);
	if (status != NV_OK) {
		uvm_log("GetGpuInfo failed: %s", uvm_status_name(status));
		goto fail_unregister;
	}
	strlcpy(gpu->name, gpu->info.name, sizeof(gpu->name));
	status = uvm_gpu_check_info(gpu);
	if (status != NV_OK)
		goto fail_unregister;

	error = bus_dma_tag_create(bus_get_dma_tag((device_t)gpu->platform.dev), PAGE_SIZE, 0,
	    gpu->platform.dma_addressable_limit, BUS_SPACE_MAXADDR, NULL, NULL, UVM_GPU_DMA_MAXSIZE, 1, UVM_GPU_DMA_MAXSIZE, 0,
	    NULL, NULL, &gpu->dmat);
	if (error != 0) {
		uvm_log("%s: DMA tag creation failed: %d", gpu->name, error);
		status = NV_ERR_NO_MEMORY;
		goto fail_unregister;
	}
	/* 3. the RM device */
	status = nvUvmInterfaceDeviceCreate(uvm.rm_session, &gpu->info, uuid, &gpu->rm_device, NV_FALSE);
	if (status != NV_OK) {
		uvm_log("%s: DeviceCreate failed: %s", gpu->name, uvm_status_name(status));
		goto fail_dmat;
	}
	/* 4. frame buffer geometry */
	status = nvUvmInterfaceGetFbInfo(gpu->rm_device, &gpu->fb);
	gpu->fb_state = uvm_fb_info_state(status, &gpu->fb);
	if (gpu->fb_state == UVM_FB_BROKEN) {
		/*
		 * The frame buffer information is broken or missing.  That is not
		 * the same as a GPU with unified memory: a missing PMA object
		 * would be normal there, so the registration fails with a message
		 * instead of pretending the GPU has unified memory.
		 */
		if (status != NV_OK)
			uvm_log("%s: the frame buffer information is missing (GetFbInfo failed: %s)",
			    gpu->name, uvm_status_name(status));
		else
			uvm_log("%s: the frame buffer is broken (RM reports bZeroFb with %u KB of heap)",
			    gpu->name, gpu->fb.heapSize);
		goto fail_device;
	}
	/* 5. fault buffers and interrupt ownership */
	status = nvUvmInterfaceInitFaultInfo(gpu->rm_device, &gpu->fault_info);
	if (status != NV_OK) {
		uvm_log("%s: InitFaultInfo failed: %s", gpu->name, uvm_status_name(status));
		goto fail_device;
	}
	gpu->fault_info_valid = true;
	status = nvUvmInterfaceOwnPageFaultIntr(gpu->rm_device, NV_TRUE);
	if (status != NV_OK) {
		uvm_log("%s: OwnPageFaultIntr failed: %s", gpu->name, uvm_status_name(status));
		goto fail_faultinfo;
	}
	gpu->owns_fault_intr = true;
	/* 6. capabilities */
	status = nvUvmInterfaceQueryCaps(gpu->rm_device, &gpu->caps);
	if (status != NV_OK) {
		uvm_log("%s: QueryCaps failed: %s", gpu->name, uvm_status_name(status));
		goto fail_intr;
	}
	/* 7. the internal address space */
	status = nvUvmInterfaceAddressSpaceCreate(gpu->rm_device, 0, gpu->gen->rm_va_size, NV_FALSE, &gpu->rm_as,
	    &gpu->as_info);
	if (status != NV_OK) {
		uvm_log("%s: AddressSpaceCreate failed: %s", gpu->name, uvm_status_name(status));
		goto fail_intr;
	}
	if (gpu->as_info.bigPageSize != UVM_PAGE_SIZE_64K) {
		uvm_log("%s: big page size %ju is not 64K", gpu->name, (uintmax_t)gpu->as_info.bigPageSize);
		status = NV_ERR_INVALID_FLAGS;
		goto fail_as;
	}
	/* 8. ECC and NVLink state, 9. the physical memory allocator */
	status = nvUvmInterfaceGetEccInfo(gpu->rm_device, &gpu->ecc);
	if (status != NV_OK) {
		uvm_log("%s: GetEccInfo failed: %s", gpu->name, uvm_status_name(status));
		goto fail_as;
	}
	status = nvUvmInterfaceGetNvlinkInfo(gpu->rm_device, &gpu->nvlink);
	if (status != NV_OK) {
		uvm_log("%s: GetNvlinkInfo failed: %s", gpu->name, uvm_status_name(status));
		goto fail_as;
	}
	status = nvUvmInterfaceGetPmaObject(gpu->rm_device, &gpu->pma, &gpu->pma_stats);
	pma_verdict = uvm_fb_pma_judge(gpu->fb_state, status);
	if (pma_verdict.decision == UVM_FB_PMA_FAIL) {
		uvm_log("%s: GetPmaObject failed: %s (%s)", gpu->name, uvm_status_name(status),
		    pma_verdict.reason);
		goto fail_as;
	}
	if (pma_verdict.decision == UVM_FB_PMA_UNIFIED) {
		/* unified memory: RM has no PMA: nothing to evict from, no callbacks */
		gpu->pma = NULL;
		gpu->pma_stats = NULL;
		uvm_log("%s: %s; all GPU memory is system memory", gpu->name, pma_verdict.reason);
	} else if (gpu->pma == NULL) {
		/* the RM answered success but gave no object: that is not a PMA to use */
		uvm_log("%s: GetPmaObject answered %s but gave no PMA object", gpu->name,
		    uvm_status_name(status));
		goto fail_as;
	} else {
		status = nvUvmInterfacePmaRegisterEvictionCallbacks(gpu->pma, uvm_pma_evict_pages, uvm_pma_evict_range,
		    gpu);
		if (status != NV_OK) {
			uvm_log("%s: PMA eviction callback registration failed: %s", gpu->name,
			    uvm_status_name(status));
			goto fail_as;
		}
		gpu->pma_callbacks_registered = true;
	}
	/* 10-11. copy engines and the channel manager (which owns the semaphore page) */
	status = nvUvmInterfaceQueryCopyEnginesCaps(gpu->rm_device, &gpu->ce_caps);
	if (status != NV_OK) {
		uvm_log("%s: QueryCopyEnginesCaps failed: %s", gpu->name, uvm_status_name(status));
		goto fail_pma;
	}
	if (uvm_channel_manager_create(gpu) != 0) {
		status = NV_ERR_INSUFFICIENT_RESOURCES;
		goto fail_pma;
	}
	/* bottom-half threads, then the table (the top half must find the GPU), then interrupts */
	status = uvm_gpu_fault_buffers_start(gpu);
	if (status != NV_OK)
		goto fail_channels;
	mtx_lock_spin(&uvm.gpu_table_lock);
	uvm.gpus[gpu->id] = gpu;
	gpu->in_table = true;
	mtx_unlock_spin(&uvm.gpu_table_lock);
	uvm_gpu_replayable_intr_enable(gpu);

	/*
	 * The kind of memory the GPU has, in its own words: a GPU with
	 * unified memory has no dedicated memory and uses shared system
	 * memory, a GPU with dedicated memory has memory of its own.
	 */
	if (gpu->fb_state == UVM_FB_UNIFIED)
		uvm_log("%s: unified memory (no dedicated memory); GPU memory is shared system memory",
		    gpu->name);
	else
		uvm_log("%s: %u MB of dedicated memory (frame buffer %s)", gpu->name,
		    (gpu->fb.heapSize + gpu->fb.reservedHeapSize) / 1024,
		    uvm_fb_state_name(gpu->fb_state));
	uvm_log("gpu%u: %s registered: arch %#x impl %#x host %#x ce %#x, MMU v%u, "
	    "ECC %s, link %u, DMA window %#jx-%#jx", gpu->id, gpu->name, gpu->info.gpuArch,
	    gpu->info.gpuImplementation, gpu->info.hostClass, gpu->info.ceClass, gpu->mmu_version,
	    gpu->ecc.bEccEnabled ? "on" : "off", gpu->info.sysmemLink,
	    (uintmax_t)gpu->platform.dma_addressable_start,
	    (uintmax_t)gpu->platform.dma_addressable_limit);
	*out = gpu;
	return (NV_OK);

fail_channels:
	uvm_channel_manager_destroy(gpu);
fail_pma:
	if (gpu->pma_callbacks_registered) {
		nvUvmInterfacePmaUnregisterEvictionCallbacks(gpu->pma);
		gpu->pma_callbacks_registered = false;
	}
fail_as:
	nvUvmInterfaceAddressSpaceDestroy(gpu->rm_as);
	gpu->rm_as = NULL;
fail_intr:
	nvUvmInterfaceOwnPageFaultIntr(gpu->rm_device, NV_FALSE);
	gpu->owns_fault_intr = false;
fail_faultinfo:
	nvUvmInterfaceDestroyFaultInfo(gpu->rm_device, &gpu->fault_info);
	gpu->fault_info_valid = false;
fail_device:
	nvUvmInterfaceDeviceDestroy(gpu->rm_device);
	gpu->rm_device = NULL;
fail_dmat:
	bus_dma_tag_destroy(gpu->dmat);
fail_unregister:
	nvUvmInterfaceUnregisterGpu(uuid);
fail_free:
	uvm_gpu_fault_buffers_stop(gpu);
	mtx_destroy(&gpu->instance_lock);
	mtx_destroy(&gpu->isr_lock);
	free(gpu, M_NVIDIA_UVM);
	return (status);
}

/* the exact reverse of uvm_gpu_add (02 section 4); caller holds uvm.lock exclusively */
static void
uvm_gpu_remove(struct uvm_gpu *gpu)
{
	struct uvm_instance_entry *e;

	/* interrupts for it are ignored first: out of the table, bit disabled, bottom halves drained */
	mtx_lock_spin(&uvm.gpu_table_lock);
	uvm.gpus[gpu->id] = NULL;
	gpu->in_table = false;
	mtx_unlock_spin(&uvm.gpu_table_lock);
	uvm_gpu_replayable_intr_disable(gpu);
	uvm_gpu_fault_buffers_stop(gpu);

	uvm_channel_manager_destroy(gpu);
	if (gpu->pma_callbacks_registered)
		nvUvmInterfacePmaUnregisterEvictionCallbacks(gpu->pma);
	if (gpu->rm_as != NULL)
		nvUvmInterfaceAddressSpaceDestroy(gpu->rm_as);
	if (gpu->owns_fault_intr)
		nvUvmInterfaceOwnPageFaultIntr(gpu->rm_device, NV_FALSE);
	if (gpu->fault_info_valid)
		nvUvmInterfaceDestroyFaultInfo(gpu->rm_device, &gpu->fault_info);
	if (gpu->rm_device != NULL)
		nvUvmInterfaceDeviceDestroy(gpu->rm_device);
	bus_dma_tag_destroy(gpu->dmat);
	nvUvmInterfaceUnregisterGpu(&gpu->uuid);
	while ((e = TAILQ_FIRST(&gpu->instances)) != NULL) {
		TAILQ_REMOVE(&gpu->instances, e, link);
		free(e, M_NVIDIA_UVM);
	}
	uvm_log("gpu%u: %s released", gpu->id, gpu->name);
	mtx_destroy(&gpu->instance_lock);
	mtx_destroy(&gpu->isr_lock);
	free(gpu, M_NVIDIA_UVM);
}

/*
 * uvm_gpu_retain_by_uuid - the GPU object for a UUID, created on the first
 * registration module-wide; the caller owns one reference.  `client` may be
 * NULL when the caller is not a user registration.
 */
NV_STATUS
uvm_gpu_retain_by_uuid_client(const NvProcessorUuid *uuid, const UvmGpuClientInfo *client, struct uvm_gpu **out)
{
	struct uvm_gpu *gpu;
	UvmGpuClientInfo none = { 0, 0 };
	NV_STATUS status = NV_OK;

	sx_xlock(&uvm.lock);
	gpu = uvm_gpu_lookup_uuid_locked(uuid);
	if (gpu != NULL) {
		gpu->refs++;
		*out = gpu;
	} else {
		status = uvm_gpu_add(uuid, client != NULL ? client : &none, out);
	}
	sx_xunlock(&uvm.lock);
	return (status);
}

NV_STATUS
uvm_gpu_retain_by_uuid(const NvProcessorUuid *uuid, struct uvm_gpu **out)
{
	return (uvm_gpu_retain_by_uuid_client(uuid, NULL, out));
}

void
uvm_gpu_release(struct uvm_gpu *gpu)
{
	sx_xlock(&uvm.lock);
	if (--gpu->refs == 0)
		uvm_gpu_remove(gpu);
	sx_xunlock(&uvm.lock);
}

/* ---- the interrupt top half entry (02 section 5) ---- */

NV_STATUS
uvm_gpu_isr_top_half(const NvProcessorUuid *uuid)
{
	struct uvm_gpu *gpu = NULL;
	int i;

	mtx_lock_spin(&uvm.gpu_table_lock);
	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++) {
		if (uvm.gpus[i] != NULL && uvm_uuid_eq(&uvm.gpus[i]->uuid, uuid)) {
			gpu = uvm.gpus[i];
			break;
		}
	}
	mtx_unlock_spin(&uvm.gpu_table_lock);
	if (gpu == NULL)
		return (NV_ERR_NO_INTR_PENDING);
	gpu->isr_top_halves++;
	return (uvm_gpu_fault_top_half(gpu));
}

/* ---- system sleep (02 section 5): phase 1 vetoes while any GPU is in use ---- */

NV_STATUS
uvm_gpu_suspend_all(void)
{
	int i;

	sx_slock(&uvm.lock);
	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++) {
		if (uvm.gpus[i] != NULL) {
			sx_sunlock(&uvm.lock);
			uvm_log("suspend refused: gpu%d is registered with a VA space (suspend/resume is a later phase)",
			    i);
			return (NV_ERR_NOT_SUPPORTED);
		}
	}
	uvm.suspended = true;
	sx_sunlock(&uvm.lock);
	return (NV_OK);
}

NV_STATUS
uvm_gpu_resume_all(void)
{
	uvm.suspended = false;
	return (NV_OK);
}
