/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * client_owner_test.c - the RM client ownership rule (sys/uvm_client_owner.h,
 * Track #2373).  A small world has two users, each with an nvidiactl file
 * and a client allocated on it, a file that is not an nvidiactl file and a
 * closed fd.  Every request that carries an rmCtrlFd is run against every
 * (fd, handle) pair; the oracle is written without the rule: only the pair
 * (the file a client was allocated on, that client) may pass.
 */

#include <stdio.h>
#include <errno.h>

#include "uvm_client_owner.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

#define	CLIENT_A	0xC1D00000u	/* the predictable base the RM hands out */
#define	CLIENT_B	0xC1D00001u
#define	CLIENT_FREE	0xC1D00002u	/* allocated by nobody */

enum file_id { F_A, F_B, F_NOT_NVCTL, F_CLOSED, F_COUNT };

/* nvidia.ko's side, as a model: the answer for a resolved file */
static NV_STATUS
nv_owner_status(enum file_id f, NvHandle h)
{
	if (f == F_A && h == CLIENT_A)
		return (NV_OK);
	if (f == F_B && h == CLIENT_B)
		return (NV_OK);
	return (NV_ERR_INSUFFICIENT_PERMISSIONS);
}

/* the kernel's side: resolve the fd, ask nvidia.ko, apply the verdict */
static NV_STATUS
run_request(enum uvm_owner_request req, enum file_id f, NvHandle h, NvHandle smc)
{
	int fget_error;

	if (!uvm_owner_needed(req, h, smc))
		return (NV_OK);
	fget_error = f == F_CLOSED ? EBADF : 0;
	return (uvm_owner_verdict(fget_error, fget_error != 0 ? NV_OK : nv_owner_status(f, h)));
}

static bool
oracle_allows(enum file_id f, NvHandle h)
{
	return ((f == F_A && h == CLIENT_A) || (f == F_B && h == CLIENT_B));
}

int
main(void)
{
	static const NvHandle handles[] = { CLIENT_A, CLIENT_B, CLIENT_FREE, 0 };
	unsigned r, f, i;

	/* every request, every file, every handle (the SMC reference nonzero so that register GPU is in play) */
	for (r = 0; r < UVM_OWNER_NREQUESTS; r++) {
		for (f = 0; f < F_COUNT; f++) {
			for (i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
				NV_STATUS got = run_request(r, f, handles[i], 1);
				NV_STATUS want = oracle_allows(f, handles[i]) ? NV_OK :
				    NV_ERR_INSUFFICIENT_PERMISSIONS;

				CHECK(got == want, "request %u file %u handle 0x%x: got 0x%x want 0x%x",
				    r, f, handles[i], (unsigned)got, (unsigned)want);
			}
		}
	}

	/* the attack itself: B's fd with A's handle, on each request */
	for (r = 0; r < UVM_OWNER_NREQUESTS; r++) {
		CHECK(run_request(r, F_B, CLIENT_A, 0) == NV_ERR_INSUFFICIENT_PERMISSIONS,
		    "request %u: B with A's handle must be refused", r);
		CHECK(run_request(r, F_NOT_NVCTL, CLIENT_A, 0) == NV_ERR_INSUFFICIENT_PERMISSIONS,
		    "request %u: a non-nvidiactl fd must be refused", r);
		CHECK(run_request(r, F_CLOSED, CLIENT_A, 0) == NV_ERR_INSUFFICIENT_PERMISSIONS,
		    "request %u: a closed fd must be refused", r);
		CHECK(run_request(r, F_A, CLIENT_A, 0) == NV_OK, "request %u: the owner must pass", r);
	}

	/* register GPU names a client only when it passes one or an SMC reference */
	CHECK(!uvm_owner_needed(UVM_OWNER_REGISTER_GPU, 0, 0), "register GPU with no client needs no check");
	CHECK(uvm_owner_needed(UVM_OWNER_REGISTER_GPU, CLIENT_A, 0), "register GPU with a client is checked");
	CHECK(uvm_owner_needed(UVM_OWNER_REGISTER_GPU, 0, 7), "register GPU with an SMC reference is checked");
	CHECK(run_request(UVM_OWNER_REGISTER_GPU, F_A, 0, 7) == NV_ERR_INSUFFICIENT_PERMISSIONS,
	    "register GPU with an SMC reference and no client is refused: handle 0 is nobody's");
	for (r = UVM_OWNER_REGISTER_GPU_VASPACE; r < UVM_OWNER_NREQUESTS; r++)
		CHECK(uvm_owner_needed(r, 0, 0), "request %u is checked even with handle 0", r);

	/* the verdict alone: only a resolved fd and an owned handle give NV_OK */
	CHECK(uvm_owner_verdict(0, NV_OK) == NV_OK, "resolved and owned");
	CHECK(uvm_owner_verdict(0, NV_ERR_INSUFFICIENT_PERMISSIONS) == NV_ERR_INSUFFICIENT_PERMISSIONS, "not owned");
	CHECK(uvm_owner_verdict(0, NV_ERR_INVALID_ARGUMENT) == NV_ERR_INSUFFICIENT_PERMISSIONS,
	    "any other answer from nvidia.ko is a refusal");
	CHECK(uvm_owner_verdict(EBADF, NV_OK) == NV_ERR_INSUFFICIENT_PERMISSIONS, "an unresolved fd, whatever the owner says");
	CHECK(uvm_owner_verdict(ENOTCAPABLE, NV_OK) == NV_ERR_INSUFFICIENT_PERMISSIONS, "missing rights");

	if (fails != 0)
		return (1);
	printf("client_owner_test: ok\n");
	return (0);
}
