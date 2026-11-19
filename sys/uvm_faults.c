/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_faults.c - the replayable fault buffer UVM owns and the
 * non-replayable shadow buffer RM fills: interrupt control, the top half,
 * the bottom halves, and the flush.  uvm-spec r1, 06-faults.md sections 2,
 * 3, 5 and 6; 12-hal-per-generation.md section 5 for the entry format.
 *
 * Phase-1 behaviour (13 section "Phase 1"): there is no managed memory, so
 * every replayable fault is fatal to its context.  The bottom half fetches
 * a batch, attributes each entry to a registered user channel through the
 * instance pointer table, pushes a cancel by VA for every attributed fault
 * (the cancel needs the faulting address space's page directory), a replay
 * so nothing stays stalled, waits, and then clears and re-enables the
 * interrupt.  Entries whose instance pointer is unknown belong to channels
 * already unregistered (stale) and are dropped by the replay.  Every
 * non-replayable fault is handed to RM, which kills the channel.
 *
 * Interrupt control (06 section 2, Turing and later): enable = mask to
 * enable-set; disable = mask to enable-clear then mask to the pending
 * register; after servicing = mask to pending, write barrier, rewrite GET
 * so a fault that arrived meanwhile raises a new pulse.
 */

#include "uvm.h"

static inline void
uvm_reg_write(volatile NvU32 *reg, uint32_t value)
{
	*reg = value;
}

static inline uint32_t
uvm_reg_read(volatile NvU32 *reg)
{
	return (*reg);
}

static inline volatile uint32_t *
uvm_fault_entry_ptr(struct uvm_gpu *gpu, uint32_t index)
{
	return ((volatile uint32_t *)((uint8_t *)gpu->fault_info.replayable.bufferAddress +
	    (size_t)index * UVM_FAULT_ENTRY_BYTES));
}

static inline bool
uvm_fault_entry_is_valid(struct uvm_gpu *gpu, uint32_t index)
{
	return ((uvm_fault_entry_ptr(gpu, index)[UVM_FAULT_ENTRY_VALID_WORD] & UVM_FAULT_ENTRY_VALID_BIT) != 0);
}

/* ---- interrupt control ---- */

void
uvm_gpu_replayable_intr_enable(struct uvm_gpu *gpu)
{
	struct uvm_fault_buffer *fb = &gpu->replayable;

	mtx_lock(&gpu->isr_lock);
	if (gpu->intr_disable_count > 0)
		gpu->intr_disable_count--;
	if (gpu->intr_disable_count == 0 && !gpu->replayable_intr_enabled && gpu->fault_info_valid) {
		/* clear a pending pulse, re-evaluate the condition through GET, then enable */
		uvm_reg_write(gpu->fault_info.replayable.pPmcIntr, gpu->fault_info.replayable.replayableFaultMask);
		wmb();
		uvm_reg_write(gpu->fault_info.replayable.pFaultBufferGet, uvm_fault_buffer_get_write(fb->cached_get));
		uvm_reg_write(gpu->fault_info.replayable.pPmcIntrEnSet, gpu->fault_info.replayable.replayableFaultMask);
		gpu->replayable_intr_enabled = true;
	}
	mtx_unlock(&gpu->isr_lock);
}

void
uvm_gpu_replayable_intr_disable(struct uvm_gpu *gpu)
{
	mtx_lock(&gpu->isr_lock);
	gpu->intr_disable_count++;
	if (gpu->replayable_intr_enabled && gpu->fault_info_valid) {
		uvm_reg_write(gpu->fault_info.replayable.pPmcIntrEnClear,
		    gpu->fault_info.replayable.replayableFaultMask);
		uvm_reg_write(gpu->fault_info.replayable.pPmcIntr, gpu->fault_info.replayable.replayableFaultMask);
		gpu->replayable_intr_enabled = false;
	}
	mtx_unlock(&gpu->isr_lock);
}

/* ---- the service locks: binary semaphores with a try operation ---- */

static bool
uvm_service_trylock(struct mtx *m, bool *busy)
{
	bool got;

	mtx_lock(m);
	got = !*busy;
	if (got)
		*busy = true;
	mtx_unlock(m);
	return (got);
}

static void
uvm_service_lock(struct mtx *m, bool *busy)
{
	mtx_lock(m);
	while (*busy)
		msleep(busy, m, 0, "uvmsvc", 0);
	*busy = true;
	mtx_unlock(m);
}

static void
uvm_service_unlock(struct mtx *m, bool *busy)
{
	mtx_lock(m);
	*busy = false;
	wakeup(busy);
	mtx_unlock(m);
}

/* ---- the top half (06 section 3) ---- */

static bool
uvm_replayable_pending(struct uvm_gpu *gpu)
{
	struct uvm_fault_buffer *fb = &gpu->replayable;
	uint32_t put;

	if (fb->cached_get != fb->cached_put)
		return (true);
	if (uvm_fault_entry_is_valid(gpu, fb->cached_get))
		return (true);
	put = uvm_fault_buffer_get_ptr(uvm_reg_read(gpu->fault_info.replayable.pFaultBufferPut));
	if (put != fb->cached_get) {
		fb->cached_put = put;
		return (true);
	}
	return (false);
}

NV_STATUS
uvm_gpu_fault_top_half(struct uvm_gpu *gpu)
{
	NV_STATUS status = NV_ERR_NO_INTR_PENDING;
	NvBool nr_pending = NV_FALSE;
	bool suspended;

	mtx_lock(&gpu->isr_lock);
	suspended = gpu->isr_suspended || !gpu->in_table;
	mtx_unlock(&gpu->isr_lock);
	if (suspended)
		return (NV_ERR_NO_INTR_PENDING);

	/* replayable: take the service lock, disable the interrupt, schedule the bottom half */
	if (uvm_service_trylock(&gpu->replayable.service_lock, &gpu->replayable.service_busy)) {
		if (uvm_replayable_pending(gpu)) {
			uvm_gpu_replayable_intr_disable(gpu);
			taskqueue_enqueue(gpu->bh_tq, &gpu->replayable.task);
			status = NV_OK;
		} else {
			uvm_service_unlock(&gpu->replayable.service_lock, &gpu->replayable.service_busy);
		}
	} else if (uvm_replayable_pending(gpu)) {
		status = NV_WARN_MORE_PROCESSING_REQUIRED;
	}

	/* non-replayable: RM says whether its shadow buffer holds entries */
	if (nvUvmInterfaceHasPendingNonReplayableFaults(&gpu->fault_info, &nr_pending) == NV_OK && nr_pending) {
		if (uvm_service_trylock(&gpu->nonreplayable.service_lock, &gpu->nonreplayable.service_busy)) {
			taskqueue_enqueue(gpu->bh_tq, &gpu->nonreplayable.task);
			status = NV_OK;
		} else if (status != NV_OK) {
			status = NV_WARN_MORE_PROCESSING_REQUIRED;
		}
	}
	return (status);
}

/* ---- fetching ---- */

/*
 * uvm_fault_fetch - copy up to `max` entries from GET to PUT into `out`,
 * clearing each one's valid bit and advancing GET.  Hardware may advance PUT
 * before an entry body is complete, so each entry's valid bit is awaited
 * with a bounded spin.  Returns the number fetched.
 */
static uint32_t
uvm_fault_fetch(struct uvm_gpu *gpu, struct uvm_fault_entry *out, uint32_t max, bool update_put)
{
	struct uvm_fault_buffer *fb = &gpu->replayable;
	volatile uint32_t *e;
	uint32_t raw[UVM_FAULT_ENTRY_WORDS];
	uint32_t n = 0, put_reg, i, spins;

	put_reg = uvm_reg_read(gpu->fault_info.replayable.pFaultBufferPut);
	if (update_put || fb->cached_put == fb->cached_get)
		fb->cached_put = uvm_fault_buffer_get_ptr(put_reg);
	while (n < max && (fb->cached_get != fb->cached_put || uvm_fault_entry_is_valid(gpu, fb->cached_get))) {
		if (uvm_global_status() != NV_OK)
			break;
		e = uvm_fault_entry_ptr(gpu, fb->cached_get);
		for (spins = 0; (e[UVM_FAULT_ENTRY_VALID_WORD] & UVM_FAULT_ENTRY_VALID_BIT) == 0 && spins < 100000;
		    spins++)
			cpu_spinwait();
		if ((e[UVM_FAULT_ENTRY_VALID_WORD] & UVM_FAULT_ENTRY_VALID_BIT) == 0) {
			uvm_log_ratelimited("gpu%u: fault entry %u never became valid", gpu->id, fb->cached_get);
			break;
		}
		for (i = 0; i < UVM_FAULT_ENTRY_WORDS; i++)
			raw[i] = e[i];
		uvm_fault_entry_decode(raw, &out[n]);
		e[UVM_FAULT_ENTRY_VALID_WORD] = 0;
		n++;
		fb->cached_get = (fb->cached_get + 1) % fb->entries;
		uvm_reg_write(gpu->fault_info.replayable.pFaultBufferGet, fb->cached_get);
	}
	fb->faults_fetched += n;
	return (n);
}

/* ---- the replayable bottom half (06 section 3, phase 1: cancel everything) ---- */

static const char *
uvm_engine_type_name(uvm_mmu_engine_type_t t)
{
	switch (t) {
	case UVM_MMU_ENGINE_GRAPHICS: return ("graphics");
	case UVM_MMU_ENGINE_CE: return ("copy engine");
	case UVM_MMU_ENGINE_HOST: return ("host");
	case UVM_MMU_ENGINE_BAR1: return ("BAR1");
	case UVM_MMU_ENGINE_BAR2: return ("BAR2");
	default: return ("unknown engine");
	}
}

static void
uvm_replayable_bottom_half(void *arg, int pending)
{
	struct uvm_gpu *gpu = arg;
	struct uvm_fault_buffer *fb = &gpu->replayable;
	struct uvm_fault_entry *batch;
	struct uvm_push push;
	uint32_t n, i, batches;
	uint32_t get_reg;

	(void)pending;
	fb->bottom_halves++;
	batch = malloc(sizeof(*batch) * uvm.fault_batch_count, M_NVIDIA_UVM, M_WAITOK);

	get_reg = uvm_reg_read(gpu->fault_info.replayable.pFaultBufferGet);
	if (uvm_fault_buffer_get_overflow(get_reg)) {
		fb->overflows++;
		uvm_log_ratelimited("gpu%u: replayable fault buffer overflow", gpu->id);
	}

	for (batches = 0; batches < 20; batches++) {
		n = uvm_fault_fetch(gpu, batch, (uint32_t)uvm.fault_batch_count, true);
		if (n == 0)
			break;
		if (uvm_push_begin(gpu, &push, NULL) != NV_OK)
			break;
		for (i = 0; i < n; i++) {
			struct uvm_fault_entry *f = &batch[i];
			struct uvm_user_channel *ch;
			struct uvm_pdb pdb;
			uint64_t va = uvm_canonical_address(f->fault_address, 48);

			ch = uvm_gpu_instance_lookup(gpu, f->inst_phys, f->inst_aperture);
			if (ch == NULL) {
				fb->faults_stale++;
				continue;
			}
			pdb = uvm_page_tree_pdb(ch->gvas->tree);
			uvm_log_ratelimited("gpu%u: fatal fault at %#jx from %s (engine %u, type %u, access %u, "
			    "channel %u/%u): no managed memory in this phase; cancelled", gpu->id, (uintmax_t)va,
			    uvm_engine_type_name(uvm_hal_engine_type(gpu->gen, f->mmu_engine_id)), f->mmu_engine_id,
			    f->fault_type, f->access_type, ch->info->runlistId, ch->info->chId);
			UVM_PUSH(&push, uvm_host_cancel_faults_va, &pdb, f->fault_address, f->mmu_engine_id,
			    UVM_CANCEL_ACCESS_ALL, gpu->gen->cancel_engine_id_bits);
			fb->faults_cancelled++;
			uvm_user_channel_unref(ch);
		}
		/* at least one replay per bottom half so the hardware never stays stalled */
		UVM_PUSH(&push, uvm_host_replay_faults, UVM_REPLAY_START);
		fb->replays++;
		(void)uvm_push_end_and_wait(&push);
	}
	if (batches == 0) {
		/* nothing was fetched: still replay so stalled translations retry */
		if (uvm_push_begin(gpu, &push, NULL) == NV_OK) {
			UVM_PUSH(&push, uvm_host_replay_faults, UVM_REPLAY_START);
			fb->replays++;
			(void)uvm_push_end_and_wait(&push);
		}
	}
	free(batch, M_NVIDIA_UVM);

	/* re-enable (clears the pulse and rewrites GET) and release the service lock */
	uvm_gpu_replayable_intr_enable(gpu);
	uvm_service_unlock(&fb->service_lock, &fb->service_busy);
}

/* ---- the non-replayable bottom half: every fault goes to RM (06 section 3) ---- */

struct uvm_kill_job {
	struct task	task;
	struct uvm_gpu	*gpu;
	uint32_t	count;
	uint8_t		packets[];
};

static void
uvm_kill_channel_work(void *arg, int pending)
{
	struct uvm_kill_job *job = arg;
	uint32_t i;

	(void)pending;
	for (i = 0; i < job->count; i++) {
		NV_STATUS s = nvUvmInterfaceReportNonReplayableFault(job->gpu->rm_device,
		    job->packets + (size_t)i * UVM_FAULT_ENTRY_BYTES);

		if (s != NV_OK)
			uvm_log_ratelimited("gpu%u: RM refused a non-replayable fault report: %s", job->gpu->id,
			    uvm_status_name(s));
		job->gpu->nonreplayable.faults_reported++;
	}
	free(job, M_NVIDIA_UVM);
}

static void
uvm_nonreplayable_bottom_half(void *arg, int pending)
{
	struct uvm_gpu *gpu = arg;
	struct uvm_nonreplayable_buffer *nb = &gpu->nonreplayable;
	struct uvm_kill_job *job;
	NvU32 n;
	NV_STATUS status;
	uint32_t i;

	(void)pending;
	nb->bottom_halves++;
	for (;;) {
		n = 0;
		status = nvUvmInterfaceGetNonReplayableFaults(&gpu->fault_info, nb->packets, &n);
		if (status != NV_OK || n == 0)
			break;
		nb->faults_fetched += n;
		for (i = 0; i < n; i++) {
			struct uvm_fault_entry f;

			uvm_fault_entry_decode((const uint32_t *)(nb->packets + (size_t)i * UVM_FAULT_ENTRY_BYTES), &f);
			uvm_log_ratelimited("gpu%u: non-replayable fault at %#jx from %s (engine %u, type %u): "
			    "reported to RM", gpu->id, (uintmax_t)uvm_canonical_address(f.fault_address, 48),
			    uvm_engine_type_name(uvm_hal_engine_type(gpu->gen, f.mmu_engine_id)), f.mmu_engine_id,
			    f.fault_type);
		}
		job = malloc(sizeof(*job) + (size_t)n * UVM_FAULT_ENTRY_BYTES, M_NVIDIA_UVM, M_WAITOK);
		job->gpu = gpu;
		job->count = n;
		memcpy(job->packets, nb->packets, (size_t)n * UVM_FAULT_ENTRY_BYTES);
		TASK_INIT(&job->task, 0, uvm_kill_channel_work, job);
		taskqueue_enqueue(gpu->kill_tq, &job->task);
	}
	uvm_service_unlock(&nb->service_lock, &nb->service_busy);
}

/* ---- flush (06 section 5) ---- */

void
uvm_gpu_fault_buffer_flush(struct uvm_gpu *gpu)
{
	struct uvm_fault_buffer *fb = &gpu->replayable;
	struct uvm_fault_entry *batch;
	struct uvm_push push;
	uint32_t n;

	if (!gpu->fault_info_valid)
		return;
	uvm_service_lock(&fb->service_lock, &fb->service_busy);
	uvm_gpu_replayable_intr_disable(gpu);
	fb->flushes++;
	batch = malloc(sizeof(*batch) * 64, M_NVIDIA_UVM, M_WAITOK);
	do {
		n = uvm_fault_fetch(gpu, batch, 64, true);
	} while (n == 64);
	free(batch, M_NVIDIA_UVM);
	if (uvm_push_begin(gpu, &push, NULL) == NV_OK) {
		UVM_PUSH(&push, uvm_host_replay_faults, UVM_REPLAY_START);
		fb->replays++;
		(void)uvm_push_end_and_wait(&push);
	}
	uvm_gpu_replayable_intr_enable(gpu);
	uvm_service_unlock(&fb->service_lock, &fb->service_busy);
}

/* ---- lifetime ---- */

void
uvm_gpu_fault_buffers_init(struct uvm_gpu *gpu)
{
	mtx_init(&gpu->replayable.service_lock, "nvidia_uvm_rfault", NULL, MTX_DEF);
	mtx_init(&gpu->nonreplayable.service_lock, "nvidia_uvm_nrfault", NULL, MTX_DEF);
	TASK_INIT(&gpu->replayable.task, 0, uvm_replayable_bottom_half, gpu);
	TASK_INIT(&gpu->nonreplayable.task, 0, uvm_nonreplayable_bottom_half, gpu);
	gpu->intr_disable_count = 1;	/* enabled explicitly once the GPU is in the table */
}

NV_STATUS
uvm_gpu_fault_buffers_start(struct uvm_gpu *gpu)
{
	struct uvm_fault_buffer *fb = &gpu->replayable;
	char name[32];

	fb->entries = gpu->fault_info.replayable.bufferSize / UVM_FAULT_ENTRY_BYTES;
	if (fb->entries == 0 || gpu->fault_info.replayable.bufferAddress == NULL)
		return (NV_ERR_INVALID_STATE);
	fb->cached_get = uvm_fault_buffer_get_ptr(uvm_reg_read(gpu->fault_info.replayable.pFaultBufferGet));
	fb->cached_put = uvm_fault_buffer_get_ptr(uvm_reg_read(gpu->fault_info.replayable.pFaultBufferPut));
	gpu->nonreplayable.packets = malloc(gpu->fault_info.nonReplayable.bufferSize, M_NVIDIA_UVM, M_WAITOK | M_ZERO);

	snprintf(name, sizeof(name), "uvm%u_bh", gpu->id);
	gpu->bh_tq = taskqueue_create(name, M_WAITOK, taskqueue_thread_enqueue, &gpu->bh_tq);
	taskqueue_start_threads(&gpu->bh_tq, 1, PI_SOFT, "%s", name);
	snprintf(name, sizeof(name), "uvm%u_kill", gpu->id);
	gpu->kill_tq = taskqueue_create(name, M_WAITOK, taskqueue_thread_enqueue, &gpu->kill_tq);
	taskqueue_start_threads(&gpu->kill_tq, 1, PI_SOFT, "%s", name);
	uvm_log("gpu%u: replayable fault buffer of %u entries, non-replayable shadow buffer of %u bytes", gpu->id,
	    fb->entries, gpu->fault_info.nonReplayable.bufferSize);
	return (NV_OK);
}

void
uvm_gpu_fault_buffers_stop(struct uvm_gpu *gpu)
{
	mtx_lock(&gpu->isr_lock);
	gpu->isr_suspended = true;
	mtx_unlock(&gpu->isr_lock);
	if (gpu->bh_tq != NULL) {
		taskqueue_drain_all(gpu->bh_tq);
		taskqueue_free(gpu->bh_tq);
		gpu->bh_tq = NULL;
	}
	if (gpu->kill_tq != NULL) {
		taskqueue_drain_all(gpu->kill_tq);
		taskqueue_free(gpu->kill_tq);
		gpu->kill_tq = NULL;
	}
	free(gpu->nonreplayable.packets, M_NVIDIA_UVM);
	gpu->nonreplayable.packets = NULL;
	if (mtx_initialized(&gpu->replayable.service_lock))
		mtx_destroy(&gpu->replayable.service_lock);
	if (mtx_initialized(&gpu->nonreplayable.service_lock))
		mtx_destroy(&gpu->nonreplayable.service_lock);
}
