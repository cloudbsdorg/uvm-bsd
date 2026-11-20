/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_mmap.c - mmap of /dev/nvidia-uvm.  uvm-spec r1, 01-user-abi.md
 * section 4 and 09-freebsd-facilities.md section 3.
 *
 * Two mappings: the CPU view of a semaphore pool (rule 8 of 01 section 4.1:
 * an mmap whose extent is exactly a semaphore pool range maps that range's
 * memory; a fault beyond it is SIGBUS), and a managed range
 * (cudaMallocManaged), created by an mmap of a free address.  A managed
 * range is for now CPU resident and mapped on every GPU (no migration, no
 * fault servicing), backed by system memory like a pool.  The pager cannot
 * see the virtual address of the mapping, so rule 4 (offset == address) is
 * the UMD's guarantee (it maps with MAP_FIXED at the offset); likewise
 * MAP_SHARED, which d_mmap_single is not told.
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
#include "uvm_pager_ref.h"

#include <sys/filedesc.h>
#include <sys/file.h>
#include <sys/rwlock.h>
#include <sys/mman.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

/*
 * The pager of a semaphore pool: the handle is the pool's memory.  The
 * constructor runs exactly once per object, after the device pager's list
 * mutex is dropped (a concurrent finder of the same handle waits until the
 * constructor has returned), so it is where the object takes its memory
 * reference (uvm_pager_ref.h) and its place in the module's count of live
 * pager objects.  (Deciding afterwards,
 * from the object's reference count, whether a mapper "created" it is a
 * race: two mappers of one pool can both see a count above one, both drop
 * their reference, and leave the object with none -- Track #2272.)  The
 * destructor drops both; the count goes last, as nothing may touch module
 * state after it.
 */
static int
uvm_sema_pg_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot, vm_ooffset_t foff, struct ucred *cred,
    u_short *color)
{
	(void)size; (void)prot; (void)foff; (void)cred;
	for (unsigned i = 0; i < uvm_pager_ctor_mem_refs(); i++)
		uvm_sema_mem_ref(handle);
	uvm_pager_obj_new();
	*color = 0;
	return (0);
}

static void
uvm_sema_pg_dtor(void *handle)
{
	uvm_sema_mem_unref(handle);
	uvm_pager_obj_done();
}

static int
uvm_managed_pg_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot, vm_ooffset_t foff, struct ucred *cred,
    u_short *color)
{
	/* the object was counted, and holds its memory, when the range was created */
	(void)handle; (void)size; (void)prot; (void)foff; (void)cred;
	*color = 0;
	return (0);
}

/*
 * uvm_sema_pg_fault - the page at an absolute offset: a fictitious page
 * over the pool's memory, or SIGBUS (VM_PAGER_BAD) outside it.
 */
static int
uvm_mem_pg_fault(vm_object_t object, struct uvm_sema_mem *mem, vm_ooffset_t offset, vm_page_t *mres)
{
	vm_paddr_t paddr;
	vm_page_t page;

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

static int
uvm_sema_pg_fault(vm_object_t object, vm_ooffset_t offset, int prot, vm_page_t *mres)
{
	(void)prot;
	return (uvm_mem_pg_fault(object, object->handle, offset, mres));
}

static const struct cdev_pager_ops uvm_sema_pager_ops = {
	.cdev_pg_ctor = uvm_sema_pg_ctor,
	.cdev_pg_dtor = uvm_sema_pg_dtor,
	.cdev_pg_fault = uvm_sema_pg_fault,
};

/*
 * The pager of a managed range: the handle is the range's uvm_managed_obj.
 * Its fault handler is the pool's (a fictitious page over the range's
 * memory); its destructor, which runs on munmap of the last mapping or at
 * process exit, destroys the range (uvm_managed_obj_release).  Touching
 * the power-management lock keeps the GPU page-table writes of that
 * teardown out of a suspend.
 */
static void
uvm_managed_pg_dtor(void *handle)
{
	sx_slock(&uvm.pm_lock);
	uvm_managed_obj_release(handle);
	sx_sunlock(&uvm.pm_lock);
	/* nothing below touches module state: the unload may proceed once the count drops */
	uvm_pager_obj_done();
}

static int
uvm_managed_pg_fault(vm_object_t object, vm_ooffset_t offset, int prot, vm_page_t *mres)
{
	struct uvm_managed_obj *obj = object->handle;

	(void)prot;
	return (uvm_mem_pg_fault(object, obj->mem, offset, mres));
}

static const struct cdev_pager_ops uvm_managed_pager_ops = {
	.cdev_pg_ctor = uvm_managed_pg_ctor,
	.cdev_pg_dtor = uvm_managed_pg_dtor,
	.cdev_pg_fault = uvm_managed_pg_fault,
};

/* a managed range at [offset, offset+size): create it and its pager object; pm_lock is held shared */
static int
uvm_mmap_managed(struct uvm_va_space *vas, vm_ooffset_t *offset, vm_size_t size, vm_object_t *object, int nprot)
{
	struct uvm_managed_obj *mobj;
	vm_object_t obj;
	NV_STATUS status;

	status = uvm_vas_create_managed(vas, *offset, size, &mobj);
	if (status != NV_OK)
		return (uvm_status_to_errno(status));
	obj = cdev_pager_allocate(mobj, OBJT_DEVICE, &uvm_managed_pager_ops, *offset + size, nprot, *offset,
	    curthread->td_ucred);
	if (obj == NULL) {
		uvm_managed_obj_release(mobj);	/* pm_lock is already held: not through the dtor */
		uvm_pager_obj_done();
		return (EINVAL);
	}
	KASSERT(obj->handle == mobj, ("managed pager object %p has handle %p, not %p", obj, obj->handle, mobj));
	*object = obj;
	return (0);
}

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
		/* a free address: a managed range (rule 8); a collision further up is ADDRESS_IN_USE there */
		sx_sunlock(&vas->lock);
		error = uvm_mmap_managed(vas, offset, size, object, nprot);
		sx_sunlock(&uvm.pm_lock);
		return (error);
	}
	if (r->type != UVM_RANGE_SEMA_POOL || r->core.start != *offset || r->core.end != *offset + size - 1) {
		sx_sunlock(&vas->lock);
		sx_sunlock(&uvm.pm_lock);
		return (uvm_status_to_errno(NV_ERR_UVM_ADDRESS_IN_USE));
	}
	mem = uvm_sema_mem_ref(r->u.sema.mem);
	mem->user_start = r->core.start;
	sx_sunlock(&vas->lock);

	obj = cdev_pager_allocate(mem, OBJT_DEVICE, &uvm_sema_pager_ops, *offset + size, nprot, *offset,
	    curthread->td_ucred);
	/*
	 * Our reference only keeps the memory alive until the pager has its
	 * own: a new object's constructor took it, an existing object already
	 * had it.  Either way ours goes now.
	 */
	if (uvm_pager_mapper_drops_ref())
		uvm_sema_mem_unref(mem);
	if (obj == NULL) {
		sx_sunlock(&uvm.pm_lock);
		return (EINVAL);
	}
	*object = obj;
	sx_sunlock(&uvm.pm_lock);
	return (0);
}
