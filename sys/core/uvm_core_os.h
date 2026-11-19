/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_core_os.h - the core's only contact with the operating system: the
 * memory it allocates, the lock it takes and the assertion it makes.
 *
 * Every file under sys/core/ reaches the operating system through this
 * header and through nothing else, so that the core is written once and
 * runs in two places: in the kernel, where the memory comes from
 * malloc(9), the lock from struct sx and the assertion from KASSERT, and
 * in userland, where the same names stand for calloc(3), a pthread
 * read/write lock and assert(3).  A test can therefore exercise a core
 * file on its own, outside the kernel, and the core file itself names no
 * operating system function directly.
 *
 * The header names no device vendor.
 */

#ifndef _UVM_CORE_OS_H_
#define	_UVM_CORE_OS_H_

#include <sys/types.h>

/* CPD-OFF -- the kernel and the userland shim are the same six functions over two OS surfaces, by design */
#ifdef _KERNEL

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/sx.h>

/*
 * The zone the core allocates in.  The definition of the zone is in
 * uvm_core_device.c, which is the one core file that allocates devices;
 * this header only declares it.
 */
MALLOC_DECLARE(M_UVM_CORE);

/*
 * uvm_core_alloc - memory for the core, zeroed by the caller.
 * Time: O(1).  Space: O(size).
 */
static inline void *
uvm_core_alloc(size_t size)
{
	return (malloc(size, M_UVM_CORE, M_WAITOK));
}

/*
 * uvm_core_free - give back memory taken by uvm_core_alloc.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_free(void *p)
{
	free(p, M_UVM_CORE);
}

/* the lock the core takes: one reader at a time, many readers together */
struct uvm_core_rwlock {
	struct sx	lock;
};

/*
 * uvm_core_rwlock_init - a lock that is ready to be taken.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_init(struct uvm_core_rwlock *lock)
{
	sx_init(&lock->lock, "uvm_core");
}

/*
 * uvm_core_rwlock_destroy - a lock that nobody holds any more.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_destroy(struct uvm_core_rwlock *lock)
{
	sx_destroy(&lock->lock);
}

/*
 * uvm_core_rwlock_rlock - take the lock for reading.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_rlock(struct uvm_core_rwlock *lock)
{
	sx_slock(&lock->lock);
}

/*
 * uvm_core_rwlock_runlock - give up a read.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_runlock(struct uvm_core_rwlock *lock)
{
	sx_sunlock(&lock->lock);
}

/*
 * uvm_core_rwlock_wlock - take the lock for writing.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_wlock(struct uvm_core_rwlock *lock)
{
	sx_xlock(&lock->lock);
}

/*
 * uvm_core_rwlock_wunlock - give up a write.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_wunlock(struct uvm_core_rwlock *lock)
{
	sx_xunlock(&lock->lock);
}

/*
 * UVM_CORE_ASSERT - the core's own check: it must hold, and a kernel
 * that breaks it stops rather than carries on.
 */
#define	UVM_CORE_ASSERT(cond)	KASSERT((cond), ("uvm core: " #cond))

/* the kernel's own way back from a member to its object */
#define	UVM_CORE_CONTAINEROF(ptr, type, field)	__containerof(ptr, type, field)

#else /* _KERNEL */

#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

/*
 * uvm_core_alloc - memory for the core, zeroed.
 * Time: O(1).  Space: O(size).
 */
static inline void *
uvm_core_alloc(size_t size)
{
	return (calloc(1, size));
}

/*
 * uvm_core_free - give back memory taken by uvm_core_alloc.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_free(void *p)
{
	free(p);
}

/* the lock the core takes: one writer at a time, many readers together */
struct uvm_core_rwlock {
	pthread_rwlock_t	lock;
};

/*
 * uvm_core_rwlock_init - a lock that is ready to be taken.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_init(struct uvm_core_rwlock *lock)
{
	pthread_rwlock_init(&lock->lock, NULL);
}

/*
 * uvm_core_rwlock_destroy - a lock that nobody holds any more.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_destroy(struct uvm_core_rwlock *lock)
{
	pthread_rwlock_destroy(&lock->lock);
}

/*
 * uvm_core_rwlock_rlock - take the lock for reading.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_rlock(struct uvm_core_rwlock *lock)
{
	pthread_rwlock_rdlock(&lock->lock);
}

/*
 * uvm_core_rwlock_runlock - give up a read.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_runlock(struct uvm_core_rwlock *lock)
{
	pthread_rwlock_unlock(&lock->lock);
}

/*
 * uvm_core_rwlock_wlock - take the lock for writing.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_wlock(struct uvm_core_rwlock *lock)
{
	pthread_rwlock_wrlock(&lock->lock);
}

/*
 * uvm_core_rwlock_wunlock - give up a write.
 * Time: O(1).  Space: O(1).
 */
static inline void
uvm_core_rwlock_wunlock(struct uvm_core_rwlock *lock)
{
	pthread_rwlock_unlock(&lock->lock);
}

/* the core's own check: it must hold */
#define	UVM_CORE_ASSERT(cond)	assert(cond)

/* the userland way back from a member to its object */
#define	UVM_CORE_CONTAINEROF(ptr, type, field)	\
	((type *)(void *)((char *)(ptr) - offsetof(type, field)))

#endif /* _KERNEL */
/* CPD-ON */

#endif /* _UVM_CORE_OS_H_ */
