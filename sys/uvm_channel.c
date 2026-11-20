/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_channel.c - UVM-owned copy-engine channels: the channel manager,
 * the pushbuffer, pushes, tracking semaphores and trackers.  uvm-spec r1,
 * 04-channels-and-copy-engine.md sections 1.2, 2 and 3; 09 section 2.5 for
 * the barriers.
 *
 * Phase-1 shape.  One channel pool on one copy engine (the first usable
 * one: supported, not synchronous with graphics, able to reach system
 * memory), two channels, serving every channel type; the preference table
 * of 04 section 2 is a performance refinement for a later phase.  The
 * pushbuffer is 16 MB of RM system memory, divided into 128 slots of the
 * maximum push size (128 KB): a push takes one slot and the slot returns
 * when the push's tracking value completes.  That is simpler than the
 * chunk rings the spec describes and keeps every property the rest of the
 * module relies on (pushes never wrap, at most one push per slot, space
 * returned by completion).
 *
 * GPFIFO and PUT live in system memory by default (tunable): the CPU then
 * writes them with ordinary stores, reads the dummy BAR1 word so posted
 * writes to the GPU have landed, and rings the doorbell after a full
 * barrier (a dsb sy on arm64, since the doorbell is Device memory).
 */

#include "uvm.h"
#include "uvm_debug.h"

#include <sys/sbuf.h>

#define	UVM_CHANNEL_INIT_WORDS	16

/*
 * hw.nvidia_uvm.debug_push diagnostics: the method stream, GPFIFO entry,
 * GP_PUT and doorbell of the first N pushes, and the GPU VAs involved.
 * Reads state only; it never changes what is sent to the GPU.
 */
static unsigned uvm_debug_pushes_logged;

static bool
uvm_debug_push_enabled(void)
{
	return (uvm.debug_push > 0 && uvm_debug_pushes_logged < (unsigned)uvm.debug_push &&
	    uvm_debug_pushes_logged < UVM_DEBUG_PUSH_MAX);
}

static void
uvm_debug_log_stream(const struct uvm_channel *ch, const struct uvm_push *push, uint32_t words)
{
	char line[200];
	uint32_t pos = 0;
	size_t used;

	uvm_log("debug_push: ch%u push #%u: pushbuffer slot %d va %#jx, %u words", ch->index,
	    uvm_debug_pushes_logged, push->slot, (uintmax_t)push->gpu_va, words);
	while (pos < words) {
		used = uvm_debug_describe_method(line, sizeof(line), push->begin + pos, words - pos);
		uvm_log("debug_push:   +%#06x  %s", pos * 4, line);
		pos += (uint32_t)used;
	}
}

static void
uvm_debug_log_gpfifo(const struct uvm_channel *ch, uint32_t index, uint64_t entry)
{
	char line[200];

	uvm_debug_describe_gpfifo(line, sizeof(line), entry);
	uvm_log("debug_push: ch%u GPFIFO[%u] @cpu %p = %s", ch->index, index,
	    (const void *)&ch->info.gpFifoEntries[index], line);
}

static void
uvm_debug_log_channel(const struct uvm_channel *ch)
{
	const UvmGpuChannelInfo *i = &ch->info;

	uvm_log("debug_push: ch%u hw runlist %u channel %u class %#x: GPFIFO va %#jx (cpu %p, %u entries), "
	    "GP_PUT va %#jx (cpu %p), GP_GET va %#jx, doorbell va %#jx cpu %p token %#x, dummy BAR1 %p",
	    ch->index, i->hwRunlistId, i->hwChannelId, i->channelClassNum, (uintmax_t)i->gpFifoGpuVa,
	    (void *)i->gpFifoEntries, i->numGpFifoEntries, (uintmax_t)i->gpPutGpuVa, (void *)i->gpPut,
	    (uintmax_t)i->gpGetGpuVa, (uintmax_t)i->workSubmissionOffsetGpuVa,
	    (void *)i->workSubmissionOffset, i->workSubmissionToken, (void *)i->dummyBar1Mapping);
	uvm_log("debug_push: ch%u tracking semaphore va %#jx cpu %p; pushbuffer va %#jx cpu %p size %#x; "
	    "GPFIFO in %s memory", ch->index, (uintmax_t)ch->sema_gpu_va, (const void *)ch->sema_cpu,
	    (uintmax_t)ch->mgr->pb.gpu_va, (void *)ch->mgr->pb.cpu, UVM_PUSHBUFFER_SIZE,
	    uvm.sysmem_gpfifo ? "system" : "video");
}

static inline void
uvm_doorbell_barrier(void)
{
#if defined(__aarch64__)
	__asm __volatile("dsb sy" ::: "memory");
#else
	mb();
#endif
}

/* ---- trackers (04 section 2.4) ---- */

void
uvm_tracker_init(struct uvm_tracker *t)
{
	t->entries = &t->inline_entry;
	t->count = 0;
	t->capacity = 1;
}

void
uvm_tracker_deinit(struct uvm_tracker *t)
{
	if (t->entries != &t->inline_entry)
		free(t->entries, M_NVIDIA_UVM);
	uvm_tracker_init(t);
}

/*
 * uvm_tracker_add - record (channel, value), keeping the maximum per channel.
 * Time:  O(n) over the tracker's entries.
 */
int
uvm_tracker_add(struct uvm_tracker *t, struct uvm_channel *ch, uint64_t value)
{
	struct uvm_tracker_entry *n;
	unsigned i, cap;

	for (i = 0; i < t->count; i++) {
		if (t->entries[i].channel == ch) {
			if (t->entries[i].value < value)
				t->entries[i].value = value;
			return (0);
		}
	}
	if (t->count == t->capacity) {
		cap = t->capacity * 2;
		n = malloc(cap * sizeof(*n), M_NVIDIA_UVM, M_WAITOK);
		memcpy(n, t->entries, t->count * sizeof(*n));
		if (t->entries != &t->inline_entry)
			free(t->entries, M_NVIDIA_UVM);
		t->entries = n;
		t->capacity = cap;
	}
	t->entries[t->count].channel = ch;
	t->entries[t->count].value = value;
	t->count++;
	return (0);
}

int
uvm_tracker_add_tracker(struct uvm_tracker *dst, const struct uvm_tracker *src)
{
	unsigned i;

	for (i = 0; i < src->count; i++)
		uvm_tracker_add(dst, src->entries[i].channel, src->entries[i].value);
	return (0);
}

void
uvm_tracker_remove_completed(struct uvm_tracker *t)
{
	unsigned i = 0;

	while (i < t->count) {
		if (uvm_channel_value_complete(t->entries[i].channel, t->entries[i].value))
			t->entries[i] = t->entries[--t->count];
		else
			i++;
	}
}

static NV_STATUS uvm_channel_wait_value(struct uvm_channel *ch, uint64_t value);

NV_STATUS
uvm_tracker_wait(struct uvm_tracker *t)
{
	NV_STATUS status = NV_OK, s;
	unsigned i;

	for (i = 0; i < t->count; i++) {
		s = uvm_channel_wait_value(t->entries[i].channel, t->entries[i].value);
		if (s != NV_OK && status == NV_OK)
			status = s;
	}
	if (status == NV_OK)
		t->count = 0;
	return (status);
}

/* ---- tracking semaphores and progress (04 section 2.3) ---- */

/*
 * uvm_channel_observe - fold the 32-bit payload the GPU released into the
 * 64-bit completed value: a payload below the mirror's low word means a
 * wrap; a jump above twice the largest GPFIFO count is corruption.
 * Caller holds ch->lock.
 */
static void
uvm_channel_observe(struct uvm_channel *ch)
{
	uint32_t payload = atomic_load_acq_32(ch->sema_cpu);
	uint64_t completed = ch->completed_value;
	uint32_t low = (uint32_t)completed;
	uint64_t next;

	if (payload == low)
		return;
	next = (completed & ~0xFFFFFFFFULL) | payload;
	if (payload < low)
		next += 1ULL << 32;
	if (next - completed > 2ULL * (1ULL << 20)) {
		uvm_global_set_fatal(NV_ERR_INVALID_STATE, "tracking semaphore jumped: GPU wrote a corrupt value");
		return;
	}
	ch->completed_value = next;
}

static void
uvm_pushbuffer_free_slot(struct uvm_pushbuffer *pb, int slot)
{
	mtx_lock(&pb->lock);
	pb->slot_busy[slot / 64] &= ~(1ULL << (slot % 64));
	mtx_unlock(&pb->lock);
}

/*
 * uvm_channel_update_progress - retire GPFIFO entries whose tracking value
 * the semaphore has passed, returning their pushbuffer slots; at most 8
 * per call so the pool lock is held briefly.
 */
void
uvm_channel_update_progress(struct uvm_channel *ch)
{
	int freed[8], nfreed = 0, i;

	mtx_lock(&ch->lock);
	uvm_channel_observe(ch);
	while (ch->pending > 0 && nfreed < 8) {
		struct uvm_gpfifo_entry_state *e = &ch->entries[ch->gpu_get];

		if (e->tracking_value > ch->completed_value)
			break;
		if (e->pb_slot >= 0)
			freed[nfreed++] = e->pb_slot;
		e->pb_slot = -1;
		ch->gpu_get = (ch->gpu_get + 1) % ch->num_gpfifo;
		ch->pending--;
	}
	mtx_unlock(&ch->lock);
	for (i = 0; i < nfreed; i++)
		uvm_pushbuffer_free_slot(&ch->mgr->pb, freed[i]);
}

bool
uvm_channel_value_complete(struct uvm_channel *ch, uint64_t value)
{
	bool done;

	mtx_lock(&ch->lock);
	uvm_channel_observe(ch);
	done = ch->completed_value >= value;
	mtx_unlock(&ch->lock);
	return (done);
}

/* an RM error notifier whose status half-word is non-zero: robust-channel recovery hit us */
static bool
uvm_channel_has_error(const struct uvm_channel *ch)
{
	return (ch->info.errorNotifier != NULL && ch->info.errorNotifier->status != 0);
}

NV_STATUS
uvm_channel_check_errors(struct uvm_gpu *gpu)
{
	unsigned i;

	if (gpu->chmgr == NULL)
		return (uvm_global_status());
	for (i = 0; i < gpu->chmgr->nchannels; i++) {
		if (uvm_channel_has_error(&gpu->chmgr->channels[i])) {
			uvm_global_set_fatal(NV_ERR_RC_ERROR, "RM reported a channel error on a UVM channel");
			break;
		}
	}
	return (uvm_global_status());
}

/*
 * uvm_channel_wait_value - spin and yield until the channel completed
 * `value`, a channel or global error is recorded, or the tunable timeout
 * passes (then a fatal error: a stuck UVM channel never recovers).
 */
static NV_STATUS
uvm_channel_wait_value(struct uvm_channel *ch, uint64_t value)
{
	sbintime_t deadline = sbinuptime() + SBT_1MS * (sbintime_t)uvm.wait_timeout_ms;
	unsigned spins = 0;

	for (;;) {
		if (uvm_channel_value_complete(ch, value)) {
			uvm_channel_update_progress(ch);
			return (NV_OK);
		}
		if (uvm_channel_has_error(ch))
			uvm_global_set_fatal(NV_ERR_RC_ERROR, "RM reported a channel error on a UVM channel");
		if (uvm_global_status() != NV_OK)
			return (uvm_global_status());
		if (sbinuptime() > deadline) {
			uvm_global_set_fatal(NV_ERR_TIMEOUT, "a UVM channel did not complete its work in time");
			return (NV_ERR_TIMEOUT);
		}
		if (++spins < 1000)
			cpu_spinwait();
		else
			pause_sbt("uvmwait", SBT_1US * 50, 0, C_HARDCLOCK);
	}
}

/* ---- the pushbuffer (04 section 2.1, simplified to slots) ---- */

static int
uvm_pushbuffer_alloc_slot(struct uvm_pushbuffer *pb)
{
	unsigned i, b;

	mtx_lock(&pb->lock);
	for (i = 0; i < UVM_PUSHBUFFER_SLOTS; i++) {
		b = i / 64;
		if ((pb->slot_busy[b] & (1ULL << (i % 64))) == 0) {
			pb->slot_busy[b] |= 1ULL << (i % 64);
			mtx_unlock(&pb->lock);
			return ((int)i);
		}
	}
	mtx_unlock(&pb->lock);
	return (-1);
}

/* ---- pushes (04 section 2.2) ---- */

/*
 * uvm_channel_reserve - take one GPFIFO entry on a channel if one is
 * free: available = N - 1 (the sentinel) - pending - reserved.
 */
static bool
uvm_channel_reserve(struct uvm_channel *ch)
{
	bool ok;

	mtx_lock(&ch->lock);
	ok = ch->initialised && ch->pending + ch->reserved + 1 < ch->num_gpfifo;
	if (ok)
		ch->reserved++;
	mtx_unlock(&ch->lock);
	return (ok);
}

static NV_STATUS
uvm_push_begin_on(struct uvm_channel *ch, struct uvm_push *push, const struct uvm_tracker *acquire)
{
	struct uvm_channel_manager *mgr = ch->mgr;
	sbintime_t deadline = sbinuptime() + SBT_1MS * (sbintime_t)uvm.wait_timeout_ms;
	int slot;
	unsigned i;

	/* a GPFIFO entry, waiting for completions when the ring is full */
	while (!uvm_channel_reserve(ch)) {
		uvm_channel_update_progress(ch);
		if (uvm_channel_check_errors(mgr->gpu) != NV_OK)
			return (uvm_global_status());
		if (sbinuptime() > deadline)
			return (NV_ERR_TIMEOUT);
		pause_sbt("uvmgpf", SBT_1US * 50, 0, C_HARDCLOCK);
	}
	/* a pushbuffer slot */
	while ((slot = uvm_pushbuffer_alloc_slot(&mgr->pb)) < 0) {
		for (i = 0; i < mgr->nchannels; i++)
			uvm_channel_update_progress(&mgr->channels[i]);
		if (uvm_channel_check_errors(mgr->gpu) != NV_OK || sbinuptime() > deadline) {
			mtx_lock(&ch->lock);
			ch->reserved--;
			mtx_unlock(&ch->lock);
			return (uvm_global_status() != NV_OK ? uvm_global_status() : NV_ERR_TIMEOUT);
		}
		pause_sbt("uvmpbs", SBT_1US * 50, 0, C_HARDCLOCK);
	}
	push->channel = ch;
	push->slot = slot;
	push->begin = (uint32_t *)(mgr->pb.cpu + (size_t)slot * UVM_PUSH_MAX_SIZE);
	push->next = push->begin;
	/* room for the end-of-push semaphore release */
	push->end = push->begin + UVM_PUSH_MAX_WORDS - UVM_CE_SEM_MAX_WORDS - 2;
	push->gpu_va = mgr->pb.gpu_va + (uint64_t)slot * UVM_PUSH_MAX_SIZE;
	push->membar = UVM_MEMBAR_SYS;
	push->value = 0;

	/* acquire the tracker: wait on other channels' values still pending (same channel is in order) */
	if (acquire != NULL) {
		for (i = 0; i < acquire->count; i++) {
			struct uvm_channel *other = acquire->entries[i].channel;
			uint64_t v = acquire->entries[i].value;

			if (other == ch || uvm_channel_value_complete(other, v))
				continue;
			UVM_PUSH(push, uvm_host_semaphore_acquire, other->sema_gpu_va, (uint32_t)v);
		}
	}
	return (NV_OK);
}

NV_STATUS
uvm_push_begin(struct uvm_gpu *gpu, struct uvm_push *push, const struct uvm_tracker *acquire)
{
	struct uvm_channel_manager *mgr = gpu->chmgr;
	struct uvm_channel *ch;

	if (mgr == NULL || mgr->nchannels == 0)
		return (NV_ERR_INVALID_STATE);
	if (uvm_global_status() != NV_OK)
		return (uvm_global_status());
	ch = &mgr->channels[mgr->next_channel % mgr->nchannels];
	mgr->next_channel++;
	return (uvm_push_begin_on(ch, push, acquire));
}

/*
 * uvm_push_inline_data - place data inside the push behind a NOP header
 * (the GPU skips it) and return its GPU VA, for a copy from the pushbuffer.
 * Data is padded to 4 bytes; at most UVM_PUSH_INLINE_MAX_WORDS words.
 */
void
uvm_push_inline_data(struct uvm_push *push, const void *data, size_t bytes, uint64_t *gpu_va_out)
{
	size_t words = (bytes + 3) / 4;
	uint32_t *w;

	KASSERT(words <= UVM_PUSH_INLINE_MAX_WORDS, ("inline data too large"));
	w = uvm_push_space(push, 1 + words);
	uvm_host_nop_header(w, (uint32_t)words);
	memcpy(w + 1, data, bytes);
	if (bytes & 3)
		memset((uint8_t *)(w + 1) + bytes, 0, 4 - (bytes & 3));
	*gpu_va_out = push->gpu_va + (uint64_t)((uint8_t *)(w + 1) - (uint8_t *)push->begin);
}

/*
 * uvm_push_end_common - append the tracking semaphore release, write the
 * GPFIFO entry, PUT and the doorbell (04 section 1.2 and 2.2).  The release
 * is a copy-engine release (the spec's end-of-push); the probe variant uses
 * a host-class release so a channel can be exercised without any copy-engine
 * method (hw.nvidia_uvm.init_probe).
 */
static void
uvm_push_end_common(struct uvm_push *push, bool host_sem)
{
	struct uvm_channel *ch = push->channel;
	struct uvm_gpfifo_entry_state *e;
	uint64_t value, entry;
	uint32_t *w, words;
	size_t n;
	bool dbg = uvm_debug_push_enabled();

	mtx_lock(&ch->lock);
	value = ++ch->queued_value;
	push->end = push->begin + UVM_PUSH_MAX_WORDS;
	w = uvm_push_space(push, UVM_CE_SEM_MAX_WORDS);
	if (host_sem)
		n = uvm_host_semaphore_release(w, ch->sema_gpu_va, (uint32_t)value);
	else
		n = uvm_ce_semaphore_release(w, ch->sema_gpu_va, (uint32_t)value, UVM_MEMBAR_SYS);
	push->next = w + n;
	words = (uint32_t)(push->next - push->begin);

	e = &ch->entries[ch->cpu_put];
	e->tracking_value = value;
	e->pb_slot = push->slot;
	entry = uvm_gpfifo_entry(push->gpu_va, words);
	ch->info.gpFifoEntries[ch->cpu_put] = entry;
	if (dbg) {
		uvm_debug_log_stream(ch, push, words);
		uvm_debug_log_gpfifo(ch, ch->cpu_put, entry);
	}
	wmb();
	ch->cpu_put = (ch->cpu_put + 1) % ch->num_gpfifo;
	*ch->info.gpPut = ch->cpu_put;
	ch->pending++;
	ch->reserved--;
	/* posted writes to the GPU must have landed before the doorbell when PUT is in system memory */
	if (ch->info.dummyBar1Mapping != NULL)
		(void)*ch->info.dummyBar1Mapping;
	uvm_doorbell_barrier();
	if (dbg) {
		uvm_log("debug_push: ch%u GP_PUT <- %u (%s release value %ju), doorbell %p <- %#x", ch->index,
		    ch->cpu_put, host_sem ? "host" : "copy-engine", (uintmax_t)value,
		    (void *)ch->info.workSubmissionOffset, ch->info.workSubmissionToken);
		uvm_debug_pushes_logged++;
	}
	*ch->info.workSubmissionOffset = ch->info.workSubmissionToken;
	uvm_doorbell_barrier();
	mtx_unlock(&ch->lock);
	push->value = value;
}

void
uvm_push_end(struct uvm_push *push)
{
	uvm_push_end_common(push, false);
}

void
uvm_push_end_host_sem(struct uvm_push *push)
{
	uvm_push_end_common(push, true);
}

NV_STATUS
uvm_push_wait(struct uvm_push *push)
{
	return (uvm_channel_wait_value(push->channel, push->value));
}

NV_STATUS
uvm_push_end_and_wait(struct uvm_push *push)
{
	uvm_push_end(push);
	return (uvm_push_wait(push));
}

/* ---- channel manager (04 section 2) ---- */

/*
 * uvm_channel_probe_stage - one stage of the staged initialisation: the
 * methods `fn` adds (may be NULL for none), a host-class semaphore release,
 * a wait.  The log line names the stage so a later Xid 71 in the system log
 * can be matched to the last stage that did not complete.
 */
static NV_STATUS
uvm_channel_probe_stage(struct uvm_channel *ch, const char *name, uint32_t subch, uint32_t class_num)
{
	struct uvm_push push;
	NV_STATUS status;

	status = uvm_push_begin_on(ch, &push, NULL);
	if (status != NV_OK)
		return (status);
	if (class_num != 0)
		UVM_PUSH(&push, uvm_host_set_object, subch, class_num);
	uvm_log("ch%u init probe: stage '%s' sent (host-class semaphore release follows it)", ch->index, name);
	uvm_push_end_host_sem(&push);
	status = uvm_push_wait(&push);
	if (status == NV_OK)
		uvm_log("ch%u init probe: stage '%s' completed", ch->index, name);
	else
		uvm_log("ch%u init probe: stage '%s' FAILED: %s", ch->index, name, uvm_status_name(status));
	return (status);
}

/*
 * uvm_channel_init_probe - hw.nvidia_uvm.init_probe=1: the same work as
 * uvm_channel_init_push, split so the first stage that makes the GPU raise
 * an error is known: (0) nothing but a host semaphore release, which proves
 * the GPFIFO, PUT, doorbell, pushbuffer VA and the tracking semaphore VA
 * without any class bound; (1) SET_OBJECT of the copy class on subchannel 0;
 * (2) SET_OBJECT of the software class on subchannel 5; (3) when
 * ce_bind_subch4 is set, SET_OBJECT of the copy class on subchannel 4;
 * (4) the end-of-push copy-engine semaphore release, the first copy-engine
 * method the module ever sends.
 */
static NV_STATUS
uvm_channel_init_probe(struct uvm_channel *ch)
{
	struct uvm_push push;
	NV_STATUS status;
	uint32_t ce = ch->mgr->gpu->info.ceClass;

	status = uvm_channel_probe_stage(ch, "0 host semaphore only", 0, 0);
	if (status == NV_OK)
		status = uvm_channel_probe_stage(ch, "1 SET_OBJECT copy class subch 0", UVM_SUBCH_HOST, ce);
	if (status == NV_OK)
		status = uvm_channel_probe_stage(ch, "2 SET_OBJECT software class subch 5", UVM_SUBCH_SW,
		    UVM_SW_CLASS);
	if (status == NV_OK && uvm.ce_bind_subch4)
		status = uvm_channel_probe_stage(ch, "3 SET_OBJECT copy class subch 4", UVM_SUBCH_CE, ce);
	if (status != NV_OK)
		return (status);
	status = uvm_push_begin_on(ch, &push, NULL);
	if (status != NV_OK)
		return (status);
	uvm_log("ch%u init probe: stage '4 copy-engine semaphore release' sent", ch->index);
	status = uvm_push_end_and_wait(&push);
	if (status == NV_OK)
		uvm_log("ch%u init probe: stage '4 copy-engine semaphore release' completed", ch->index);
	else
		uvm_log("ch%u init probe: stage '4 copy-engine semaphore release' FAILED: %s", ch->index,
		    uvm_status_name(status));
	return (status);
}

/*
 * uvm_channel_init_push - the channel's first work: the pushbuffer segment
 * base control entry (Hopper and Blackwell), then SET_OBJECT of the copy
 * class on subchannel 0 and of the software class on subchannel 5 (04
 * section 1.1).  With hw.nvidia_uvm.ce_bind_subch4 the copy class is also
 * bound on subchannel 4, where every copy-engine method is issued (Q-008).
 */
static NV_STATUS
uvm_channel_init_push(struct uvm_channel *ch)
{
	struct uvm_push push;
	NV_STATUS status;

	if (uvm.debug_push > 0)
		uvm_debug_log_channel(ch);
	if (ch->mgr->gpu->gen->needs_pb_segment_base) {
		uint64_t entry = uvm_gpfifo_segment_base_entry(ch->mgr->pb.gpu_va);

		mtx_lock(&ch->lock);
		ch->entries[ch->cpu_put].tracking_value = 0;
		ch->entries[ch->cpu_put].pb_slot = -1;
		ch->info.gpFifoEntries[ch->cpu_put] = entry;
		if (uvm_debug_push_enabled())
			uvm_debug_log_gpfifo(ch, ch->cpu_put, entry);
		ch->cpu_put = (ch->cpu_put + 1) % ch->num_gpfifo;
		ch->pending++;	/* retired with the first real push's value */
		mtx_unlock(&ch->lock);
	}
	ch->initialised = true;
	if (uvm.init_probe)
		return (uvm_channel_init_probe(ch));
	status = uvm_push_begin_on(ch, &push, NULL);
	if (status != NV_OK)
		return (status);
	UVM_PUSH(&push, uvm_host_set_object, UVM_SUBCH_HOST, ch->mgr->gpu->info.ceClass);
	UVM_PUSH(&push, uvm_host_set_object, UVM_SUBCH_SW, UVM_SW_CLASS);
	if (uvm.ce_bind_subch4)
		UVM_PUSH(&push, uvm_host_set_object, UVM_SUBCH_CE, ch->mgr->gpu->info.ceClass);
	return (uvm_push_end_and_wait(&push));
}

static void
uvm_channel_destroy(struct uvm_channel *ch)
{
	if (ch->rm_channel != NULL) {
		if (ch->initialised && uvm_global_status() == NV_OK)
			(void)uvm_channel_wait_value(ch, ch->queued_value);
		nvUvmInterfaceChannelDestroy(ch->rm_channel);
		ch->rm_channel = NULL;
	}
	if (ch->entries != NULL) {
		free(ch->entries, M_NVIDIA_UVM);
		ch->entries = NULL;
	}
	if (mtx_initialized(&ch->lock))
		mtx_destroy(&ch->lock);
	ch->initialised = false;
}

static NV_STATUS
uvm_channel_create(struct uvm_channel_manager *mgr, unsigned index)
{
	struct uvm_channel *ch = &mgr->channels[index];
	UvmGpuChannelAllocParams params;
	NV_STATUS status;
	uint32_t i;

	memset(ch, 0, sizeof(*ch));
	ch->mgr = mgr;
	ch->index = index;
	mtx_init(&ch->lock, "nvidia_uvm_channel", NULL, MTX_DEF);
	ch->num_gpfifo = mgr->gpfifo_entries;
	ch->entries = malloc(sizeof(*ch->entries) * ch->num_gpfifo, M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	for (i = 0; i < ch->num_gpfifo; i++)
		ch->entries[i].pb_slot = -1;
	/* one tracking semaphore word per channel in the manager's page */
	ch->sema_cpu = mgr->sema_cpu + index;
	ch->sema_gpu_va = mgr->sema_gpu_va + index * sizeof(uint32_t);
	*ch->sema_cpu = 0;

	params.numGpFifoEntries = ch->num_gpfifo;
	params.gpFifoLoc = uvm.sysmem_gpfifo ? UVM_BUFFER_LOCATION_SYS : UVM_BUFFER_LOCATION_VID;
	params.gpPutLoc = params.gpFifoLoc;
	status = nvUvmInterfaceChannelAllocate(mgr->rm_tsg, &params, &ch->rm_channel, &ch->info);
	if (status != NV_OK) {
		uvm_log("gpu%u: RM channel allocation failed: %s", mgr->gpu->id, uvm_status_name(status));
		uvm_channel_destroy(ch);
		return (status);
	}
	if (ch->info.numGpFifoEntries != ch->num_gpfifo) {
		uvm_log("gpu%u: RM gave %u GPFIFO entries, asked %u", mgr->gpu->id, ch->info.numGpFifoEntries,
		    ch->num_gpfifo);
		uvm_channel_destroy(ch);
		return (NV_ERR_INVALID_STATE);
	}
	status = uvm_channel_init_push(ch);
	if (status != NV_OK) {
		uvm_log("gpu%u: channel %u initialisation push failed: %s", mgr->gpu->id, index,
		    uvm_status_name(status));
		uvm_channel_destroy(ch);
		return (status);
	}
	return (NV_OK);
}

/*
 * uvm_channel_manager_pick_ce - the first copy engine that is supported,
 * not synchronous with graphics, and can reach system memory.
 */
static int
uvm_channel_manager_pick_ce(const struct uvm_gpu *gpu)
{
	unsigned i;

	for (i = 0; i < UVM_COPY_ENGINE_COUNT_MAX; i++) {
		const UvmGpuCopyEngineCaps *c = &gpu->ce_caps.copyEngineCaps[i];

		if (UVM_CE_CAP_HAS(c, SUPPORTED) && !UVM_CE_CAP_HAS(c, GRCE) && UVM_CE_CAP_HAS(c, SYSMEM))
			return ((int)i);
	}
	return (-1);
}

int
uvm_channel_manager_create(struct uvm_gpu *gpu)
{
	struct uvm_channel_manager *mgr;
	UvmGpuAllocInfo alloc;
	UvmGpuTsgAllocParams tsg;
	NV_STATUS status;
	void *cpu;
	int ce;
	unsigned i;

	ce = uvm_channel_manager_pick_ce(gpu);
	if (ce < 0) {
		uvm_log("gpu%u: no usable copy engine (supported, not GRCE, sysmem-capable)", gpu->id);
		return (ENXIO);
	}
	mgr = malloc(sizeof(*mgr), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	mgr->gpu = gpu;
	mgr->ce_index = (uint32_t)ce;
	mgr->gpfifo_entries = (uint32_t)uvm.gpfifo_entries;
	mtx_init(&mgr->pb.lock, "nvidia_uvm_pushbuffer", NULL, MTX_DEF);
	gpu->chmgr = mgr;

	/* the pushbuffer: 16 MB of RM system memory aligned to its size, CPU mapped */
	memset(&alloc, 0, sizeof(alloc));
	alloc.pageSize = UVM_PAGE_SIZE_4K;
	alloc.alignment = UVM_PUSHBUFFER_SIZE;
	status = nvUvmInterfaceMemoryAllocSys(gpu->rm_as, UVM_PUSHBUFFER_SIZE, &mgr->pb.gpu_va, &alloc);
	if (status != NV_OK) {
		uvm_log("gpu%u: pushbuffer allocation failed: %s", gpu->id, uvm_status_name(status));
		goto fail;
	}
	status = nvUvmInterfaceMemoryCpuMap(gpu->rm_as, mgr->pb.gpu_va, UVM_PUSHBUFFER_SIZE, &cpu, UVM_PAGE_SIZE_4K);
	if (status != NV_OK) {
		uvm_log("gpu%u: pushbuffer CPU mapping failed: %s", gpu->id, uvm_status_name(status));
		goto fail;
	}
	mgr->pb.cpu = cpu;

	/* the semaphore page */
	memset(&alloc, 0, sizeof(alloc));
	alloc.pageSize = UVM_PAGE_SIZE_4K;
	status = nvUvmInterfaceMemoryAllocSys(gpu->rm_as, PAGE_SIZE, &mgr->sema_gpu_va, &alloc);
	if (status != NV_OK) {
		uvm_log("gpu%u: semaphore page allocation failed: %s", gpu->id, uvm_status_name(status));
		goto fail;
	}
	status = nvUvmInterfaceMemoryCpuMap(gpu->rm_as, mgr->sema_gpu_va, PAGE_SIZE, &cpu, UVM_PAGE_SIZE_4K);
	if (status != NV_OK) {
		uvm_log("gpu%u: semaphore page CPU mapping failed: %s", gpu->id, uvm_status_name(status));
		goto fail;
	}
	mgr->sema_cpu = cpu;
	memset(cpu, 0, PAGE_SIZE);

	/* the TSG on the chosen engine, then the channels */
	tsg.engineType = UVM_GPU_CHANNEL_ENGINE_TYPE_CE;
	tsg.engineIndex = mgr->ce_index;
	status = nvUvmInterfaceTsgAllocate(gpu->rm_as, &tsg, &mgr->rm_tsg);
	if (status != NV_OK) {
		uvm_log("gpu%u: TSG allocation on copy engine %u failed: %s", gpu->id, mgr->ce_index,
		    uvm_status_name(status));
		goto fail;
	}
	for (i = 0; i < UVM_CHANNELS_PER_POOL; i++) {
		status = uvm_channel_create(mgr, i);
		if (status != NV_OK)
			goto fail;
		mgr->nchannels++;
	}
	uvm_log("gpu%u: %u channels on copy engine %u, %u GPFIFO entries each, GPFIFO in %s memory",
	    gpu->id, mgr->nchannels, mgr->ce_index, mgr->gpfifo_entries, uvm.sysmem_gpfifo ? "system" : "video");
	return (0);

fail:
	uvm_channel_manager_destroy(gpu);
	return (ENXIO);
}

void
uvm_channel_manager_destroy(struct uvm_gpu *gpu)
{
	struct uvm_channel_manager *mgr = gpu->chmgr;
	unsigned i;

	if (mgr == NULL)
		return;
	for (i = 0; i < mgr->nchannels; i++)
		uvm_channel_destroy(&mgr->channels[i]);
	mgr->nchannels = 0;
	if (mgr->rm_tsg != NULL)
		nvUvmInterfaceTsgDestroy(mgr->rm_tsg);
	if (mgr->sema_cpu != NULL)
		nvUvmInterfaceMemoryCpuUnMap(gpu->rm_as, (void *)(uintptr_t)mgr->sema_cpu);
	if (mgr->sema_gpu_va != 0)
		nvUvmInterfaceMemoryFree(gpu->rm_as, mgr->sema_gpu_va);
	if (mgr->pb.cpu != NULL)
		nvUvmInterfaceMemoryCpuUnMap(gpu->rm_as, mgr->pb.cpu);
	if (mgr->pb.gpu_va != 0)
		nvUvmInterfaceMemoryFree(gpu->rm_as, mgr->pb.gpu_va);
	mtx_destroy(&mgr->pb.lock);
	free(mgr, M_NVIDIA_UVM);
	gpu->chmgr = NULL;
}
