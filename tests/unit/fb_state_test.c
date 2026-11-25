/*
 * Copyright (c) 2026 SkunkWerks, GmbH
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * fb_state_test.c - the rules for a GPU without video memory
 * (sys/uvm_fb_state.h): the state of the frame buffer information, what a
 * missing PMA object means on each state, and where the GPFIFO ring and
 * the PUT word go.
 *
 * The descriptions used are the ones RM gives: all zero for a zero-FB GPU
 * (GB10), a heap and an allocatable range for a discrete GPU, and the same
 * heap with bZeroFb set for a GPU whose frame buffer RM calls broken.
 *
 * The cases are tables.  Every row is one of the checks that main() used to
 * make one by one, with the same message, so a failure still says what
 * failed; a row reports the line it is written on.
 *
 * The last two sections drive the same helpers through fakes, the way
 * sys/uvm_gpu.c and sys/uvm_channel.c do, so that the transitions the
 * kernel code makes are checked here row by row.
 */

#include <stdio.h>
#include <string.h>

#include "uvm_fb_state.h"

static int fails;

/*
 * ROW_CHECK - the check of one row of a table: the row carries the line it
 * is written on, so a failure is reported where the case is, not where the
 * loop runs.
 */
#define	ROW_CHECK(cond, line, ...) do {				\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, (line));		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

/* the rows of a table */
#define	ROWS(a)	(sizeof(a) / sizeof((a)[0]))

/* a row of a table: the line it is written on comes first */
#define	ROW(...)	{ __LINE__, __VA_ARGS__ }

/* the answers a fake RM gives to the two questions the driver asks */
struct fake_rm {
	NV_STATUS	fb_status;		/* what GetFbInfo answered */
	NV_STATUS	pma_status;		/* what GetPmaObject answered */
	bool		callbacks;		/* the eviction callbacks were registered */
	bool		failed;			/* the registration failed */
	const char	*message;		/* why it failed */
};

static void
fake_rm_init(struct fake_rm *rm, NV_STATUS fb_status, NV_STATUS pma_status)
{
	memset(rm, 0, sizeof(*rm));
	rm->fb_status = fb_status;
	rm->pma_status = pma_status;
}

/*
 * fake_register - the registration of sys/uvm_gpu.c with the RM replaced by
 * the fake: the same helpers, the same branches.
 */
static void
fake_register(struct fake_rm *rm, const UvmGpuFbInfo *fb)
{
	enum uvm_fb_state state;
	struct uvm_fb_pma_verdict verdict;

	state = uvm_fb_info_state(rm->fb_status, fb);
	if (state == UVM_FB_BROKEN) {
		rm->failed = true;
		rm->message = "the frame buffer information is broken or missing";
		return;
	}
	verdict = uvm_fb_pma_judge(state, rm->pma_status);
	if (verdict.decision == UVM_FB_PMA_FAIL) {
		rm->failed = true;
		rm->message = verdict.reason;
		return;
	}
	if (verdict.decision == UVM_FB_PMA_ABSENT)
		return;			/* no PMA object, so no callbacks to register */
	rm->callbacks = true;
}

/*
 * fake_channel_alloc - the channel allocation of sys/uvm_channel.c with the
 * RM replaced by the fake: the placement, then the buffer locations that
 * the channel allocation asks for.
 */
static void
fake_channel_alloc(const UvmGpuFbInfo *fb, bool tunable_sysmem, NvU32 *fifo_loc, NvU32 *put_loc)
{
	enum uvm_fb_state state;
	enum uvm_fb_gpfifo_place place;
	bool sysmem;

	state = uvm_fb_info_state(NV_OK, fb);
	place = uvm_fb_gpfifo_place(tunable_sysmem, state);
	sysmem = uvm_fb_gpfifo_is_sysmem(place);
	*fifo_loc = uvm_fb_gpfifo_location(sysmem);
	*put_loc = *fifo_loc;
}

/* the frame buffer descriptions the rows use */
enum fb_desc {
	DESC_ZERO,		/* RM cleared the whole description */
	DESC_DISCRETE,		/* a 16 GB discrete GPU: sizes in KB */
	DESC_BROKEN,		/* the discrete description with bZeroFb set */
	DESC_HEAP,		/* only a heap size */
	DESC_RESERVED,		/* only a reserved heap size */
	DESC_ALLOC,		/* only an allocatable address */
	DESC_PAGE,		/* only a page size and BAR1 fields */
	DESC_EMPTY_BROKEN	/* bZeroFb on an empty description */
};

/*
 * fb_desc_info - the description of one of the rows: the same fields RM
 * fills in, so that the rows test the rules and not the filler.
 */
static void
fb_desc_info(enum fb_desc desc, UvmGpuFbInfo *fb)
{
	memset(fb, 0, sizeof(*fb));
	switch (desc) {
	case DESC_DISCRETE:
	case DESC_BROKEN:
		fb->heapSize = 16u * 1024 * 1024;
		fb->reservedHeapSize = 64 * 1024;
		fb->maxAllocatableAddress = (16ull << 30) - 1;
		fb->maxVidmemPageSize = 2u << 20;
		if (desc == DESC_BROKEN)
			fb->bZeroFb = 1;
		break;
	case DESC_HEAP:
		fb->heapSize = 1;
		break;
	case DESC_RESERVED:
		fb->reservedHeapSize = 1;
		break;
	case DESC_ALLOC:
		fb->maxAllocatableAddress = 4095;
		break;
	case DESC_PAGE:
		fb->maxVidmemPageSize = 2u << 20;
		fb->staticBar1Size = 1;
		break;
	case DESC_ZERO:
	case DESC_EMPTY_BROKEN:
		break;
	}
	/* RM never sets bZeroFb on a GPU without a frame buffer, but the rule
	 * must not depend on that: a broken frame buffer is broken */
	if (desc == DESC_EMPTY_BROKEN)
		fb->bZeroFb = 1;
}

/* the state of the frame buffer information */
struct state_case {
	int			line;
	NV_STATUS		fb_status;
	enum fb_desc		desc;
	enum uvm_fb_state	expect;
	const char		*msg;
};

static const struct state_case state_cases[] = {
	ROW(NV_OK, DESC_ZERO, UVM_FB_ABSENT,
	    "an all-zero description means a GPU without video memory"),
	ROW(NV_OK, DESC_DISCRETE, UVM_FB_USABLE,
	    "a heap and an allocatable address mean a GPU with video memory"),
	ROW(NV_OK, DESC_BROKEN, UVM_FB_BROKEN,
	    "bZeroFb is RM's broken answer, not the absence of video memory"),
	ROW(NV_ERR_GENERIC, DESC_ZERO, UVM_FB_BROKEN,
	    "an error status means the information is missing"),
	ROW(NV_ERR_GENERIC, DESC_DISCRETE, UVM_FB_BROKEN,
	    "an error status means the information is missing (discrete)"),
	ROW(NV_ERR_NOT_SUPPORTED, DESC_ZERO, UVM_FB_BROKEN,
	    "any other error status means the information is missing"),
	ROW(NV_OK, DESC_HEAP, UVM_FB_USABLE, "a heap size means video memory"),
	ROW(NV_OK, DESC_RESERVED, UVM_FB_USABLE,
	    "a reserved heap size means video memory"),
	ROW(NV_OK, DESC_ALLOC, UVM_FB_USABLE,
	    "an allocatable address means video memory"),
	ROW(NV_OK, DESC_PAGE, UVM_FB_ABSENT,
	    "page size and BAR1 fields alone are not video memory"),
	ROW(NV_OK, DESC_EMPTY_BROKEN, UVM_FB_BROKEN,
	    "bZeroFb on an empty description is still broken")
};

/* only a GPU that has no video memory has no video memory */
struct novid_case {
	int		line;
	enum fb_desc	desc;
	bool		expect;
	const char	*msg;
};

static const struct novid_case novid_cases[] = {
	ROW(DESC_ZERO, true, "an all-zero description has no video memory"),
	ROW(DESC_DISCRETE, false, "a discrete GPU has video memory"),
	ROW(DESC_BROKEN, false,
	    "a broken frame buffer is not a GPU without video memory")
};

/*
 * the PMA object: absent is normal only on a GPU without video memory.
 * A row that also has a reason message checks that the verdict carries a
 * reason as well.
 */
struct pma_case {
	int			line;
	enum uvm_fb_state	state;
	NV_STATUS		pma_status;
	enum uvm_fb_pma_decision	expect;
	const char		*msg;
	const char		*msg_reason;
};

static const struct pma_case pma_cases[] = {
	ROW(UVM_FB_ABSENT, NV_ERR_OBJECT_NOT_FOUND, UVM_FB_PMA_ABSENT,
	    "no PMA on a zero-FB GPU is normal", "the normal case has a reason"),
	ROW(UVM_FB_ABSENT, NV_OK, UVM_FB_PMA_FAIL,
	    "a PMA object on a GPU without video memory is a contradiction", NULL),
	ROW(UVM_FB_ABSENT, NV_ERR_NOT_SUPPORTED, UVM_FB_PMA_FAIL,
	    "another status stays an error", NULL),
	ROW(UVM_FB_USABLE, NV_OK, UVM_FB_PMA_USE,
	    "a discrete GPU with a PMA uses it", NULL),
	ROW(UVM_FB_USABLE, NV_ERR_OBJECT_NOT_FOUND, UVM_FB_PMA_FAIL,
	    "no PMA on a discrete GPU is an error", "the failure has a reason"),
	ROW(UVM_FB_USABLE, NV_ERR_INVALID_ARGUMENT, UVM_FB_PMA_FAIL,
	    "another status stays an error (discrete)", NULL),
	ROW(UVM_FB_USABLE, NV_ERR_NO_MEMORY, UVM_FB_PMA_FAIL,
	    "another status stays an error (discrete, 2)", NULL),
	ROW(UVM_FB_USABLE, NV_ERR_GENERIC, UVM_FB_PMA_FAIL,
	    "another status stays an error (discrete, 3)", NULL),
	ROW(UVM_FB_BROKEN, NV_OK, UVM_FB_PMA_FAIL,
	    "a broken frame buffer is an error", NULL),
	ROW(UVM_FB_BROKEN, NV_ERR_OBJECT_NOT_FOUND, UVM_FB_PMA_FAIL,
	    "a missing PMA does not excuse broken information", NULL),
	ROW(UVM_FB_BROKEN, NV_ERR_GENERIC, UVM_FB_PMA_FAIL,
	    "a broken frame buffer is an error (2)", NULL)
};

/*
 * the GPFIFO location: the tunable decides only where video memory exists.
 * A row that also has a message for the place checks where the ring is as
 * well.
 */
struct place_case {
	int			line;
	bool			tunable;
	enum uvm_fb_state	state;
	enum uvm_fb_gpfifo_place	expect;
	bool			expect_sysmem;
	const char		*msg;
	const char		*msg_sysmem;
};

static const struct place_case place_cases[] = {
	ROW(true, UVM_FB_USABLE, UVM_FB_GPFIFO_SYSMEM_TUNABLE, true,
	    "tunable 1 on a discrete GPU: system memory",
	    "tunable 1 on a discrete GPU: system memory"),
	ROW(false, UVM_FB_USABLE, UVM_FB_GPFIFO_VIDEO, false,
	    "tunable 0 on a discrete GPU: video memory",
	    "tunable 0 on a discrete GPU: video memory"),
	ROW(true, UVM_FB_ABSENT, UVM_FB_GPFIFO_SYSMEM_NO_VIDEO, true,
	    "tunable 1 on a zero-FB GPU: system memory, because there is no video memory",
	    NULL),
	ROW(false, UVM_FB_ABSENT, UVM_FB_GPFIFO_SYSMEM_NO_VIDEO, true,
	    "tunable 0 on a zero-FB GPU: still system memory, because there is no video memory",
	    NULL),
	ROW(false, UVM_FB_BROKEN, UVM_FB_GPFIFO_SYSMEM_UNUSABLE, true,
	    "a broken frame buffer leaves only system memory", NULL)
};

/* the buffer locations the channel allocation asks for */
struct loc_case {
	int	line;
	bool	sysmem;
	NvU32	expect;
	const char *msg;
};

static const struct loc_case loc_cases[] = {
	ROW(true, UVM_BUFFER_LOCATION_SYS, "system memory is the system buffer location"),
	ROW(false, UVM_BUFFER_LOCATION_VID, "video memory is the video buffer location")
};

/*
 * the registration of a GPU, with the RM replaced by the fake.  A row that
 * has a message for the callbacks and one for the message checks them; a
 * row without them checks only whether the registration failed.
 */
struct reg_case {
	int		line;
	NV_STATUS	fb_status;
	NV_STATUS	pma_status;
	enum fb_desc	desc;
	bool		expect_failed;
	bool		expect_callbacks;
	bool		expect_message;
	const char	*msg_failed;
	const char	*msg_callbacks;
	const char	*msg_message;
};

static const struct reg_case reg_cases[] = {
	ROW(NV_OK, NV_ERR_OBJECT_NOT_FOUND, DESC_ZERO, false, false, false,
	    "a zero-FB GPU with no PMA object registers",
	    "a zero-FB GPU registers no eviction callbacks",
	    "a zero-FB GPU with no PMA object has no failure message"),
	ROW(NV_OK, NV_ERR_OBJECT_NOT_FOUND, DESC_DISCRETE, true, false, true,
	    "a discrete GPU with no PMA object does not register",
	    "a failed registration registers no eviction callbacks",
	    "the failure says why"),
	ROW(NV_OK, NV_ERR_OBJECT_NOT_FOUND, DESC_BROKEN, true, false, false,
	    "a GPU whose frame buffer is broken does not register",
	    "a broken frame buffer registers no eviction callbacks", NULL),
	ROW(NV_ERR_GENERIC, NV_OK, DESC_ZERO, true, false, false,
	    "a GPU whose frame buffer information is missing does not register",
	    NULL, NULL),
	ROW(NV_OK, NV_OK, DESC_ZERO, true, false, false,
	    "a PMA object on a GPU whose description says it has no video memory does not register",
	    NULL, NULL),
	ROW(NV_OK, NV_OK, DESC_DISCRETE, false, true, false,
	    "a discrete GPU with a PMA object registers",
	    "a discrete GPU with a PMA object registers the eviction callbacks", NULL)
};

/* the channel allocation of a GPU, with the RM replaced by the fake */
struct chan_case {
	int		line;
	enum fb_desc	desc;
	bool		tunable;
	NvU32	expect_fifo;
	NvU32	expect_put;
	const char	*msg_fifo;
	const char	*msg_put;
};

static const struct chan_case chan_cases[] = {
	ROW(DESC_ZERO, false, UVM_BUFFER_LOCATION_SYS, UVM_BUFFER_LOCATION_SYS,
	    "a zero-FB GPU puts the GPFIFO in system memory",
	    "a zero-FB GPU puts the PUT word in system memory"),
	ROW(DESC_ZERO, true, UVM_BUFFER_LOCATION_SYS, UVM_BUFFER_LOCATION_SYS,
	    "a zero-FB GPU puts the GPFIFO in system memory (2)",
	    "a zero-FB GPU puts the PUT word in system memory (2)"),
	ROW(DESC_DISCRETE, false, UVM_BUFFER_LOCATION_VID, UVM_BUFFER_LOCATION_VID,
	    "a discrete GPU keeps the GPFIFO in video memory",
	    "a discrete GPU keeps the PUT word in video memory"),
	ROW(DESC_DISCRETE, true, UVM_BUFFER_LOCATION_SYS, UVM_BUFFER_LOCATION_SYS,
	    "the tunable moves the GPFIFO to system memory",
	    "the tunable moves the PUT word to system memory")
};

static void
run_state_cases(void)
{
	UvmGpuFbInfo fb;
	size_t i;

	for (i = 0; i < ROWS(state_cases); i++) {
		const struct state_case *c = &state_cases[i];

		fb_desc_info(c->desc, &fb);
		ROW_CHECK(uvm_fb_info_state(c->fb_status, &fb) == c->expect,
		    c->line, "%s", c->msg);
	}
}

static void
run_novid_cases(void)
{
	UvmGpuFbInfo fb;
	size_t i;

	for (i = 0; i < ROWS(novid_cases); i++) {
		const struct novid_case *c = &novid_cases[i];

		fb_desc_info(c->desc, &fb);
		ROW_CHECK(uvm_fb_no_video_memory(&fb) == c->expect,
		    c->line, "%s", c->msg);
	}
}

static void
run_pma_cases(void)
{
	size_t i;

	for (i = 0; i < ROWS(pma_cases); i++) {
		const struct pma_case *c = &pma_cases[i];
		struct uvm_fb_pma_verdict verdict;

		verdict = uvm_fb_pma_judge(c->state, c->pma_status);
		ROW_CHECK(verdict.decision == c->expect, c->line, "%s", c->msg);
		if (c->msg_reason != NULL)
			ROW_CHECK(verdict.reason != NULL && verdict.reason[0] != '\0',
			    c->line, "%s", c->msg_reason);
	}
}

static void
run_place_cases(void)
{
	size_t i;

	for (i = 0; i < ROWS(place_cases); i++) {
		const struct place_case *c = &place_cases[i];
		enum uvm_fb_gpfifo_place place;

		place = uvm_fb_gpfifo_place(c->tunable, c->state);
		ROW_CHECK(place == c->expect, c->line, "%s", c->msg);
		if (c->msg_sysmem != NULL)
			ROW_CHECK(uvm_fb_gpfifo_is_sysmem(place) == c->expect_sysmem,
			    c->line, "%s", c->msg_sysmem);
	}
}

static void
run_loc_cases(void)
{
	size_t i;

	for (i = 0; i < ROWS(loc_cases); i++) {
		const struct loc_case *c = &loc_cases[i];

		ROW_CHECK(uvm_fb_gpfifo_location(c->sysmem) == c->expect,
		    c->line, "%s", c->msg);
	}
}

static void
run_reg_cases(void)
{
	UvmGpuFbInfo fb;
	struct fake_rm rm;
	size_t i;

	for (i = 0; i < ROWS(reg_cases); i++) {
		const struct reg_case *c = &reg_cases[i];

		fb_desc_info(c->desc, &fb);
		fake_rm_init(&rm, c->fb_status, c->pma_status);
		fake_register(&rm, &fb);
		ROW_CHECK(rm.failed == c->expect_failed, c->line, "%s", c->msg_failed);
		if (c->msg_callbacks != NULL)
			ROW_CHECK(rm.callbacks == c->expect_callbacks,
			    c->line, "%s", c->msg_callbacks);
		if (c->msg_message != NULL)
			ROW_CHECK((rm.message != NULL) == c->expect_message,
			    c->line, "%s", c->msg_message);
	}
}

static void
run_chan_cases(void)
{
	UvmGpuFbInfo fb;
	NvU32 fifo_loc, put_loc;
	size_t i;

	for (i = 0; i < ROWS(chan_cases); i++) {
		const struct chan_case *c = &chan_cases[i];

		fb_desc_info(c->desc, &fb);
		fake_channel_alloc(&fb, c->tunable, &fifo_loc, &put_loc);
		ROW_CHECK(fifo_loc == c->expect_fifo, c->line, "%s", c->msg_fifo);
		ROW_CHECK(put_loc == c->expect_put, c->line, "%s", c->msg_put);
	}
}

int
main(void)
{
	run_state_cases();
	run_novid_cases();
	run_pma_cases();
	run_place_cases();
	run_loc_cases();
	run_reg_cases();
	run_chan_cases();

	if (fails == 0)
		printf("fb_state_test: all rows pass\n");
	return (fails != 0);
}
