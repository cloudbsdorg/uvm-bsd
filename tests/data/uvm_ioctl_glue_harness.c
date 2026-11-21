/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_ioctl_glue_harness.c - behaviour test of the REAL glue that turns the
 * ownership verdict into a refusal (Track #2373): uvm_client_hold,
 * uvm_client_release and the five request handlers of sys/uvm_ioctl.c, extracted
 * comment-stripped by tests/client_owner_glue_test.sh into uvm_glue.inc and
 * compiled here against fakes of fget/fdrop/cap_rights, of nvUvmInterfaceClient
 * Hold/Release and of the VA-space operations that reach RM.  Every request is
 * run with an owner, a bad fd, a refusal and an odd answer from nvidia.ko; the
 * fakes record what was passed on, what was balanced and what ran when.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "nvidia_uvm_ioctl.h"
#include "uvm_client_owner.h"

struct thread { int x; };
struct file { int id; };
struct uvm_fd { int x; };
struct uvm_va_space { int x; };
struct fake_rights { unsigned m; };
#define	cap_rights_t	struct fake_rights
#define	CAP_READ	1u
#define	CAP_IOCTL	2u
#define	uvm_log_slow(...)	((void)0)
#define	UVM_NOT_YET(what, phase)	((void)0)

static int fget_calls, fget_error, last_fd, open_refs, hold_calls, release_calls, vas_calls, order_violation;
static unsigned last_rights;
static NvHandle last_hold_handle, last_rel_handle;
static struct file the_file, *last_hold_fp, *last_rel_fp;
static NV_STATUS hold_answer;
static int needs_hold;			/* the request under test must hold before the VA-space work */

static cap_rights_t *cap_rights_init_one(cap_rights_t *r, unsigned right) { r->m = right; return (r); }
struct filecaps { void *fc_ioctls; };
static int caps_limited;
static void filecaps_free(struct filecaps *c) { (void)c; }
static int
fget_cap(struct thread *td, int fd, cap_rights_t *rights, unsigned char *flags, struct file **fpp, struct filecaps *caps)
{
	(void)td; (void)flags;
	caps->fc_ioctls = caps_limited ? (void *)&caps_limited : NULL;
	fget_calls++;
	last_fd = fd;
	last_rights = rights->m;
	if (fget_error != 0)
		return (fget_error);
	*fpp = &the_file;
	open_refs++;
	return (0);
}
static void fdrop(struct file *fp, struct thread *td) { (void)fp; (void)td; open_refs--; }
static NV_STATUS
nvUvmInterfaceClientHold(struct file *fp, NvHandle h)
{
	hold_calls++;
	last_hold_fp = fp;
	last_hold_handle = h;
	if (hold_answer == NV_OK)
		open_refs++;		/* the hold's own file reference */
	return (hold_answer);
}
static void
nvUvmInterfaceClientRelease(struct file *fp, NvHandle h)
{
	release_calls++;
	last_rel_fp = fp;
	last_rel_handle = h;
	open_refs--;
}

#define	VAS_SENTINEL	0x42
static NV_STATUS
vas_op(void)
{
	vas_calls++;
	if (needs_hold && hold_calls == 0)
		order_violation++;	/* the VA-space work reached RM before the hold */
	if (needs_hold && release_calls != 0)
		order_violation++;	/* ... or after the release */
	return (VAS_SENTINEL);
}
static NV_STATUS uvm_vas_register_gpu(struct uvm_va_space *v, UVM_REGISTER_GPU_PARAMS *p) { (void)v; (void)p; return (vas_op()); }
static NV_STATUS uvm_vas_register_gpu_va_space(struct uvm_va_space *v, UVM_REGISTER_GPU_VASPACE_PARAMS *p) { (void)v; (void)p; return (vas_op()); }
static NV_STATUS uvm_vas_register_channel(struct uvm_va_space *v, UVM_REGISTER_CHANNEL_PARAMS *p) { (void)v; (void)p; return (vas_op()); }
static NV_STATUS uvm_vas_map_external_allocation(struct uvm_va_space *v, UVM_MAP_EXTERNAL_ALLOCATION_PARAMS *p) { (void)v; (void)p; return (vas_op()); }

#include "uvm_glue.inc"

static int fails;
#define	CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define	FD	7
#define	HC	0xC1D00000u
typedef NV_STATUS handler_t(struct uvm_fd *, struct uvm_va_space *, void *, struct thread *);

static void
reset(void)
{
	fget_calls = fget_error = last_fd = open_refs = hold_calls = release_calls = vas_calls = order_violation = 0;
	last_rights = 0;
	caps_limited = 0;
	last_hold_handle = last_rel_handle = 0;
	last_hold_fp = last_rel_fp = NULL;
	hold_answer = NV_OK;
}

static void
run_one(const char *name, handler_t *h, void *params, bool is_p2p)
{
	struct thread td;
	NV_STATUS want_ok = is_p2p ? NV_ERR_NOT_SUPPORTED : VAS_SENTINEL, st;

	needs_hold = 1;
	/* the owner passes: its fd and handle reach the hold, the work runs between hold and release, all balanced */
	reset();
	st = h(NULL, NULL, params, &td);
	CHECK(st == want_ok, "%s: the owner's request returns the work's status (0x%x)", name, (unsigned)st);
	CHECK(fget_calls == 1 && last_fd == FD, "%s: the request's own rmCtrlFd (%d) is resolved", name, last_fd);
	CHECK(last_rights == CAP_IOCTL, "%s: the fd is resolved with CAP_IOCTL alone (0x%x)", name, last_rights);
	CHECK(hold_calls == 1 && last_hold_handle == HC && last_hold_fp == &the_file, "%s: the request's own hClient reaches the hold (0x%x)", name, (unsigned)last_hold_handle);
	CHECK(release_calls == 1 && last_rel_handle == HC && last_rel_fp == &the_file, "%s: released once, same file and handle", name);
	CHECK(open_refs == 0, "%s: file references balance (%d)", name, open_refs);
	CHECK(is_p2p || vas_calls == 1, "%s: the VA-space work ran once", name);
	CHECK(order_violation == 0, "%s: the work ran after the hold and before the release", name);
	/* the fd does not resolve */
	reset();
	fget_error = EBADF;
	st = h(NULL, NULL, params, &td);
	CHECK(st == NV_ERR_INSUFFICIENT_PERMISSIONS, "%s: a bad fd is refused (0x%x)", name, (unsigned)st);
	CHECK(hold_calls == 0 && release_calls == 0 && vas_calls == 0 && open_refs == 0, "%s: a bad fd reaches no hold, release or work", name);
	/* a descriptor with a limited ioctl list (cap_ioctls_limit) is refused: the allowlist does not bind this path */
	reset();
	caps_limited = 1;
	st = h(NULL, NULL, params, &td);
	CHECK(st == NV_ERR_INSUFFICIENT_PERMISSIONS && hold_calls == 0 && vas_calls == 0 && open_refs == 0,
	    "%s: a descriptor with a limited ioctl list is refused, our reference dropped (0x%x)", name, (unsigned)st);
	/* the file is not an owner's */
	reset();
	hold_answer = NV_ERR_INSUFFICIENT_PERMISSIONS;
	st = h(NULL, NULL, params, &td);
	CHECK(st == NV_ERR_INSUFFICIENT_PERMISSIONS, "%s: a refused hold is returned (0x%x)", name, (unsigned)st);
	CHECK(vas_calls == 0 && release_calls == 0 && open_refs == 0, "%s: a refusal does no work, no release, drops our reference (%d)", name, open_refs);
	/* an answer other than NV_OK, of any kind, is a refusal */
	reset();
	hold_answer = NV_ERR_INVALID_ARGUMENT;
	st = h(NULL, NULL, params, &td);
	CHECK(st == NV_ERR_INSUFFICIENT_PERMISSIONS && vas_calls == 0 && release_calls == 0 && open_refs == 0,
	    "%s: any other answer is a refusal (0x%x)", name, (unsigned)st);
}

int
main(void)
{
	UVM_REGISTER_GPU_PARAMS pg;
	UVM_REGISTER_GPU_VASPACE_PARAMS pv;
	UVM_REGISTER_CHANNEL_PARAMS pc;
	UVM_MAP_EXTERNAL_ALLOCATION_PARAMS pm;
	UVM_ALLOC_DEVICE_P2P_PARAMS pp;
	struct thread td;
	NV_STATUS st;

	memset(&pg, 0, sizeof(pg)); pg.rmCtrlFd = FD; pg.hClient = HC;
	memset(&pv, 0, sizeof(pv)); pv.rmCtrlFd = FD; pv.hClient = HC;
	memset(&pc, 0, sizeof(pc)); pc.rmCtrlFd = FD; pc.hClient = HC;
	memset(&pm, 0, sizeof(pm)); pm.rmCtrlFd = FD; pm.hClient = HC;
	memset(&pp, 0, sizeof(pp)); pp.rmCtrlFd = FD; pp.hClient = HC;
	run_one("register_gpu", uvm_h_register_gpu, &pg, false);
	run_one("register_gpu_vaspace", uvm_h_register_gpu_vaspace, &pv, false);
	run_one("register_channel", uvm_h_register_channel, &pc, false);
	run_one("map_external_allocation", uvm_h_map_external_allocation, &pm, false);
	run_one("alloc_device_p2p", uvm_h_alloc_device_p2p, &pp, true);

	/* register GPU names no client: nothing to own, the work runs without a hold */
	needs_hold = 0;
	reset();
	pg.hClient = 0; pg.hSmcPartRef = 0;
	st = uvm_h_register_gpu(NULL, NULL, &pg, &td);
	CHECK(st == VAS_SENTINEL && fget_calls == 0 && hold_calls == 0, "register_gpu without a client needs no hold");
	/* an SMC reference without its client is refused (handle 0 is nobody's) */
	reset();
	hold_answer = NV_ERR_INSUFFICIENT_PERMISSIONS;
	pg.hSmcPartRef = 5;
	st = uvm_h_register_gpu(NULL, NULL, &pg, &td);
	CHECK(st == NV_ERR_INSUFFICIENT_PERMISSIONS && hold_calls == 1 && last_hold_handle == 0 && vas_calls == 0,
	    "an SMC reference alone goes to the hold with handle 0 and is refused");
	if (fails != 0)
		return (1);
	printf("uvm_ioctl_glue_harness: ok\n");
	return (0);
}
