/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_mmap.c - mmap of /dev/nvidia-uvm.  uvm-spec r1, 01-user-abi.md
 * section 4 and 09-freebsd-facilities.md section 3.
 *
 * Phase-1 scope: the one mapping libcuda needs for cudaMalloc programs, the
 * CPU view of a semaphore pool (rule 8 of 01 section 4.1: an mmap whose
 * extent is exactly a semaphore pool range maps that range's memory; its
 * pages are inserted eagerly, a fault beyond them is SIGBUS).  Managed
 * ranges (cudaMallocManaged) are a later phase: an mmap of a free address
 * is refused with ENOSYS and logged, never created half-way.
 *
 * The mapping is a device pager object keyed by the range, so every mmap
 * of the same pool in the process shares it; its fault handler answers
 * with a fictitious page over the pool's physical memory, as the in-tree
 * device pagers do, so no page of the pool is ever copied on fork and the
 * memory lives (reference counted) until both the range and the object are
 * gone.  The file offset is the address the UMD maps (rule 4): the pager's
 * offsets are absolute, and the pool's start is subtracted.
 */

#include "uvm.h"

#include <sys/filedesc.h>
#include <sys/file.h>
#include <sys/rwlock.h>
#include <sys/mman.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

static int
uvm_sema_pg_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot, vm_ooffset_t foff, struct ucred *cred,
    u_short *color)
{
	(void)handle; (void)size; (void)prot; (void)foff; (void)cred;
	*color = 0;
	return (0);
}

static void
uvm_sema_pg_dtor(void *handle)
{
	struct uvm_sema_mem *mem = handle;

	uvm_sema_mem_unref(mem);
}

/*
 * uvm_sema_pg_fault - the page at an absolute offset: a fictitious page
 * over the pool's memory, or SIGBUS (VM_PAGER_BAD) outside it.
 */
static int
uvm_sema_pg_fault(vm_object_t object, vm_ooffset_t offset, int prot, vm_page_t *mres)
{
	struct uvm_sema_mem *mem = object->handle;
	vm_paddr_t paddr;
	vm_page_t page;

	(void)prot;
	if (offset < mem->user_start || offset - mem->user_start >= mem->bytes)
		return (VM_PAGER_BAD);
	paddr = pmap_kextract((vm_offset_t)mem->kva + (vm_offset_t)(offset - mem->user_start));
	if (((*mres)->flags & PG_FICTITIOUS) != 0) {
		page = *mres;
		vm_page_updatefake(page, paddr, VM_MEMATTR_DEFAULT);
	} else {
		VM_OBJECT_WUNLOCK(object);
		page = vm_page_getfake(paddr, VM_MEMATTR_DEFAULT);
		VM_OBJECT_WLOCK(object);
		vm_page_replace(page, object, (*mres)->pindex, *mres);
		*mres = page;
	}
	vm_page_valid(page);
	return (VM_PAGER_OK);
}

static const struct cdev_pager_ops uvm_sema_pager_ops = {
	.cdev_pg_ctor = uvm_sema_pg_ctor,
	.cdev_pg_dtor = uvm_sema_pg_dtor,
	.cdev_pg_fault = uvm_sema_pg_fault,
};

int
uvm_mmap_single(struct cdev *cdev, vm_ooffset_t *offset, vm_size_t size, vm_object_t *object, int nprot)
{
	struct uvm_fd *fd;
	struct uvm_va_space *vas;
	struct uvm_range *r;
	struct uvm_sema_mem *mem;
	vm_object_t obj;
	int error;

	if (cdev != uvm.cdev_main)
		return (ENODEV);
	if (uvm_global_status() != NV_OK)
		return (uvm_status_to_errno(uvm_global_status()));
	error = devfs_get_cdevpriv((void **)&fd);
	if (error != 0)
		return (error);
	vas = uvm_fd_va_space(fd);
	if (vas == NULL)
		return (EBADF);	/* the spec's EBADFD has no FreeBSD equivalent */
	if (vas->vm != NULL && curproc->p_vmspace != vas->vm)
		return (EOPNOTSUPP);
	if ((*offset & PAGE_MASK) != 0 || size == 0)
		return (EINVAL);
	if ((nprot & (PROT_READ | PROT_WRITE)) != (PROT_READ | PROT_WRITE))
		return (EINVAL);
	if (!sx_try_slock(&uvm.pm_lock))
		return (EAGAIN);

	sx_slock(&vas->lock);
	r = uvm_range_find(vas, *offset);
	if (r == NULL) {
		sx_sunlock(&vas->lock);
		sx_sunlock(&uvm.pm_lock);
		uvm_log_ratelimited("mmap of %#jx (%ju bytes) by pid %d: managed memory is a later phase (ENOSYS)",
		    (uintmax_t)*offset, (uintmax_t)size, curproc->p_pid);
		return (ENOSYS);
	}
	if (r->type != UVM_RANGE_SEMA_POOL || r->start != *offset || r->end != *offset + size - 1) {
		sx_sunlock(&vas->lock);
		sx_sunlock(&uvm.pm_lock);
		return (uvm_status_to_errno(NV_ERR_UVM_ADDRESS_IN_USE));
	}
	mem = uvm_sema_mem_ref(r->u.sema.mem);
	mem->user_start = r->start;
	sx_sunlock(&vas->lock);

	obj = cdev_pager_allocate(mem, OBJT_DEVICE, &uvm_sema_pager_ops, *offset + size, nprot, *offset,
	    curthread->td_ucred);
	if (obj == NULL) {
		uvm_sema_mem_unref(mem);
		sx_sunlock(&uvm.pm_lock);
		return (EINVAL);
	}
	/* the pager holds exactly one memory reference per object: drop ours when it reused an existing one */
	if (obj->handle == mem && obj->ref_count > 1)
		uvm_sema_mem_unref(mem);
	*object = obj;
	sx_sunlock(&uvm.pm_lock);
	return (0);
}
