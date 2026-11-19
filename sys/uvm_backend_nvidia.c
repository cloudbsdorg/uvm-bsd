/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_backend_nvidia.c - the NVIDIA backend of the UVM core: the backend
 * that knows the GPUs of this machine, takes the identity of a GPU and
 * fills the device the core keeps.
 *
 * The backend is the vendor's half of the split of docs/ARCHITECTURE.md.
 * It speaks the language of the RM (a GPU is named by its UUID and its
 * answers are status codes of the vendor) and it answers the core in the
 * core's language (errno, and the neutral capabilities of
 * sys/core/uvm_core_types.h).  The core itself never sees any of it.
 *
 * The identity of a GPU is the 16 bytes of its UUID.  The core keeps no
 * more than 32 bytes of an identity and never looks inside.
 */

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/libkern.h>

#include "uvm.h"
#include "core/uvm_core_ops.h"

/* the identity of a GPU: the 16 bytes of its UUID */
#define	UVM_NVIDIA_UUID_LEN	16

/* ---- the status of the vendor ---- */

/*
 * The errno a status of the vendor becomes inside the core: the core
 * knows errno and nothing else.
 */
struct uvm_nvidia_status_errno {
	NV_STATUS	status;
	int		error;
};

static const struct uvm_nvidia_status_errno uvm_nvidia_status_errnos[] = {
	{ NV_OK, 0 },
	{ NV_ERR_NO_MEMORY, ENOMEM },
	{ NV_ERR_GPU_UUID_NOT_FOUND, ENODEV },
};

/*
 * uvm_nvidia_status_to_errno - the errno of a status of the vendor.  A
 * status the table does not know is a failure of the vendor's own.
 * Time: O(3).  Space: O(1).
 */
static int
uvm_nvidia_status_to_errno(NV_STATUS status)
{
	size_t i;

	for (i = 0; i < sizeof(uvm_nvidia_status_errnos) / sizeof(uvm_nvidia_status_errnos[0]); i++)
		if (uvm_nvidia_status_errnos[i].status == status)
			return (uvm_nvidia_status_errnos[i].error);
	return (EIO);
}

/*
 * uvm_nvidia_core_status - the status of the vendor that a core errno
 * becomes.  The paths of the module that answer a user with a status of
 * the vendor go through it.
 * Time: O(1).  Space: O(1).
 */
NV_STATUS
uvm_nvidia_core_status(int core_errno)
{
	switch (core_errno) {
	case 0:
		return (NV_OK);
	case ENODEV:
		return (NV_ERR_GPU_UUID_NOT_FOUND);
	case ENOSPC:
	case ENOMEM:
		return (NV_ERR_NO_MEMORY);
	default:
		return (NV_ERR_GENERIC);
	}
}

/* ---- the operations a GPU offers the core ---- */

/*
 * uvm_nvidia_describe - what the GPU can do, in the core's words: its
 * name, the memory it has, the page sizes it maps with, the largest
 * virtual address it maps, the faults it reports and the window the core
 * may program.
 * Time: O(1).  Space: O(1).
 */
static int
uvm_nvidia_describe(struct uvm_core_device *device, struct uvm_core_device_caps *caps)
{
	struct uvm_gpu *gpu = device->backend_private;

	if (gpu == NULL)
		return (EINVAL);
	memset(caps, 0, sizeof(*caps));
	strlcpy(caps->name, gpu->name, sizeof(caps->name));
	/* the frame buffer heap, in kilobytes, as bytes */
	caps->memory_bytes = ((uint64_t)gpu->fb.heapSize + gpu->fb.reservedHeapSize) * 1024;
	caps->page_sizes = UVM_CORE_PAGE_SIZE_BIT_4K;
	if (gpu->as_info.bigPageSize == (NvU64)UVM_CORE_PAGE_SIZE_64K)
		caps->page_sizes |= UVM_CORE_PAGE_SIZE_BIT_64K;
	caps->max_virtual_address = gpu->max_channel_va;
	caps->supports_replayable_faults = true;
	caps->dma_window_start = gpu->platform.dma_addressable_start;
	caps->dma_window_limit = gpu->platform.dma_addressable_limit;
	return (0);
}

/*
 * uvm_nvidia_retain - the core took a reference on the device.  The
 * count of the core already covers the GPU, so there is nothing to do.
 * Time: O(1).  Space: O(1).
 */
static int
uvm_nvidia_retain(struct uvm_core_device *device)
{
	(void)device;
	return (0);
}

/*
 * uvm_nvidia_release - the last reference on the device went.  The count
 * of the core already covered the GPU, so there is nothing to do.
 * Time: O(1).  Space: O(1).
 */
static int
uvm_nvidia_release(struct uvm_core_device *device)
{
	(void)device;
	return (0);
}

/*
 * The operations a GPU offers the core.  Only the lifetime of a device
 * is filled in: the other groups of the table come later.
 */
static struct uvm_core_device_ops uvm_nvidia_device_ops = {
	.retain = uvm_nvidia_retain,
	.release = uvm_nvidia_release,
	.describe = uvm_nvidia_describe,
};

/* ---- the backend ---- */

/*
 * uvm_nvidia_probe_by_identity - the core offers an identity; it is the
 * UUID of a GPU and the backend takes it when the RM knows that GPU.
 * Time: O(1).  Space: O(1).
 */
static int
uvm_nvidia_probe_by_identity(const void *identity, size_t identity_len, uvm_core_id_t id,
    struct uvm_core_device *device)
{
	struct uvm_gpu *gpu = NULL;
	NV_STATUS status;

	if (identity == NULL || identity_len != UVM_NVIDIA_UUID_LEN)
		return (ENODEV);
	status = uvm_gpu_retain_by_uuid_client((const NvProcessorUuid *)identity, NULL, &gpu);
	if (status != NV_OK)
		return (uvm_nvidia_status_to_errno(status));

	/* the two objects know each other from now on */
	gpu->core_dev = device;
	device->backend_private = gpu;
	device->ops = &uvm_nvidia_device_ops;
	(void)id;
	return (0);
}

/*
 * uvm_nvidia_backend_release - the core let the device go; the reference
 * the backend took on the GPU goes with it.
 * Time: O(1).  Space: O(1).
 */
static int
uvm_nvidia_backend_release(struct uvm_core_device *device)
{
	struct uvm_gpu *gpu = device->backend_private;

	if (gpu == NULL)
		return (0);
	gpu->core_dev = NULL;
	device->backend_private = NULL;
	uvm_gpu_release(gpu);
	return (0);
}

/*
 * The backend itself, registered with the core when the module loads and
 * unregistered when it unloads.
 */
struct uvm_core_backend uvm_nvidia_backend = {
	.name = "nvidia",
	.probe_by_identity = uvm_nvidia_probe_by_identity,
	.release = uvm_nvidia_backend_release,
};
