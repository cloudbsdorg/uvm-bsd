/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * Copyright (c) 2026 SkunkWerks, GmbH
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm.h - internal types of nvidia-uvm.ko: module state, VA spaces and
 * ranges, GPU objects, channels and pushes, page trees and fault state.
 * The functional contract for every structure here is uvm-spec r1; the
 * section that governs each one is named beside it.
 *
 * Lock order (08-locking.md, outermost first): uvm.pm_lock (readers try
 * only) > uvm.lock (global) > GPU service locks > the bound process map >
 * va_space->serialize > va_space->lock > external range tree mutex >
 * RM calls > page tree lock > channel pool mutex > leaf mutexes (GPU table,
 * interrupt state, instance pointer table, pushbuffer, tracking semaphore).
 * A VA space held for write never calls RM.
 */

#ifndef _UVM_H_
#define	_UVM_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/taskqueue.h>
#include <sys/conf.h>
#include <sys/bus.h>
#include <sys/proc.h>
#include <machine/bus.h>
#include <machine/atomic.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>

#include "core/uvm_core_range.h"
#include "uvm_managed_state.h"
#include "core/uvm_core_group.h"
#include "core/uvm_core_device.h"

#include "nvidia_uvm_ioctl.h"
#include "nvidia_uvm_rm.h"
#include "uvm_hal_methods.h"
#include "uvm_pte.h"
#include "uvm_mm_state.h"
#include "uvm_unload_state.h"
#include "uvm_fb_state.h"
#include "uvm_pt_loop.h"

MALLOC_DECLARE(M_NVIDIA_UVM);

#define	UVM_NAME	"nvidia-uvm"

/* diagnostics: every message carries the module name; "once" variants cannot flood */
#define	uvm_log(fmt, ...)	printf(UVM_NAME ": " fmt "\n", ##__VA_ARGS__)
#define	uvm_log_ratelimited(fmt, ...) do {				\
	static struct timeval __lasttv;					\
	static int __curpps;						\
	if (ppsratecheck(&__lasttv, &__curpps, 4))			\
		uvm_log(fmt, ##__VA_ARGS__);				\
} while (0)
/* at most one line per call site every UVM_LOG_SLOW_SECS: for messages a client can provoke in a loop (#2295) */
#define	uvm_log_slow(fmt, ...) do {					\
	static struct timeval __slowtv;					\
	static const struct timeval __slowint = { UVM_LOG_SLOW_SECS, 0 };	\
	struct timeval __slownow;					\
									\
	getmicrouptime(&__slownow);					\
	if (uvm_ratelimit_ok(&__slowtv, &__slowint, &__slownow))	\
		uvm_log(fmt, ##__VA_ARGS__);				\
} while (0)

/* processor ids: 0 is the CPU, GPUs are 1..UVM_MAX_GPUS (03 section 1) */
typedef uint16_t uvm_gpu_id_t;
#define	UVM_ID_CPU		0
#define	UVM_ID_GPU_FIRST	1
#define	UVM_ID_GPU_LAST		UVM_MAX_GPUS

/* a bitmap over GPU ids 1..256 */
struct uvm_gpu_mask {
	uint64_t	bits[(UVM_MAX_GPUS + 64) / 64];
};

static inline void
uvm_gpu_mask_set(struct uvm_gpu_mask *m, uvm_gpu_id_t id)
{
	m->bits[id / 64] |= 1ULL << (id % 64);
}

static inline void
uvm_gpu_mask_clear(struct uvm_gpu_mask *m, uvm_gpu_id_t id)
{
	m->bits[id / 64] &= ~(1ULL << (id % 64));
}

static inline bool
uvm_gpu_mask_test(const struct uvm_gpu_mask *m, uvm_gpu_id_t id)
{
	return ((m->bits[id / 64] & (1ULL << (id % 64))) != 0);
}

static inline bool
uvm_uuid_eq(const NvProcessorUuid *a, const NvProcessorUuid *b)
{
	return (memcmp(a->uuid, b->uuid, sizeof(a->uuid)) == 0);
}

/* ---- trackers (04 section 2.4) ---- */

struct uvm_channel;

struct uvm_tracker_entry {
	struct uvm_channel	*channel;
	uint64_t		value;
};

struct uvm_tracker {
	struct uvm_tracker_entry *entries;
	unsigned		count;
	unsigned		capacity;
	struct uvm_tracker_entry inline_entry;
};

void	uvm_tracker_init(struct uvm_tracker *t);
void	uvm_tracker_deinit(struct uvm_tracker *t);
int	uvm_tracker_add(struct uvm_tracker *t, struct uvm_channel *ch, uint64_t value);
int	uvm_tracker_add_tracker(struct uvm_tracker *dst, const struct uvm_tracker *src);
void	uvm_tracker_remove_completed(struct uvm_tracker *t);
NV_STATUS uvm_tracker_wait(struct uvm_tracker *t);

/* ---- pushbuffer, channels, pushes (04 sections 1 and 2) ---- */

#define	UVM_PUSHBUFFER_SIZE		(16u << 20)
#define	UVM_PUSH_MAX_SIZE		(128u << 10)
#define	UVM_PUSH_MAX_WORDS		(UVM_PUSH_MAX_SIZE / 4)
#define	UVM_PUSHBUFFER_SLOTS		(UVM_PUSHBUFFER_SIZE / UVM_PUSH_MAX_SIZE)
#define	UVM_PUSH_INLINE_MAX_WORDS	UVM_METHOD_MAX_COUNT
#define	UVM_CHANNELS_PER_POOL		2

struct uvm_gpu;
struct uvm_channel_manager;
struct uvm_core_device;
struct uvm_core_backend;

struct uvm_gpfifo_entry_state {
	uint64_t	tracking_value;
	int		pb_slot;	/* -1 when none */
};

struct uvm_channel {
	struct uvm_channel_manager *mgr;
	uvmGpuChannelHandle	rm_channel;
	UvmGpuChannelInfo	info;
	struct mtx		lock;		/* channel pool lock: GPFIFO bookkeeping, semaphore state */
	unsigned		index;
	uint32_t		num_gpfifo;
	uint32_t		cpu_put;
	uint32_t		gpu_get;	/* software view: oldest pending entry */
	uint32_t		pending;	/* entries written, not yet known complete */
	uint32_t		reserved;	/* entries reserved by pushes in progress */
	struct uvm_gpfifo_entry_state *entries;
	volatile uint32_t	*sema_cpu;	/* tracking semaphore word */
	uint64_t		sema_gpu_va;
	uint64_t		queued_value;	/* last value a finished push releases */
	uint64_t		completed_value;/* 64-bit mirror of the last payload seen */
	bool			initialised;
};

struct uvm_pushbuffer {
	UvmGpuPointer		gpu_va;
	uint8_t			*cpu;
	struct mtx		lock;
	uint64_t		slot_busy[(UVM_PUSHBUFFER_SLOTS + 63) / 64];
};

struct uvm_channel_manager {
	struct uvm_gpu		*gpu;
	struct uvm_pushbuffer	pb;
	uvmGpuTsgHandle		rm_tsg;
	uint32_t		ce_index;
	struct uvm_channel	channels[UVM_CHANNELS_PER_POOL];
	unsigned		nchannels;
	unsigned		next_channel;
	/* the page of tracking semaphores: RM system memory mapped on the GPU and the CPU */
	UvmGpuPointer		sema_gpu_va;
	volatile uint32_t	*sema_cpu;
	uint32_t		gpfifo_entries;
	bool			gpfifo_sysmem;	/* GPFIFO ring and PUT word in system memory */
};

struct uvm_push {
	struct uvm_channel	*channel;
	uint32_t		*begin;
	uint32_t		*next;
	uint32_t		*end;
	int			slot;
	uint64_t		gpu_va;		/* GPU VA of begin */
	uvm_membar_t		membar;		/* the membar the end-of-push flush uses */
	uint64_t		value;		/* tracking value once ended */
};

static inline uint32_t *
uvm_push_space(struct uvm_push *p, size_t words)
{
	uint32_t *w = p->next;

	KASSERT(p->next + words <= p->end, ("push overflow"));
	p->next += words;
	return (w);
}

#define	UVM_PUSH(p, fn, ...) do {					\
	uint32_t *__w = uvm_push_space((p), UVM_MEM_OP_MAX_WORDS + 8);	\
	size_t __n = fn(__w, ##__VA_ARGS__);				\
	(p)->next = __w + __n;						\
} while (0)

/* the GPU VA of the current write position of a push (for inline data) */
static inline uint64_t
uvm_push_current_gpu_va(const struct uvm_push *p)
{
	return (p->gpu_va + (uint64_t)((const uint8_t *)p->next - (const uint8_t *)p->begin));
}

int	uvm_channel_manager_create(struct uvm_gpu *gpu);
void	uvm_channel_manager_destroy(struct uvm_gpu *gpu);
NV_STATUS uvm_push_begin(struct uvm_gpu *gpu, struct uvm_push *push, const struct uvm_tracker *acquire);
void	uvm_push_end(struct uvm_push *push);
void	uvm_push_end_host_sem(struct uvm_push *push);
NV_STATUS uvm_push_wait(struct uvm_push *push);
NV_STATUS uvm_push_end_and_wait(struct uvm_push *push);
void	uvm_push_inline_data(struct uvm_push *push, const void *data, size_t bytes, uint64_t *gpu_va_out);
void	uvm_channel_update_progress(struct uvm_channel *ch);
bool	uvm_channel_value_complete(struct uvm_channel *ch, uint64_t value);
NV_STATUS uvm_channel_check_errors(struct uvm_gpu *gpu);

/* ---- page trees (05) ---- */

struct uvm_page_tree;

struct uvm_page_table {
	void			*kva;
	bus_dmamap_t		map;
	bus_addr_t		bus;		/* GPU-visible address of the table */
	uint32_t		depth;
	uint64_t		page_size;	/* at the PTE level: 4K or 64K; 0 otherwise */
	uint32_t		entries;
	uint32_t		bytes;
	u_int			refs;		/* children plus PTE ranges */
	struct uvm_page_table	*parent;
	uint32_t		parent_index;
	int			parent_half;	/* dual level: 0 big, 1 small; else -1 */
	struct uvm_page_table	**children;	/* entries (x2 at the dual level) */
	uint64_t		va_base;
};

struct uvm_page_tree {
	struct uvm_gpu		*gpu;
	uint32_t		mmu_version;
	struct sx		lock;
	struct uvm_page_table	*root;
	uint64_t		root_rm_mask;	/* root indexes RM filled at hand-over */
	struct uvm_tracker	tracker;
	bool			is_user;
};

struct uvm_pt_range {
	struct uvm_page_tree	*tree;
	struct uvm_page_table	*table;
	uint32_t		start_index;
	uint32_t		count;
	uint64_t		page_size;
	uint64_t		va_start;
};

NV_STATUS uvm_page_tree_create(struct uvm_gpu *gpu, bool is_user, struct uvm_page_tree **out);
void	uvm_page_tree_destroy(struct uvm_page_tree *tree);
struct uvm_pdb uvm_page_tree_pdb(const struct uvm_page_tree *tree);
NV_STATUS uvm_page_tree_record_rm_root(struct uvm_page_tree *tree);
NV_STATUS uvm_page_tree_get_ptes(struct uvm_page_tree *tree, uint64_t page_size, uint64_t va, uint64_t size,
    struct uvm_pt_range *range);
void	uvm_page_tree_put_ptes(struct uvm_page_tree *tree, struct uvm_pt_range *range);
NV_STATUS uvm_page_tree_write_ptes(struct uvm_page_tree *tree, struct uvm_pt_range *range,
    uint32_t first, const uint64_t *ptes, uint32_t count, uvm_membar_t membar, struct uvm_tracker *tracker);
NV_STATUS uvm_page_tree_clear_ptes(struct uvm_page_tree *tree, struct uvm_pt_range *range,
    uvm_membar_t membar, struct uvm_tracker *tracker);
uint64_t uvm_page_tree_invalid_pte(const struct uvm_page_tree *tree, uint64_t page_size);
uint64_t uvm_page_tree_sparse_pte(const struct uvm_page_tree *tree);
uint64_t uvm_page_tree_sked_pte(const struct uvm_page_tree *tree);
uint64_t uvm_page_tree_pte(const struct uvm_page_tree *tree, uint64_t addr, uvm_aperture_t ap,
    uvm_prot_t prot, bool uncached);

/* a vector of PTE ranges covering [va, va+size) at one page size (05 section 2) */
struct uvm_pt_range_vec {
	struct uvm_pt_range	*ranges;
	unsigned		count;
	uint64_t		va;
	uint64_t		size;
	uint64_t		page_size;
};

NV_STATUS uvm_pt_range_vec_init(struct uvm_page_tree *tree, uint64_t va, uint64_t size, uint64_t page_size,
    bool intr, struct uvm_pt_range_vec *vec);
void	uvm_pt_range_vec_deinit(struct uvm_page_tree *tree, struct uvm_pt_range_vec *vec);
NV_STATUS uvm_pt_range_vec_write(struct uvm_page_tree *tree, struct uvm_pt_range_vec *vec, const uint64_t *ptes,
    uvm_membar_t membar, struct uvm_tracker *tracker);
NV_STATUS uvm_pt_range_vec_write_same(struct uvm_page_tree *tree, struct uvm_pt_range_vec *vec, uint64_t pte,
    uvm_membar_t membar, struct uvm_tracker *tracker);
NV_STATUS uvm_pt_range_vec_clear(struct uvm_page_tree *tree, struct uvm_pt_range_vec *vec, uvm_membar_t membar,
    struct uvm_tracker *tracker);
NV_STATUS uvm_pt_range_vec_clear_sub(struct uvm_page_tree *tree, struct uvm_pt_range_vec *vec, uint64_t va,
    uint64_t size, uvm_membar_t membar, struct uvm_tracker *tracker);

/* ---- GPU objects (02 section 3.2, 03 section 1, 06) ---- */

struct uvm_va_space;
struct uvm_gpu_va_space;
struct uvm_user_channel;

/* instance pointer -> user channel attribution (03 section 3.2, 08 section 3) */
struct uvm_instance_entry {
	TAILQ_ENTRY(uvm_instance_entry) link;
	uint64_t		inst_phys;
	uvm_aperture_t		aperture;
	struct uvm_user_channel	*channel;
};

struct uvm_fault_buffer {
	struct mtx		service_lock;	/* taken as a binary semaphore */
	bool			service_busy;
	uint32_t		entries;
	uint32_t		cached_get;
	uint32_t		cached_put;
	struct task		task;
	uint64_t		faults_fetched;
	uint64_t		faults_cancelled;
	uint64_t		faults_stale;
	uint64_t		replays;
	uint64_t		flushes;
	uint64_t		bottom_halves;
	uint64_t		overflows;
};

struct uvm_nonreplayable_buffer {
	struct mtx		service_lock;
	bool			service_busy;
	struct task		task;
	uint8_t			*packets;	/* nonReplayable.bufferSize bytes */
	uint64_t		faults_fetched;
	uint64_t		faults_reported;
	uint64_t		bottom_halves;
};

struct uvm_gpu {
	uvm_gpu_id_t		id;
	u_int			refs;		/* VA spaces that registered it plus retainers */
	NvProcessorUuid		uuid;
	char			name[UVM_GPU_NAME_LENGTH];
	struct uvm_core_device	*core_dev;	/* the device of the core, when it has one */
	UvmGpuInfo		info;
	UvmGpuPlatformInfo	platform;
	UvmGpuFbInfo		fb;
	enum uvm_fb_state	fb_state;	/* absent, usable or broken: the frame buffer information */
	UvmGpuEccInfo		ecc;
	UvmGpuNvlinkInfo	nvlink;
	UvmGpuCaps		caps;
	UvmGpuAddressSpaceInfo	as_info;
	UvmGpuCopyEnginesCaps	ce_caps;
	UvmGpuFaultInfo		fault_info;
	uvmGpuDeviceHandle	rm_device;
	uvmGpuAddressSpaceHandle rm_as;
	void			*pma;		/* NULL on a GPU without video memory: RM has no PMA there */
	const UvmPmaStatistics	*pma_stats;	/* NULL with it */
	bool			pma_callbacks_registered;
	bool			fault_info_valid;
	bool			owns_fault_intr;
	bool			in_table;
	const struct uvm_hal_generation *gen;
	uint32_t		mmu_version;
	uint64_t		max_channel_va;
	bus_dma_tag_t		dmat;		/* one-page tag: page tables and buffers the GPU reads */
	struct uvm_channel_manager *chmgr;
	struct taskqueue	*bh_tq;		/* bottom halves */
	struct taskqueue	*kill_tq;	/* report non-replayable faults to RM */
	struct mtx		isr_lock;	/* interrupt state */
	int			intr_disable_count;
	bool			isr_suspended;
	bool			replayable_intr_enabled;
	struct uvm_fault_buffer	replayable;
	struct uvm_nonreplayable_buffer nonreplayable;
	struct mtx		instance_lock;
	TAILQ_HEAD(, uvm_instance_entry) instances;
	uint64_t		isr_top_halves;
};

NV_STATUS uvm_gpu_retain_by_uuid(const NvProcessorUuid *uuid, struct uvm_gpu **out);
NV_STATUS uvm_gpu_retain_by_uuid_client(const NvProcessorUuid *uuid, const UvmGpuClientInfo *client,
    struct uvm_gpu **out);
NV_STATUS uvm_gpu_fault_top_half(struct uvm_gpu *gpu);
void	uvm_gpu_release(struct uvm_gpu *gpu);
struct uvm_gpu *uvm_gpu_lookup_uuid_locked(const NvProcessorUuid *uuid);
NV_STATUS uvm_gpu_isr_top_half(const NvProcessorUuid *uuid);
void	uvm_gpu_fault_buffer_flush(struct uvm_gpu *gpu);
void	uvm_gpu_instance_add(struct uvm_gpu *gpu, struct uvm_user_channel *ch);
void	uvm_gpu_instance_remove(struct uvm_gpu *gpu, struct uvm_user_channel *ch);
struct uvm_user_channel *uvm_gpu_instance_lookup(struct uvm_gpu *gpu, uint64_t inst_phys, uvm_aperture_t ap);
NV_STATUS uvm_gpu_suspend_all(void);
NV_STATUS uvm_gpu_resume_all(void);
NV_STATUS uvm_gpu_sysmem_alloc(struct uvm_gpu *gpu, size_t bytes, void **kva, bus_dmamap_t *map, bus_addr_t *bus);
void	uvm_gpu_sysmem_free(struct uvm_gpu *gpu, void *kva, bus_dmamap_t map, size_t bytes);
uint64_t uvm_gpu_dma_address(const struct uvm_gpu *gpu, bus_addr_t bus);

/* ---- the NVIDIA backend of the core (sys/uvm_backend_nvidia.c) ---- */

/* the backend that is registered with the core when the module loads */
extern struct uvm_core_backend uvm_nvidia_backend;

/* the status of the vendor that a core errno becomes */
NV_STATUS uvm_nvidia_core_status(int core_errno);

/* ---- VA spaces, ranges (03) ---- */

enum uvm_range_type {
	UVM_RANGE_MANAGED = 1,
	UVM_RANGE_EXTERNAL,
	UVM_RANGE_CHANNEL,
	UVM_RANGE_SEMA_POOL,
	UVM_RANGE_SKED
};

/*
 * The duplicated RM handle of an external mapping, shared by the parts a
 * mapping is split into (03 section 4.2: "the duplicated RM handle
 * (reference counted because splitting one mapping shares the handle)").
 * The count is a refcount (sys/refcount.h); the handle is freed with RM
 * when the last part is gone.
 */
struct uvm_ext_handle {
	u_int			refs;
	NvHandle		dup;
};

/* an RM allocation mapped on one GPU inside an external range (03 section 4.2) */
struct uvm_ext_mapping {
	TAILQ_ENTRY(uvm_ext_mapping) link;
	uint64_t		start, end;	/* inclusive */
	struct uvm_gpu		*gpu;
	struct uvm_ext_handle	*handle;	/* NULL for a sparse mapping */
	bool			is_sysmem;
	bool			l2_invalidate_at_unmap;
	struct uvm_pt_range_vec	ptes;
	struct uvm_tracker	tracker;
};

struct uvm_ext_gpu_state {
	TAILQ_ENTRY(uvm_ext_gpu_state) link;
	struct uvm_gpu		*gpu;
	struct sx		lock;		/* this GPU's mapping list */
	TAILQ_HEAD(, uvm_ext_mapping) mappings;
};

/* a semaphore pool's memory: shared by the range and the user mapping's pager object */
struct uvm_sema_mem {
	u_int			refs;
	bus_dma_tag_t		tag;
	bool			own_tag;
	bus_dmamap_t		map;
	void			*kva;
	bus_addr_t		bus;
	size_t			bytes;
	uint64_t		user_start;	/* the range's start: mmap offsets are absolute */
};

struct uvm_sema_mem *uvm_sema_mem_ref(struct uvm_sema_mem *mem);
void	uvm_sema_mem_unref(struct uvm_sema_mem *mem);

/*
 * The handle of a managed range's device pager object (uvm_managed_state.h
 * has the lifetime rules).  `range` and `state` are protected by
 * uvm.managed_lock; `mem` and `start` never change.  The object holds its
 * own reference on the memory, so the CPU view outlives the VA space.
 */
struct uvm_managed_obj {
	struct uvm_sema_mem	*mem;
	struct uvm_range	*range;		/* NULL once DETACHED */
	enum uvm_mobj_state	state;
	uint64_t		start;		/* the range's start: pager offsets are absolute */
};

/* a semaphore pool's mapping on one GPU */
struct uvm_sema_gpu_map {
	TAILQ_ENTRY(uvm_sema_gpu_map) link;
	struct uvm_gpu		*gpu;
	struct uvm_pt_range_vec	ptes;
	bool			cached;
};

struct uvm_range {
	struct uvm_core_range	core;		/* the interval: core.start, core.end */
	enum uvm_range_type	type;
	struct uvm_va_space	*vas;
	union {
		struct {
			TAILQ_HEAD(, uvm_ext_gpu_state) gpus;
		} ext;
		struct {
			struct uvm_sema_mem *mem;
			struct uvm_gpu_mask cached_gpus;	/* GPUs asked to map it cached (at most one) */
			TAILQ_HEAD(, uvm_sema_gpu_map) gpus;
			struct uvm_tracker tracker;
			struct uvm_managed_obj *obj;	/* UVM_RANGE_MANAGED only: the pager's handle */
		} sema;		/* a UVM_RANGE_MANAGED range is backed and mapped like a pool */
		struct {
			struct uvm_gpu	*gpu;
			struct uvm_pt_range_vec ptes;
		} sked;
		struct {
			struct uvm_gpu_va_space *gvas;
			NvP64		descriptor;
			uint32_t	resource_id;
			uint32_t	runlist_id;
			uint32_t	tsg_id;
			bool		tsg_channel;
			u_int		refs;
			bool		mapped;
			uvm_aperture_t	aperture;
			struct uvm_pt_range_vec ptes;
			struct uvm_tracker tracker;
		} chan;
	} u;
};

struct uvm_user_channel {
	TAILQ_ENTRY(uvm_user_channel) link;
	struct uvm_gpu_va_space	*gvas;
	NvHandle		hClient, hChannel;
	uvmGpuRetainedChannelHandle rm_retained;
	UvmGpuChannelInstanceInfo *info;
	uint64_t		inst_phys;
	uvm_aperture_t		inst_aperture;
	bool			stopped;
	bool			bound;
	bool			in_instance_table;
	struct uvm_range	*resources[UVM_GPU_CHANNEL_MAX_RESOURCES];
	unsigned		nresources;
	u_int			refs;
	struct uvm_tracker	clear_faulted_tracker;
};

struct uvm_gpu_va_space {
	struct uvm_va_space	*vas;
	struct uvm_gpu		*gpu;
	uvmGpuAddressSpaceHandle rm_as;
	UvmGpuAddressSpaceInfo	info;
	struct uvm_page_tree	*tree;
	bool			page_dir_set;
	bool			dead;
	bool			disallow_new_channels;
	TAILQ_HEAD(, uvm_user_channel) channels;
	u_int			refs;
};

struct uvm_va_space {
	TAILQ_ENTRY(uvm_va_space) link;
	struct sx		lock;		/* the VA space lock */
	struct sx		serialize;	/* writer serialisation (08 order 7) */
	struct vmspace		*vm;		/* the bound address space (a reference) */
	struct vmspace		*mm_users_vm;	/* the MM fd's users reference on it (03 section 2.1), or NULL */
	pid_t			owner_pid;
	uint64_t		init_flags;
	struct uvm_core_range_tree	ranges;
	struct uvm_gpu		*gpus[UVM_MAX_GPUS + 1];
	struct uvm_gpu_va_space	*gvas[UVM_MAX_GPUS + 1];
	struct uvm_gpu_mask	registered;
	struct uvm_gpu_mask	unregister_in_progress;
	struct uvm_gpu_mask	sys_wide_atomics;
	bool			user_channels_stopped;
	bool			disallow_new_registers;
	enum uvm_mm_state	mm_state;
	struct uvm_core_group_set	groups;
	u_int			refs;
};

/*
 * The ranges of a VA space live in the core's interval tree: a module
 * range starts with the core range, so the core's ranges are found, walked
 * and taken out through the four functions below.  The VA space lock stays
 * with the module; the core takes none.
 */
static inline struct uvm_range *
uvm_range_find(struct uvm_va_space *vas, uint64_t addr)
{
	struct uvm_core_range *r;

	r = uvm_core_range_find(&vas->ranges, addr);
	if (r == NULL)
		return (NULL);
	return (__containerof(r, struct uvm_range, core));
}

static inline struct uvm_range *
uvm_range_find_overlap(struct uvm_va_space *vas, uint64_t start, uint64_t end)
{
	struct uvm_core_range *r;

	r = uvm_core_range_find_overlap(&vas->ranges, start, end);
	if (r == NULL)
		return (NULL);
	return (__containerof(r, struct uvm_range, core));
}

/* the lowest range of the VA space, or NULL when it has none */
static inline struct uvm_range *
uvm_range_first(struct uvm_va_space *vas)
{
	struct uvm_core_range *r;

	r = uvm_core_range_first(&vas->ranges);
	if (r == NULL)
		return (NULL);
	return (__containerof(r, struct uvm_range, core));
}

/* the range that follows `r` in address order, or NULL when `r` is the last */
static inline struct uvm_range *
uvm_range_next(struct uvm_va_space *vas, struct uvm_range *r)
{
	struct uvm_core_range *n;

	n = uvm_core_range_next(&vas->ranges, &r->core);
	if (n == NULL)
		return (NULL);
	return (__containerof(n, struct uvm_range, core));
}

NV_STATUS uvm_va_space_create(uint64_t flags, struct uvm_va_space **out);
void	uvm_va_space_destroy(struct uvm_va_space *vas);
void	uvm_va_space_ref(struct uvm_va_space *vas);
void	uvm_va_space_unref(struct uvm_va_space *vas);
NV_STATUS uvm_va_space_mm_bind(struct uvm_va_space *vas, enum uvm_mm_caller caller);
void	uvm_va_space_mm_release(struct uvm_va_space *vas);
NV_STATUS uvm_range_create(struct uvm_va_space *vas, enum uvm_range_type type, uint64_t start, uint64_t end,
    struct uvm_range **out);
void	uvm_range_destroy(struct uvm_range *r);

/* VA space operations behind the ioctls (uvm_va_space.c) */
NV_STATUS uvm_vas_register_gpu(struct uvm_va_space *vas, UVM_REGISTER_GPU_PARAMS *p);
NV_STATUS uvm_vas_unregister_gpu(struct uvm_va_space *vas, const NvProcessorUuid *uuid);
NV_STATUS uvm_vas_register_gpu_va_space(struct uvm_va_space *vas, UVM_REGISTER_GPU_VASPACE_PARAMS *p);
NV_STATUS uvm_vas_unregister_gpu_va_space(struct uvm_va_space *vas, const NvProcessorUuid *uuid);
NV_STATUS uvm_vas_register_channel(struct uvm_va_space *vas, UVM_REGISTER_CHANNEL_PARAMS *p);
NV_STATUS uvm_vas_unregister_channel(struct uvm_va_space *vas, UVM_UNREGISTER_CHANNEL_PARAMS *p);
NV_STATUS uvm_vas_create_external_range(struct uvm_va_space *vas, uint64_t base, uint64_t length);
NV_STATUS uvm_vas_map_external_allocation(struct uvm_va_space *vas, UVM_MAP_EXTERNAL_ALLOCATION_PARAMS *p);
NV_STATUS uvm_vas_map_external_sparse(struct uvm_va_space *vas, UVM_MAP_EXTERNAL_SPARSE_PARAMS *p);
NV_STATUS uvm_vas_unmap_external(struct uvm_va_space *vas, UVM_UNMAP_EXTERNAL_PARAMS *p);
NV_STATUS uvm_vas_free(struct uvm_va_space *vas, uint64_t base, uint64_t length);
NV_STATUS uvm_vas_validate(struct uvm_va_space *vas, uint64_t base, uint64_t length);
NV_STATUS uvm_vas_map_dynamic_parallelism(struct uvm_va_space *vas, UVM_MAP_DYNAMIC_PARALLELISM_REGION_PARAMS *p);
NV_STATUS uvm_vas_create_managed(struct uvm_va_space *vas, uint64_t start, uint64_t size,
    struct uvm_managed_obj **out);
void	uvm_managed_obj_release(struct uvm_managed_obj *obj);
NV_STATUS uvm_vas_alloc_semaphore_pool(struct uvm_va_space *vas, UVM_ALLOC_SEMAPHORE_POOL_PARAMS *p);
NV_STATUS uvm_vas_set_sys_wide_atomics(struct uvm_va_space *vas, const NvProcessorUuid *uuid, bool enable);
NV_STATUS uvm_vas_pageable_on_gpu(struct uvm_va_space *vas, const NvProcessorUuid *uuid, NvBool *out);
NV_STATUS uvm_vas_create_range_group(struct uvm_va_space *vas, uint64_t *id);
NV_STATUS uvm_vas_destroy_range_group(struct uvm_va_space *vas, uint64_t id);
NV_STATUS uvm_vas_range_group_lookup(struct uvm_va_space *vas, uint64_t id);
NV_STATUS uvm_vas_processor_uuid_table(struct uvm_va_space *vas, NvU64 user_ptr, bool v2);
NV_STATUS uvm_vas_check_managed_interval(struct uvm_va_space *vas, uint64_t base, uint64_t length);
NV_STATUS uvm_vas_gpu_registered(struct uvm_va_space *vas, const NvProcessorUuid *uuid, struct uvm_gpu **out);
struct uvm_user_channel *uvm_user_channel_ref(struct uvm_user_channel *ch);
void	uvm_user_channel_unref(struct uvm_user_channel *ch);

/* ---- the module (uvm_main.c) ---- */

struct uvm_global {
	struct sx		pm_lock;	/* 08 order 1 */
	struct sx		lock;		/* 08 order 2 */
	struct sx		managed_lock;	/* managed range <-> pager object links; above every VA space lock */
	u_int			pager_objs;	/* live pager objects (pool and managed mappings): the module cannot unload under them */
	u_int			open_fds;	/* open files whose private data is not yet destroyed */
	bool			unloading;	/* set under `lock` once the unload is decided: no new open (uvm_unload_state.h) */
	struct mtx		gpu_table_lock;	/* irq-safe lookup for the top half */
	uvmGpuSessionHandle	rm_session;
	UvmPlatformInfo		platform;
	uint32_t		fatal_error;	/* 01 section 2.4; 0 is NV_OK; atomics only */
	struct uvm_gpu		*gpus[UVM_MAX_GPUS + 1];
	TAILQ_HEAD(, uvm_va_space) va_spaces;
	struct cdev		*cdev_main;
	struct cdev		*cdev_tools;
	bool			callbacks_registered;
	bool			suspended;
	/* tunables (01 section 1) */
	int			builtin_tests;
	int			enable_va_space_mm;
	int			gpfifo_entries;
	int			fault_batch_count;
	int			wait_timeout_ms;
	int			sysmem_gpfifo;
	/* Xid 71 diagnostics (default off, no behaviour change unless set) */
	int			debug_push;	/* log the first N pushes, GPFIFO entries, PUT and doorbell */
	int			init_probe;	/* staged channel initialisation probe (see uvm_channel.c) */
	int			init_probe_work; /* bit mask of first-work probe stages on channel 0 (see uvm_channel.c) */
	int			ce_bind_subch4;	/* also SET_OBJECT the copy class on subchannel 4 (Q-008) */
};

extern struct uvm_global uvm;

/*
 * A pager object or an open file is counted from its birth to the very end
 * of its destructor: done() is the last access to module state, so that the
 * unload, which waits for both counts to reach zero, never frees the module
 * under a destructor that is still returning (Track #2272).
 */
static inline void
uvm_pager_obj_new(void)
{
	atomic_add_int(&uvm.pager_objs, 1);
}

static inline void
uvm_pager_obj_done(void)
{
	KASSERT(atomic_load_int(&uvm.pager_objs) > 0, ("uvm pager object count underflow"));
	atomic_subtract_int(&uvm.pager_objs, 1);
}

static inline NV_STATUS
uvm_global_status(void)
{
	return (atomic_load_acq_32(&uvm.fatal_error));
}

void	uvm_global_set_fatal(NV_STATUS status, const char *why);

/* fd state machine (01 section 3.2) */
struct uvm_fd {
	volatile uintptr_t	state;		/* 0, UVM_FD_INITIALIZING, or a tagged VA space pointer */
	struct cdev		*cdev;
};

#define	UVM_FD_INITIALIZING	((uintptr_t)1)
#define	UVM_FD_TAG_MM		((uintptr_t)2)
#define	UVM_FD_TAG_MASK		((uintptr_t)7)

static inline struct uvm_va_space *
uvm_fd_va_space(const struct uvm_fd *fd)
{
	uintptr_t s = atomic_load_acq_ptr(&fd->state);

	if (s <= UVM_FD_INITIALIZING || (s & UVM_FD_TAG_MM) != 0)
		return (NULL);
	return ((struct uvm_va_space *)(s & ~UVM_FD_TAG_MASK));
}

static inline struct uvm_va_space *
uvm_fd_mm_va_space(const struct uvm_fd *fd)
{
	uintptr_t s = atomic_load_acq_ptr(&fd->state);

	if (s <= UVM_FD_INITIALIZING || (s & UVM_FD_TAG_MM) == 0)
		return (NULL);
	return ((struct uvm_va_space *)(s & ~UVM_FD_TAG_MASK));
}

int	uvm_ioctl_dispatch(struct uvm_fd *fd, bool tools_node, uint32_t request, void *user_ptr,
    struct thread *td);
int	uvm_mmap_single(struct cdev *cdev, vm_ooffset_t *offset, vm_size_t size, vm_object_t *object, int nprot);
int	uvm_tools_ioctl(struct uvm_fd *fd, uint32_t request, void *user_ptr);

/* uvm_faults.c */
void	uvm_gpu_fault_buffers_init(struct uvm_gpu *gpu);
NV_STATUS uvm_gpu_fault_buffers_start(struct uvm_gpu *gpu);
void	uvm_gpu_fault_buffers_stop(struct uvm_gpu *gpu);
void	uvm_gpu_replayable_intr_enable(struct uvm_gpu *gpu);
void	uvm_gpu_replayable_intr_disable(struct uvm_gpu *gpu);

#endif /* _UVM_H_ */
