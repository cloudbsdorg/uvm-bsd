/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_main.c - module load and unload, the two device nodes, the
 * Linuxulator ioctl bridge, the RM session and callback table, the global
 * fatal error, tunables and statistics.  uvm-spec r1: 01-user-abi.md
 * sections 1, 2, 3; 02-rm-interface.md sections 3.1 and 5;
 * 09-freebsd-facilities.md section 3.
 *
 * Module init order (03 section 7): the RM session, then the global state,
 * then the callback table (from then on RM calls the top half, so the GPU
 * table must already answer), then the device nodes.  Exit reverses it.
 *
 * Request numbers arrive two ways.  A Linux process (libcuda) issues the
 * raw numbers of 01 section 2.1; the Linuxulator passes every request whose
 * low 16 bits fall in [1, 2047] to the handler registered here, which checks
 * that the file is one of ours and re-enters the kernel's own ioctl path
 * with the native encoding of nvidia_uvm_ioctl.h, so the per-file state
 * (devfs cdevpriv) is found the same way on both paths.  A native FreeBSD
 * caller uses that encoding directly.
 */

#include "uvm.h"

#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/sbuf.h>
#include <sys/poll.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/capsicum.h>
#include <sys/vnode.h>
#include <sys/syscallsubr.h>
#include <sys/ioccom.h>
#include <sys/filio.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/priv.h>
#include <sys/eventhandler.h>

#include <machine/../linux/linux.h>
#include <machine/../linux/linux_proto.h>
#include <compat/linux/linux_ioctl.h>

MALLOC_DEFINE(M_NVIDIA_UVM, "nvidia_uvm", "NVIDIA UVM driver");

struct uvm_global uvm = {
	.builtin_tests = 0,
	.enable_va_space_mm = 1,
	.gpfifo_entries = 1024,
	.fault_batch_count = 256,
	.wait_timeout_ms = 30000,
	.sysmem_gpfifo = 1,
};

/* ---- tunables and statistics: hw.nvidia_uvm.* (01 section 1, 09 section 1) ---- */

/* CPD-OFF -- one declaration per tunable: the repetition is the list */
SYSCTL_NODE(_hw, OID_AUTO, nvidia_uvm, CTLFLAG_RW | CTLFLAG_MPSAFE, 0, "NVIDIA UVM driver");
SYSCTL_INT(_hw_nvidia_uvm, OID_AUTO, builtin_tests, CTLFLAG_RDTUN, &uvm.builtin_tests, 0,
    "built-in tests enabled (0: unknown request numbers fail with ENOSYS)");
SYSCTL_INT(_hw_nvidia_uvm, OID_AUTO, enable_va_space_mm, CTLFLAG_RDTUN, &uvm.enable_va_space_mm, 0,
    "bind VA spaces to the creating process address space");
SYSCTL_INT(_hw_nvidia_uvm, OID_AUTO, gpfifo_entries, CTLFLAG_RDTUN, &uvm.gpfifo_entries, 0,
    "GPFIFO entries per UVM channel (power of two in [32, 1M])");
SYSCTL_INT(_hw_nvidia_uvm, OID_AUTO, fault_batch_count, CTLFLAG_RDTUN, &uvm.fault_batch_count, 0,
    "replayable faults fetched per batch");
SYSCTL_INT(_hw_nvidia_uvm, OID_AUTO, wait_timeout_ms, CTLFLAG_RWTUN, &uvm.wait_timeout_ms, 0,
    "milliseconds a wait on GPU work may take before it is a fatal error");
SYSCTL_INT(_hw_nvidia_uvm, OID_AUTO, sysmem_gpfifo, CTLFLAG_RDTUN, &uvm.sysmem_gpfifo, 0,
    "place the GPFIFO ring and PUT word in system memory (1) or video memory (0)");
SYSCTL_INT(_hw_nvidia_uvm, OID_AUTO, debug_push, CTLFLAG_RDTUN, &uvm.debug_push, 0,
    "log the method stream, GPFIFO entry, GP_PUT and doorbell of the first N pushes (0: off, max 64)");
SYSCTL_INT(_hw_nvidia_uvm, OID_AUTO, init_probe, CTLFLAG_RDTUN, &uvm.init_probe, 0,
    "staged channel initialisation (1: host-class semaphores only, then the copy engine) with a log line per stage");
SYSCTL_INT(_hw_nvidia_uvm, OID_AUTO, init_probe_work, CTLFLAG_RDTUN, &uvm.init_probe_work, 0,
    "with init_probe: bit mask of first-work probe stages on channel 0 (0: none; see nvidia-uvm(4))");
SYSCTL_INT(_hw_nvidia_uvm, OID_AUTO, ce_bind_subch4, CTLFLAG_RDTUN, &uvm.ce_bind_subch4, 0,
    "also bind the copy class on subchannel 4 during channel initialisation (experiment, spec question Q-008)");
SYSCTL_UINT(_hw_nvidia_uvm, OID_AUTO, fatal_error, CTLFLAG_RD, &uvm.fatal_error, 0,
    "the module-wide fatal status (0 = NV_OK)");

/* CPD-ON */

static int uvm_sysctl_gpus(SYSCTL_HANDLER_ARGS);
SYSCTL_PROC(_hw_nvidia_uvm, OID_AUTO, gpus, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    uvm_sysctl_gpus, "A", "registered GPUs and their fault statistics");

static int
uvm_sysctl_gpus(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	struct uvm_gpu *gpu;
	int error, i;

	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	sx_slock(&uvm.lock);
	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++) {
		gpu = uvm.gpus[i];
		if (gpu == NULL)
			continue;
		sbuf_printf(&sb, "gpu%d %s arch %#x host %#x ce %#x mmu v%u refs %u top-halves %ju "
		    "replayable: fetched %ju cancelled %ju stale %ju replays %ju flushes %ju overflows %ju "
		    "non-replayable: fetched %ju reported %ju\n",
		    gpu->id, gpu->name, gpu->info.gpuArch, gpu->info.hostClass, gpu->info.ceClass,
		    gpu->mmu_version, gpu->refs, (uintmax_t)gpu->isr_top_halves,
		    (uintmax_t)gpu->replayable.faults_fetched, (uintmax_t)gpu->replayable.faults_cancelled,
		    (uintmax_t)gpu->replayable.faults_stale, (uintmax_t)gpu->replayable.replays,
		    (uintmax_t)gpu->replayable.flushes, (uintmax_t)gpu->replayable.overflows,
		    (uintmax_t)gpu->nonreplayable.faults_fetched, (uintmax_t)gpu->nonreplayable.faults_reported);
	}
	sx_sunlock(&uvm.lock);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

/*
 * uvm_global_set_fatal - record the module-wide fatal error (01 section
 * 2.4).  The first error wins; it never clears without a module reload.
 */
void
uvm_global_set_fatal(NV_STATUS status, const char *why)
{
	if (status == NV_OK)
		return;
	if (atomic_cmpset_32(&uvm.fatal_error, NV_OK, status))
		uvm_log("FATAL: %s (%s); every later request fails with it until the module is reloaded",
		    why, uvm_status_name(status));
}

/* ---- the device nodes ---- */

static void
uvm_fd_dtor(void *arg)
{
	struct uvm_fd *fd = arg;
	struct uvm_va_space *vas;

	vas = uvm_fd_va_space(fd);
	if (vas != NULL) {
		/* VA-space fd: the whole teardown (01 section 3.5) */
		uvm_va_space_unref(vas);
	} else if ((vas = uvm_fd_mm_va_space(fd)) != NULL) {
		/* MM fd (01 section 3.5): the mm shutdown, the map's users reference, then the primary's VA space */
		uvm_va_space_mm_release(vas);
		uvm_va_space_unref(vas);
	}
	free(fd, M_NVIDIA_UVM);
	/* last: the unload waits for the count, so nothing may touch module state after it */
	KASSERT(atomic_load_int(&uvm.open_fds) > 0, ("uvm open file count underflow"));
	atomic_subtract_int(&uvm.open_fds, 1);
}

static int
uvm_open(struct cdev *cdev, int oflags, int devtype, struct thread *td)
{
	struct uvm_fd *fd;
	int error;

	if (uvm_global_status() != NV_OK)
		return (uvm_status_to_errno(uvm_global_status()));
	/* the count and the unload's decision are one step under uvm.lock (uvm_unload_state.h) */
	sx_slock(&uvm.lock);
	if (!uvm_open_admit(uvm.unloading)) {
		sx_sunlock(&uvm.lock);
		return (ENXIO);
	}
	atomic_add_int(&uvm.open_fds, 1);
	sx_sunlock(&uvm.lock);
	fd = malloc(sizeof(*fd), M_NVIDIA_UVM, M_WAITOK | M_ZERO);
	fd->cdev = cdev;
	error = devfs_set_cdevpriv(fd, uvm_fd_dtor);
	if (error != 0) {
		free(fd, M_NVIDIA_UVM);
		atomic_subtract_int(&uvm.open_fds, 1);
	}
	return (error);
}

static int
uvm_ioctl(struct cdev *cdev, u_long cmd, caddr_t data, int fflag, struct thread *td)
{
	struct uvm_fd *fd;
	uint32_t request;
	int error;

	/* fcntl F_SETFL arrives as these; a zero argument is a no-op (01 section 2) */
	if (cmd == FIONBIO || cmd == FIOASYNC)
		return (*(int *)data == 0 ? 0 : ENOTTY);
	request = uvm_ioctl_native_decode(cmd);
	if (request == 0)
		return (ENOTTY);
	error = devfs_get_cdevpriv((void **)&fd);
	if (error != 0)
		return (error);
	return (uvm_ioctl_dispatch(fd, cdev == uvm.cdev_tools, request, *(void **)data, td));
}

static int
uvm_poll(struct cdev *cdev, int events, struct thread *td)
{
	/* the tools node's queues are a later phase: a tracker in error, no readable data */
	if (cdev == uvm.cdev_tools)
		return (events & POLLERR);
	return (events & (POLLERR | POLLHUP));
}

static struct cdevsw uvm_cdevsw = {
	.d_version =	D_VERSION,
	.d_flags =	D_TRACKCLOSE,
	.d_name =	"nvidia-uvm",
	.d_open =	uvm_open,
	.d_ioctl =	uvm_ioctl,
	.d_mmap_single = uvm_mmap_single,
	.d_poll =	uvm_poll,
};

/* ---- the Linuxulator bridge (09 section 3) ---- */

static int
uvm_linux_ioctl(struct thread *td, struct linux_ioctl_args *args)
{
	struct file *fp;
	struct cdev *cdev;
	uintptr_t arg = args->arg;
	uint32_t raw = (uint32_t)args->cmd;
	unsigned long native;
	bool ours;
	int error;

	/* is this fd one of our two nodes? Anything else is not ours: let the next handler try */
	error = fget(td, args->fd, &cap_ioctl_rights, &fp);
	if (error != 0)
		return (error);
	ours = false;
	if (fp->f_type == DTYPE_VNODE && fp->f_vnode != NULL && fp->f_vnode->v_type == VCHR) {
		cdev = fp->f_vnode->v_rdev;
		ours = (cdev == uvm.cdev_main || cdev == uvm.cdev_tools);
	}
	fdrop(fp, td);
	if (!ours)
		return (ENOIOCTL);

	/* the full 32-bit request value is compared (01 section 2.1) */
	if (raw == UVM_INITIALIZE || raw == UVM_DEINITIALIZE || (raw >= 1 && raw <= 2047))
		native = UVM_IOCTL_NATIVE(raw);
	else
		return (ENOSYS);
	/*
	 * IOC_VOID with a zero size: the kernel hands the driver the pointer
	 * to the argument word, so the parameter pointer travels unchanged.
	 */
	return (kern_ioctl(td, args->fd, native, (caddr_t)&arg));
}

static struct linux_ioctl_handler uvm_linux_handler = {
	.func = uvm_linux_ioctl,
	.low = 1,
	.high = 2047,
};

/* ---- RM callbacks (02 section 5) ---- */

static NV_STATUS
uvm_cb_suspend(void)
{
	return (uvm_gpu_suspend_all());
}

static NV_STATUS
uvm_cb_resume(void)
{
	return (uvm_gpu_resume_all());
}

static NV_STATUS
uvm_cb_isr_top_half(const NvProcessorUuid *uuid)
{
	if (uuid == NULL)
		return (NV_ERR_NO_INTR_PENDING);
	return (uvm_gpu_isr_top_half(uuid));
}

static NV_STATUS
uvm_cb_p2p_not_supported(const NvProcessorUuid *uuid)
{
	(void)uuid;
	return (NV_ERR_NOT_SUPPORTED);
}

static const struct UvmOpsUvmEvents uvm_rm_callbacks = {
	.suspend = uvm_cb_suspend,
	.resume = uvm_cb_resume,
	.startDevice = NULL,
	.stopDevice = NULL,
	.isrTopHalf = uvm_cb_isr_top_half,
	.drainP2P = uvm_cb_p2p_not_supported,
	.resumeP2P = uvm_cb_p2p_not_supported,
};

/* ---- module lifetime ---- */

static int
uvm_check_tunables(void)
{
	int n = uvm.gpfifo_entries;

	if (n < 32 || n > (1 << 20) || (n & (n - 1)) != 0) {
		uvm_log("hw.nvidia_uvm.gpfifo_entries=%d is not a power of two in [32, 1048576]", n);
		return (EINVAL);
	}
	if (uvm.fault_batch_count < 1 || uvm.fault_batch_count > 4096) {
		uvm_log("hw.nvidia_uvm.fault_batch_count=%d is not in [1, 4096]", uvm.fault_batch_count);
		return (EINVAL);
	}
	return (0);
}

static int
uvm_load(void)
{
	struct make_dev_args args;
	NV_STATUS status;
	int error;

	error = uvm_check_tunables();
	if (error != 0)
		return (error);

	sx_init(&uvm.pm_lock, "nvidia_uvm_pm");
	sx_init(&uvm.lock, "nvidia_uvm_global");
	sx_init(&uvm.managed_lock, "nvidia_uvm_managed");
	mtx_init(&uvm.gpu_table_lock, "nvidia_uvm_gpus", NULL, MTX_SPIN);
	TAILQ_INIT(&uvm.va_spaces);
	uvm.fatal_error = NV_OK;

	status = nvUvmInterfaceSessionCreate(&uvm.rm_session, &uvm.platform);
	if (status != NV_OK) {
		uvm_log("RM session creation failed: %s", uvm_status_name(status));
		error = ENXIO;
		goto fail_locks;
	}
	uvm_log("RM session created (ATS %s, confidential computing %s)",
	    uvm.platform.atsSupported ? "supported" : "not supported",
	    uvm.platform.confComputingEnabled ? "enabled" : "disabled");

	/* callbacks last among the RM steps: from here RM calls the top half */
	status = nvUvmInterfaceRegisterUvmCallbacks(&uvm_rm_callbacks);
	if (status != NV_OK) {
		uvm_log("callback registration failed: %s", uvm_status_name(status));
		error = EBUSY;
		goto fail_session;
	}
	uvm.callbacks_registered = true;

	/* the core's registry and the NVIDIA backend come before the nodes */
	uvm_core_registry_init();
	error = uvm_core_backend_register(&uvm_nvidia_backend);
	if (error != 0) {
		uvm_log("the NVIDIA backend did not register: %d", error);
		goto fail_core;
	}

	make_dev_args_init(&args);
	args.mda_devsw = &uvm_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0666;
	args.mda_unit = 0;
	error = make_dev_s(&args, &uvm.cdev_main, "nvidia-uvm");
	if (error != 0)
		goto fail_callbacks;
	args.mda_unit = 1;
	error = make_dev_s(&args, &uvm.cdev_tools, "nvidia-uvm-tools");
	if (error != 0)
		goto fail_main;

	error = linux_ioctl_register_handler(&uvm_linux_handler);
	if (error != 0)
		goto fail_tools;

	uvm_log("loaded: /dev/nvidia-uvm and /dev/nvidia-uvm-tools (spec r1, phase 1)");
	return (0);

fail_tools:
	destroy_dev(uvm.cdev_tools);
fail_main:
	destroy_dev(uvm.cdev_main);
fail_core:
	uvm_core_backend_unregister(&uvm_nvidia_backend);
	uvm_core_registry_deinit();
fail_callbacks:
	nvUvmInterfaceDeRegisterUvmOps();
	uvm.callbacks_registered = false;
fail_session:
	nvUvmInterfaceSessionDestroy(uvm.rm_session);
	uvm.rm_session = NULL;
fail_locks:
	mtx_destroy(&uvm.gpu_table_lock);
	sx_destroy(&uvm.managed_lock);
	sx_destroy(&uvm.lock);
	sx_destroy(&uvm.pm_lock);
	return (error);
}

/* the module is in use: a GPU, a VA space, an open file or a pager object (uvm_unload_state.h); `lock` is held */
static bool
uvm_in_use_locked(void)
{
	unsigned gpus = 0;
	int i;

	for (i = UVM_ID_GPU_FIRST; i <= UVM_ID_GPU_LAST; i++) {
		if (uvm.gpus[i] != NULL)
			gpus++;
	}
	return (uvm_unload_busy(atomic_load_int(&uvm.open_fds), atomic_load_int(&uvm.pager_objs),
	    TAILQ_EMPTY(&uvm.va_spaces) ? 0 : 1, gpus));
}

static int
uvm_unload(void)
{
	/*
	 * The check and the mark are one step: from the mark on uvm_open
	 * refuses, so no file, VA space or pager object can appear.  Pager
	 * objects outlive their fd (the process map goes after its files), so
	 * they are counted too; a file with no VA space yet is counted as
	 * well, its destructor is module code.
	 */
	sx_xlock(&uvm.lock);
	if (uvm_in_use_locked()) {
		sx_xunlock(&uvm.lock);
		uvm_log("unload refused: %u open files, %u pager objects, a GPU or a VA space is still in use",
		    atomic_load_int(&uvm.open_fds), atomic_load_int(&uvm.pager_objs));
		return (EBUSY);
	}
	uvm.unloading = true;
	sx_xunlock(&uvm.lock);
	/*
	 * A destructor drops its count after its last access to module
	 * state; the barrier below waits out one that still holds the
	 * power-management lock, the pause one that is returning.
	 */
	sx_xlock(&uvm.pm_lock);
	sx_xunlock(&uvm.pm_lock);
	pause("uvmdrn", hz / 10);

	linux_ioctl_unregister_handler(&uvm_linux_handler);
	/* the nodes go first so no new open can race the teardown */
	destroy_dev(uvm.cdev_tools);
	destroy_dev(uvm.cdev_main);
	/* callbacks first among the RM steps, session last (02 section 3.1) */
	nvUvmInterfaceDeRegisterUvmOps();
	uvm.callbacks_registered = false;
	nvUvmInterfaceSessionDestroy(uvm.rm_session);
	uvm.rm_session = NULL;
	mtx_destroy(&uvm.gpu_table_lock);
	sx_destroy(&uvm.managed_lock);
	sx_destroy(&uvm.lock);
	sx_destroy(&uvm.pm_lock);
	uvm_log("unloaded");
	return (0);
}

static int
uvm_modevent(module_t mod, int what, void *arg)
{
	int error;

	switch (what) {
	case MOD_LOAD:
		return (uvm_load());
	case MOD_UNLOAD:
		return (uvm_unload());
	case MOD_QUIESCE:
		sx_slock(&uvm.lock);
		error = uvm_in_use_locked() ? EBUSY : 0;
		sx_sunlock(&uvm.lock);
		return (error);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t uvm_mod = { "nvidia_uvm", uvm_modevent, NULL };
DECLARE_MODULE(nvidia_uvm, uvm_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(nvidia_uvm, 1);
MODULE_DEPEND(nvidia_uvm, nvidia, 1, 1, 1);
#if defined(__aarch64__)
MODULE_DEPEND(nvidia_uvm, linux64elf, 1, 1, 1);
#else
MODULE_DEPEND(nvidia_uvm, linux, 1, 1, 1);
#endif
MODULE_DEPEND(nvidia_uvm, linux_common, 1, 1, 1);
