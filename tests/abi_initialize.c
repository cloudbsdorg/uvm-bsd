/* Copyright (c) 2026 REVYTECH, Inc. */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * amd64 layout of the first UVM ioctl, the one libcuda sends.
 * UVM_INITIALIZE is 0x30000001. The struct is 16 bytes, aligned 8,
 * with flags at 0 and rmStatus at 8. Those numbers come from the
 * pinned 595.99.02 headers. A mismatch fails this file at compile time.
 */
#include "uvm_linux_ioctl.h"

#include <stddef.h>

_Static_assert(sizeof(void *) == 8, "UVM_INITIALIZE layout is for a 64-bit ABI");
_Static_assert(UVM_INITIALIZE == 0x30000001u, "UVM_INITIALIZE is not 0x30000001");
_Static_assert(sizeof(UVM_INITIALIZE_PARAMS) == 16, "UVM_INITIALIZE_PARAMS is not 16 bytes");
_Static_assert(_Alignof(UVM_INITIALIZE_PARAMS) == 8, "UVM_INITIALIZE_PARAMS alignment is not 8");
_Static_assert(offsetof(UVM_INITIALIZE_PARAMS, flags) == 0, "flags is not at offset 0");
_Static_assert(offsetof(UVM_INITIALIZE_PARAMS, rmStatus) == 8, "rmStatus is not at offset 8");
_Static_assert(sizeof(((UVM_INITIALIZE_PARAMS *)0)->flags) == 8, "flags is not 8 bytes");
_Static_assert(sizeof(((UVM_INITIALIZE_PARAMS *)0)->rmStatus) == 4, "rmStatus is not 4 bytes");

int
main(void)
{
	return 0;
}
