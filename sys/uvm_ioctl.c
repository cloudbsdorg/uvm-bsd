/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_ioctl.c - the ioctl dispatcher, the fd state machine and one handler
 * per request.  uvm-spec r1, 01-user-abi.md sections 2 (conventions), 3
 * (fd states), 5 (requests) and 7 (the tools node); 13 "Phase 1" for what
 * answers in full and what reports its absence.
 *
 * Dispatch (01 section 2.2): copy the whole structure in (EFAULT), preset
 * rmStatus to the module's global status, refuse a VA-space request on an
 * fd that has no VA space with NV_ERR_ILLEGAL_ACTION, run the handler,
 * copy the whole structure out (EFAULT), return 0.  Every request first
 * tries the power-management lock for read; EAGAIN when it cannot.  An
 * unknown number is ENOSYS on the main node and EINVAL on the tools node.
 *
 * The status is written at the ABI's rmStatus offset
 * (UVM_RM_STATUS_OFFSET), not at the end of the structure: a structure
 * with tail padding carries rmStatus before that padding, and the user
 * (libcuda, the test harness) reads the field where the specification
 * puts it.
 *
 * Requests whose feature is a later phase answer as the spec says for the
 * state that exists: policy and migration requests find no managed range
 * and return NV_ERR_INVALID_ADDRESS; peer access, device P2P and the tools
 * trackers return NV_ERR_NOT_SUPPORTED, each with a rate-limited log line
 * naming the phase, never silently.
 */

#include "uvm.h"

#include <sys/uio.h>
#include <sys/capsicum.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/vnode.h>
#include <vm/vm_object.h>
#include <fs/devfs/devfs_int.h>

typedef NV_STATUS uvm_handler_t(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td);

struct uvm_request {
	uint32_t	number;
	const char	*name;
	size_t		size;
	size_t		rm_status_offset;	/* where rmStatus sits in the structure */
	bool		needs_va_space;
	bool		tools_node;	/* accepted on /dev/nvidia-uvm-tools */
	uvm_handler_t	*handler;
};

#define	UVM_NOT_YET(what, phase)						\
	uvm_log_ratelimited("%s is not available yet (%s); answering NV_ERR_NOT_SUPPORTED", what, phase)

/*
 * CPD-OFF -- one adapter per request between the table's uniform handler
 * signature and the VA space operation it names, and then the table
 * itself: the repetition is the dispatch, not a copy to factor.
 */

/* ---- 5.1 initialisation family ---- */

static NV_STATUS
uvm_h_initialize(struct uvm_fd *fd, struct uvm_va_space *unused, void *params, struct thread *td)
{
	UVM_INITIALIZE_PARAMS *p = params;
	struct uvm_va_space *vas, *existing;
	NV_STATUS status;
	uintptr_t state;

	(void)unused; (void)td;
	/* exactly one thread creates; the compare-and-swap decides who */
	if (!atomic_cmpset_ptr(&fd->state, 0, UVM_FD_INITIALIZING)) {
		state = atomic_load_acq_ptr(&fd->state);
		if (state == UVM_FD_INITIALIZING)
			return (NV_ERR_BUSY_RETRY);
		if ((state & UVM_FD_TAG_MM) != 0)
			return (NV_ERR_INVALID_ARGUMENT);
		existing = uvm_fd_va_space(fd);
		return (existing != NULL && existing->init_flags == p->flags ? NV_OK : NV_ERR_INVALID_ARGUMENT);
	}
	status = uvm_va_space_create(p->flags, &vas);
	if (status != NV_OK) {
		atomic_store_rel_ptr(&fd->state, 0);
		return (status);
	}
	atomic_store_rel_ptr(&fd->state, (uintptr_t)vas);
	return (NV_OK);
}

static NV_STATUS
uvm_h_mm_initialize(struct uvm_fd *fd, struct uvm_va_space *unused, void *params, struct thread *td)
{
	UVM_MM_INITIALIZE_PARAMS *p = params;
	struct file *fp;
	struct uvm_fd *other;
	struct uvm_va_space *vas;
	cap_rights_t rights;
	int error;

	(void)unused;
	/* the target must be one of our fds in the VA-space state */
	error = fget(td, p->uvmFd, cap_rights_init_one(&rights, CAP_IOCTL), &fp);
	if (error != 0)
		return (NV_ERR_INVALID_ARGUMENT);
	if (fp->f_type != DTYPE_VNODE || fp->f_vnode == NULL || fp->f_vnode->v_type != VCHR ||
	    fp->f_vnode->v_rdev != uvm.cdev_main) {
		fdrop(fp, td);
		return (NV_ERR_INVALID_ARGUMENT);
	}
	/* the other file's per-open state, as devfs keeps it for its own d_ioctl */
	other = fp->f_cdevpriv != NULL ? fp->f_cdevpriv->cdpd_data : NULL;
	vas = other != NULL ? uvm_fd_va_space(other) : NULL;
	fdrop(fp, td);
	if (vas == NULL)
		return (NV_ERR_INVALID_ARGUMENT);
	/*
	 * Design choice allowed by 01 section 3.4: this phase has no managed
	 * memory whose CPU pages the GPU could touch during process exit, so
	 * the secondary fd binds nothing and the user may close it.
	 */
	(void)fd;
	return (NV_WARN_NOTHING_TO_DO);
}

static NV_STATUS
uvm_h_pageable_mem_access(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_PAGEABLE_MEM_ACCESS_PARAMS *p = params;

	(void)fd; (void)vas; (void)td;
	p->pageableMemAccess = NV_FALSE;	/* no HMM, no ATS (11 sections 1 and 2) */
	return (NV_OK);
}

static NV_STATUS
uvm_h_pageable_mem_access_on_gpu(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_PAGEABLE_MEM_ACCESS_ON_GPU_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_pageable_on_gpu(vas, &p->gpu_uuid, &p->pageableMemAccess));
}

/* ---- 5.2 - 5.4 registration ---- */

static NV_STATUS
uvm_h_register_gpu(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)td;
	return (uvm_vas_register_gpu(vas, params));
}

static NV_STATUS
uvm_h_unregister_gpu(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_UNREGISTER_GPU_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_unregister_gpu(vas, &p->gpu_uuid));
}

static NV_STATUS
uvm_h_register_gpu_vaspace(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)td;
	return (uvm_vas_register_gpu_va_space(vas, params));
}

static NV_STATUS
uvm_h_unregister_gpu_vaspace(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_UNREGISTER_GPU_VASPACE_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_unregister_gpu_va_space(vas, &p->gpuUuid));
}

static NV_STATUS
uvm_h_register_channel(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)td;
	return (uvm_vas_register_channel(vas, params));
}

static NV_STATUS
uvm_h_unregister_channel(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)td;
	return (uvm_vas_unregister_channel(vas, params));
}

/* ---- 5.5 - 5.6 external ranges, semaphore pools ---- */

static NV_STATUS
uvm_h_create_external_range(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_CREATE_EXTERNAL_RANGE_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_create_external_range(vas, p->base, p->length));
}

static NV_STATUS
uvm_h_map_external_allocation(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)td;
	return (uvm_vas_map_external_allocation(vas, params));
}

static NV_STATUS
uvm_h_map_external_sparse(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)td;
	return (uvm_vas_map_external_sparse(vas, params));
}

static NV_STATUS
uvm_h_unmap_external(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)td;
	return (uvm_vas_unmap_external(vas, params));
}

static NV_STATUS
uvm_h_free(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_FREE_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_free(vas, p->base, p->length));
}

static NV_STATUS
uvm_h_validate_va_range(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_VALIDATE_VA_RANGE_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_validate(vas, p->base, p->length));
}

static NV_STATUS
uvm_h_map_dynamic_parallelism(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)td;
	return (uvm_vas_map_dynamic_parallelism(vas, params));
}

static NV_STATUS
uvm_h_alloc_semaphore_pool(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)td;
	return (uvm_vas_alloc_semaphore_pool(vas, params));
}

/* ---- 5.7 policy and migration: no managed range can exist in this phase ---- */

static NV_STATUS
uvm_h_managed_interval(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	const NvU64 *p = params;	/* every one of these starts with base @0 and length @8 */

	(void)fd; (void)td;
	return (uvm_vas_check_managed_interval(vas, p[0], p[1]));
}

static NV_STATUS
uvm_h_set_preferred_location(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_SET_PREFERRED_LOCATION_PARAMS *p = params;
	NV_STATUS status;

	(void)fd; (void)td;
	if (!uvm_uuid_eq(&p->preferredLocation, &NV_PROCESSOR_UUID_CPU_DEFAULT)) {
		status = uvm_vas_gpu_registered(vas, &p->preferredLocation, NULL);
		if (status != NV_OK)
			return (status);
	} else if (p->preferredCpuNumaNode != -1 && p->preferredCpuNumaNode != 0) {
		return (NV_ERR_INVALID_ARGUMENT);
	}
	return (uvm_vas_check_managed_interval(vas, p->requestedBase, p->length));
}

static NV_STATUS
uvm_h_accessed_by(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_SET_ACCESSED_BY_PARAMS *p = params;
	NV_STATUS status;

	(void)fd; (void)td;
	if (!uvm_uuid_eq(&p->accessedByUuid, &NV_PROCESSOR_UUID_CPU_DEFAULT)) {
		status = uvm_vas_gpu_registered(vas, &p->accessedByUuid, NULL);
		if (status != NV_OK)
			return (status);
	}
	return (uvm_vas_check_managed_interval(vas, p->requestedBase, p->length));
}

static NV_STATUS
uvm_h_migrate(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_MIGRATE_PARAMS *p = params;
	NV_STATUS status;

	(void)fd; (void)td;
	if ((p->flags & ~UVM_MIGRATE_FLAG_ASYNC) != 0)
		return (NV_ERR_INVALID_ARGUMENT);	/* test-only bits need the built-in tests */
	if ((p->flags & UVM_MIGRATE_FLAG_ASYNC) == 0) {
		if (p->semaphoreAddress != 0)
			return (NV_ERR_INVALID_ARGUMENT);
	} else if (p->semaphoreAddress == 0 && p->semaphorePayload != 0) {
		return (NV_ERR_INVALID_ARGUMENT);
	}
	if (!uvm_uuid_eq(&p->destinationUuid, &NV_PROCESSOR_UUID_CPU_DEFAULT)) {
		status = uvm_vas_gpu_registered(vas, &p->destinationUuid, NULL);
		if (status != NV_OK)
			return (status);
	} else if (p->cpuNumaNode != -1 && p->cpuNumaNode != 0) {
		return (NV_ERR_INVALID_ARGUMENT);
	}
	return (uvm_vas_check_managed_interval(vas, p->base, p->length));
}

static NV_STATUS
uvm_h_migrate_range_group(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_MIGRATE_RANGE_GROUP_PARAMS *p = params;
	NV_STATUS status;

	(void)fd; (void)td;
	status = uvm_vas_range_group_lookup(vas, p->rangeGroupId);
	if (status != NV_OK)
		return (status);
	if (!uvm_uuid_eq(&p->destinationUuid, &NV_PROCESSOR_UUID_CPU_DEFAULT)) {
		status = uvm_vas_gpu_registered(vas, &p->destinationUuid, NULL);
		if (status != NV_OK)
			return (status);
	}
	return (NV_OK);	/* the group has no intervals: nothing to migrate */
}

static NV_STATUS
uvm_h_discard(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_DISCARD_PARAMS *p = params;

	(void)fd; (void)td;
	if ((p->flags & ~UVM_DISCARD_FLAGS_UNMAP) != 0)
		return (NV_ERR_INVALID_ARGUMENT);
	return (uvm_vas_check_managed_interval(vas, p->base, p->length));
}

/* ---- 5.8 range groups ---- */

static NV_STATUS
uvm_h_create_range_group(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_CREATE_RANGE_GROUP_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_create_range_group(vas, &p->rangeGroupId));
}

static NV_STATUS
uvm_h_destroy_range_group(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_DESTROY_RANGE_GROUP_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_destroy_range_group(vas, p->rangeGroupId));
}

static NV_STATUS
uvm_h_set_range_group(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_SET_RANGE_GROUP_PARAMS *p = params;
	NV_STATUS status;

	(void)fd; (void)td;
	status = uvm_vas_range_group_lookup(vas, p->rangeGroupId);
	if (status != NV_OK)
		return (status);
	return (uvm_vas_check_managed_interval(vas, p->requestedBase, p->length));
}

static NV_STATUS
uvm_h_migration_range_groups(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_PREVENT_MIGRATION_RANGE_GROUPS_PARAMS *p = params;
	NV_STATUS status;
	NvU64 i;

	(void)fd; (void)td;
	if (p->numGroupIds > UVM_MAX_RANGE_GROUPS_REQUEST_SIZE)
		return (NV_ERR_INVALID_ARGUMENT);
	for (i = 0; i < p->numGroupIds; i++) {
		status = uvm_vas_range_group_lookup(vas, p->rangeGroupIds[i]);
		if (status != NV_OK)
			return (status);
	}
	return (NV_OK);	/* no intervals are assigned to any group yet */
}

/* ---- 5.9 - 5.14 ---- */

static NV_STATUS
uvm_h_system_wide_atomics(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td,
    bool enable)
{
	UVM_ENABLE_SYSTEM_WIDE_ATOMICS_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_set_sys_wide_atomics(vas, &p->gpu_uuid, enable));
}

static NV_STATUS
uvm_h_enable_atomics(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	return (uvm_h_system_wide_atomics(fd, vas, params, td, true));
}

static NV_STATUS
uvm_h_disable_atomics(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	return (uvm_h_system_wide_atomics(fd, vas, params, td, false));
}

static NV_STATUS
uvm_h_ok(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)vas; (void)params; (void)td;
	return (NV_OK);
}

static NV_STATUS
uvm_h_populate_pageable(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_POPULATE_PAGEABLE_PARAMS *p = params;
	vm_map_t map;
	vm_map_entry_t entry;
	vm_offset_t start, end;

	(void)fd; (void)vas;
	if (p->flags != 0)
		return (NV_ERR_INVALID_ARGUMENT);
	if (p->length == 0 || (p->base & PAGE_MASK) != 0 || (p->length & PAGE_MASK) != 0 ||
	    p->base + p->length < p->base)
		return (NV_ERR_INVALID_ADDRESS);
	/* the interval must be readable ordinary mappings of the calling process, not device ones */
	map = &td->td_proc->p_vmspace->vm_map;
	start = (vm_offset_t)p->base;
	end = (vm_offset_t)(p->base + p->length);
	vm_map_lock_read(map);
	for (; start < end; start = entry->end) {
		if (!vm_map_lookup_entry(map, start, &entry) || (entry->protection & VM_PROT_READ) == 0 ||
		    (entry->eflags & MAP_ENTRY_IS_SUB_MAP) != 0 ||
		    (entry->object.vm_object != NULL && (entry->object.vm_object->type == OBJT_DEVICE ||
		    entry->object.vm_object->type == OBJT_MGTDEVICE))) {
			vm_map_unlock_read(map);
			return (NV_ERR_INVALID_ADDRESS);
		}
	}
	vm_map_unlock_read(map);
	/* a best-effort populate (01 section 5.10): the pages are touched on first GPU use instead */
	return (NV_OK);
}

static NV_STATUS
uvm_h_tools_process_memory(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_TOOLS_READ_PROCESS_MEMORY_PARAMS *p = params;

	(void)fd; (void)td;
	p->bytesRead = 0;
	if (p->size == 0)
		return (NV_OK);
	/* only managed memory is reachable this way; none exists yet */
	return (uvm_vas_check_managed_interval(vas, p->targetVa & ~(uint64_t)PAGE_MASK, PAGE_SIZE));
}

static NV_STATUS
uvm_h_uuid_table(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_processor_uuid_table(vas, p->tablePtr, false));
}

static NV_STATUS
uvm_h_uuid_table_v2(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_V2_PARAMS *p = params;

	(void)fd; (void)td;
	return (uvm_vas_processor_uuid_table(vas, p->tablePtr, true));
}

static NV_STATUS
uvm_h_peer_access(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	UVM_ENABLE_PEER_ACCESS_PARAMS *p = params;
	NV_STATUS status;

	(void)fd; (void)td;
	status = uvm_vas_gpu_registered(vas, &p->gpuUuidA, NULL);
	if (status == NV_OK)
		status = uvm_vas_gpu_registered(vas, &p->gpuUuidB, NULL);
	if (status != NV_OK)
		return (status);
	UVM_NOT_YET("PCIe peer access", "phase 3");
	return (NV_ERR_NOT_SUPPORTED);
}

static NV_STATUS
uvm_h_alloc_device_p2p(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)vas; (void)params; (void)td;
	UVM_NOT_YET("device P2P memory", "phase 2, 11 section 6");
	return (NV_ERR_NOT_SUPPORTED);
}

static NV_STATUS
uvm_h_tools_tracker(struct uvm_fd *fd, struct uvm_va_space *vas, void *params, struct thread *td)
{
	(void)fd; (void)vas; (void)params; (void)td;
	UVM_NOT_YET("the tools event trackers and counters", "phase 3");
	return (NV_ERR_NOT_SUPPORTED);
}

/* ---- the table ---- */

#define	REQ(num, type, vas_req, tools, fn) \
	{ num, #num, sizeof(type), UVM_RM_STATUS_OFFSET(type), vas_req, tools, fn }

static const struct uvm_request uvm_requests[] = {
	REQ(UVM_INITIALIZE, UVM_INITIALIZE_PARAMS, false, false, uvm_h_initialize),
	REQ(UVM_MM_INITIALIZE, UVM_MM_INITIALIZE_PARAMS, false, false, uvm_h_mm_initialize),
	REQ(UVM_PAGEABLE_MEM_ACCESS, UVM_PAGEABLE_MEM_ACCESS_PARAMS, true, false, uvm_h_pageable_mem_access),
	REQ(UVM_PAGEABLE_MEM_ACCESS_ON_GPU, UVM_PAGEABLE_MEM_ACCESS_ON_GPU_PARAMS, true, false,
	    uvm_h_pageable_mem_access_on_gpu),
	REQ(UVM_REGISTER_GPU, UVM_REGISTER_GPU_PARAMS, true, false, uvm_h_register_gpu),
	REQ(UVM_UNREGISTER_GPU, UVM_UNREGISTER_GPU_PARAMS, true, false, uvm_h_unregister_gpu),
	REQ(UVM_REGISTER_GPU_VASPACE, UVM_REGISTER_GPU_VASPACE_PARAMS, true, false, uvm_h_register_gpu_vaspace),
	REQ(UVM_UNREGISTER_GPU_VASPACE, UVM_UNREGISTER_GPU_VASPACE_PARAMS, true, false,
	    uvm_h_unregister_gpu_vaspace),
	REQ(UVM_REGISTER_CHANNEL, UVM_REGISTER_CHANNEL_PARAMS, true, false, uvm_h_register_channel),
	REQ(UVM_UNREGISTER_CHANNEL, UVM_UNREGISTER_CHANNEL_PARAMS, true, false, uvm_h_unregister_channel),
	REQ(UVM_ENABLE_PEER_ACCESS, UVM_ENABLE_PEER_ACCESS_PARAMS, true, false, uvm_h_peer_access),
	REQ(UVM_DISABLE_PEER_ACCESS, UVM_DISABLE_PEER_ACCESS_PARAMS, true, false, uvm_h_peer_access),
	REQ(UVM_CREATE_RANGE_GROUP, UVM_CREATE_RANGE_GROUP_PARAMS, true, false, uvm_h_create_range_group),
	REQ(UVM_DESTROY_RANGE_GROUP, UVM_DESTROY_RANGE_GROUP_PARAMS, true, false, uvm_h_destroy_range_group),
	REQ(UVM_SET_RANGE_GROUP, UVM_SET_RANGE_GROUP_PARAMS, true, false, uvm_h_set_range_group),
	REQ(UVM_PREVENT_MIGRATION_RANGE_GROUPS, UVM_PREVENT_MIGRATION_RANGE_GROUPS_PARAMS, true, false,
	    uvm_h_migration_range_groups),
	REQ(UVM_ALLOW_MIGRATION_RANGE_GROUPS, UVM_ALLOW_MIGRATION_RANGE_GROUPS_PARAMS, true, false,
	    uvm_h_migration_range_groups),
	REQ(UVM_CREATE_EXTERNAL_RANGE, UVM_CREATE_EXTERNAL_RANGE_PARAMS, true, false, uvm_h_create_external_range),
	REQ(UVM_MAP_EXTERNAL_ALLOCATION, UVM_MAP_EXTERNAL_ALLOCATION_PARAMS, true, false,
	    uvm_h_map_external_allocation),
	REQ(UVM_MAP_EXTERNAL_SPARSE, UVM_MAP_EXTERNAL_SPARSE_PARAMS, true, false, uvm_h_map_external_sparse),
	REQ(UVM_UNMAP_EXTERNAL, UVM_UNMAP_EXTERNAL_PARAMS, true, false, uvm_h_unmap_external),
	REQ(UVM_FREE, UVM_FREE_PARAMS, true, false, uvm_h_free),
	REQ(UVM_VALIDATE_VA_RANGE, UVM_VALIDATE_VA_RANGE_PARAMS, true, false, uvm_h_validate_va_range),
	REQ(UVM_MAP_DYNAMIC_PARALLELISM_REGION, UVM_MAP_DYNAMIC_PARALLELISM_REGION_PARAMS, true, false,
	    uvm_h_map_dynamic_parallelism),
	REQ(UVM_ALLOC_SEMAPHORE_POOL, UVM_ALLOC_SEMAPHORE_POOL_PARAMS, true, false, uvm_h_alloc_semaphore_pool),
	REQ(UVM_SET_PREFERRED_LOCATION, UVM_SET_PREFERRED_LOCATION_PARAMS, true, false,
	    uvm_h_set_preferred_location),
	REQ(UVM_UNSET_PREFERRED_LOCATION, UVM_UNSET_PREFERRED_LOCATION_PARAMS, true, false, uvm_h_managed_interval),
	REQ(UVM_SET_ACCESSED_BY, UVM_SET_ACCESSED_BY_PARAMS, true, false, uvm_h_accessed_by),
	REQ(UVM_UNSET_ACCESSED_BY, UVM_UNSET_ACCESSED_BY_PARAMS, true, false, uvm_h_accessed_by),
	REQ(UVM_ENABLE_READ_DUPLICATION, UVM_ENABLE_READ_DUPLICATION_PARAMS, true, false, uvm_h_managed_interval),
	REQ(UVM_DISABLE_READ_DUPLICATION, UVM_DISABLE_READ_DUPLICATION_PARAMS, true, false, uvm_h_managed_interval),
	REQ(UVM_MIGRATE, UVM_MIGRATE_PARAMS, true, false, uvm_h_migrate),
	REQ(UVM_MIGRATE_RANGE_GROUP, UVM_MIGRATE_RANGE_GROUP_PARAMS, true, false, uvm_h_migrate_range_group),
	REQ(UVM_DISCARD, UVM_DISCARD_PARAMS, true, false, uvm_h_discard),
	REQ(UVM_ENABLE_SYSTEM_WIDE_ATOMICS, UVM_ENABLE_SYSTEM_WIDE_ATOMICS_PARAMS, true, false, uvm_h_enable_atomics),
	REQ(UVM_DISABLE_SYSTEM_WIDE_ATOMICS, UVM_DISABLE_SYSTEM_WIDE_ATOMICS_PARAMS, true, false,
	    uvm_h_disable_atomics),
	REQ(UVM_CLEAN_UP_ZOMBIE_RESOURCES, UVM_CLEAN_UP_ZOMBIE_RESOURCES_PARAMS, true, false, uvm_h_ok),
	REQ(UVM_POPULATE_PAGEABLE, UVM_POPULATE_PAGEABLE_PARAMS, true, false, uvm_h_populate_pageable),
	REQ(UVM_TOOLS_READ_PROCESS_MEMORY, UVM_TOOLS_READ_PROCESS_MEMORY_PARAMS, true, false,
	    uvm_h_tools_process_memory),
	REQ(UVM_TOOLS_WRITE_PROCESS_MEMORY, UVM_TOOLS_WRITE_PROCESS_MEMORY_PARAMS, true, false,
	    uvm_h_tools_process_memory),
	REQ(UVM_TOOLS_GET_PROCESSOR_UUID_TABLE, UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_PARAMS, true, false,
	    uvm_h_uuid_table),
	REQ(UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_V2, UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_V2_PARAMS, true, false,
	    uvm_h_uuid_table_v2),
	REQ(UVM_TOOLS_FLUSH_EVENTS, UVM_TOOLS_FLUSH_EVENTS_PARAMS, true, false, uvm_h_ok),
	REQ(UVM_CLEAR_ALL_ACCESS_COUNTERS, UVM_CLEAR_ALL_ACCESS_COUNTERS_PARAMS, true, false, uvm_h_ok),
	REQ(UVM_ALLOC_DEVICE_P2P, UVM_ALLOC_DEVICE_P2P_PARAMS, true, false, uvm_h_alloc_device_p2p),
	/* the tools node (7.1): accepted there and only there */
	REQ(UVM_TOOLS_INIT_EVENT_TRACKER, UVM_TOOLS_INIT_EVENT_TRACKER_PARAMS, false, true, uvm_h_tools_tracker),
	REQ(UVM_TOOLS_INIT_EVENT_TRACKER_V2, UVM_TOOLS_INIT_EVENT_TRACKER_V2_PARAMS, false, true,
	    uvm_h_tools_tracker),
	REQ(UVM_TOOLS_SET_NOTIFICATION_THRESHOLD, UVM_TOOLS_SET_NOTIFICATION_THRESHOLD_PARAMS, false, true,
	    uvm_h_tools_tracker),
	REQ(UVM_TOOLS_EVENT_QUEUE_ENABLE_EVENTS, UVM_TOOLS_EVENT_QUEUE_ENABLE_EVENTS_PARAMS, false, true,
	    uvm_h_tools_tracker),
	REQ(UVM_TOOLS_EVENT_QUEUE_DISABLE_EVENTS, UVM_TOOLS_EVENT_QUEUE_DISABLE_EVENTS_PARAMS, false, true,
	    uvm_h_tools_tracker),
	REQ(UVM_TOOLS_ENABLE_COUNTERS, UVM_TOOLS_ENABLE_COUNTERS_PARAMS, false, true, uvm_h_tools_tracker),
	REQ(UVM_TOOLS_DISABLE_COUNTERS, UVM_TOOLS_DISABLE_COUNTERS_PARAMS, false, true, uvm_h_tools_tracker),
};

/* CPD-ON */

static const struct uvm_request *
uvm_request_lookup(uint32_t number)
{
	size_t i;

	for (i = 0; i < nitems(uvm_requests); i++)
		if (uvm_requests[i].number == number)
			return (&uvm_requests[i]);
	return (NULL);
}

/*
 * uvm_rm_status_field - the rmStatus field of a parameter structure.
 *
 * The offset is the ABI's (UVM_RM_STATUS_OFFSET), not the end of the
 * structure: a structure with tail padding carries rmStatus before that
 * padding, and the user reads the field where the specification puts it
 * (01 section 2.2 step 4).
 */
static inline NV_STATUS *
uvm_rm_status_field(void *params, size_t offset)
{
	return ((NV_STATUS *)((uint8_t *)params + offset));
}

#define	UVM_IOCTL_SMALL_SIZE	288

/*
 * uvm_ioctl_dispatch - 01 section 2.2.  Returns the syscall result; the
 * request's own result travels in rmStatus.
 */
int
uvm_ioctl_dispatch(struct uvm_fd *fd, bool tools_node, uint32_t request, void *user_ptr, struct thread *td)
{
	const struct uvm_request *req;
	struct uvm_va_space *vas;
	uint8_t small[UVM_IOCTL_SMALL_SIZE] __aligned(8);
	void *params;
	NV_STATUS status;
	int error = 0;

	if (request == UVM_DEINITIALIZE)
		return (0);	/* accepted on any fd, reads nothing, does nothing */
	req = uvm_request_lookup(request);
	if (req == NULL || req->tools_node != tools_node) {
		if (uvm.builtin_tests && !tools_node)
			uvm_log_ratelimited("request %u: built-in tests are not implemented", request);
		return (tools_node ? EINVAL : ENOSYS);
	}
	if (!sx_try_slock(&uvm.pm_lock))
		return (EAGAIN);

	params = req->size <= sizeof(small) ? small : malloc(req->size, M_NVIDIA_UVM, M_WAITOK);
	error = copyin(user_ptr, params, req->size);
	if (error != 0) {
		error = EFAULT;
		goto out;
	}
	status = uvm_global_status();
	*uvm_rm_status_field(params, req->rm_status_offset) = status;
	if (status == NV_OK) {
		vas = uvm_fd_va_space(fd);
		if (req->needs_va_space && vas == NULL)
			status = NV_ERR_ILLEGAL_ACTION;
		else
			status = req->handler(fd, vas, params, td);
		*uvm_rm_status_field(params, req->rm_status_offset) = status;
		if (status != NV_OK)
			uvm_log_ratelimited("%s: rmStatus 0x%x", req->name, (unsigned)status);
	}
	if (copyout(params, user_ptr, req->size) != 0)
		error = EFAULT;
out:
	if (params != small)
		free(params, M_NVIDIA_UVM);
	sx_sunlock(&uvm.pm_lock);
	return (error);
}
