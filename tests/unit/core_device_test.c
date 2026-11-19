/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * core_device_test.c - the device registry of sys/core/uvm_core_device.h
 * behaves as the core/backend split of docs/ARCHITECTURE.md needs it to: a
 * backend registers itself, an identity is probed to a device with an id
 * and the capabilities the backend describes, the same identity gives the
 * same device with one more reference, an identity no backend accepts is
 * refused, a device is found by its id, the last reference frees the id
 * again, and a backend that is unregistered is asked nothing more.
 *
 * The backend is a fake one, defined here: it is not a device of any
 * kind, it only accepts an identity whose first byte says so, and it
 * counts what the core asks of it.
 *
 * Red first: compiled before the header existed.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/uvm_core_device.h"
#include "core/uvm_core_types.h"

static int fails;

#define	CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fails++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

/* ---- the fake backend ---- */

/* the identity the fake backend accepts: 16 bytes whose first byte is this */
#define	FAKE_IDENTITY_LEN	16
#define	FAKE_FIRST_BYTE		0x5A

/* what the fake backend counts */
static unsigned fake_probes;
static unsigned fake_retains;
static unsigned fake_releases;
static unsigned fake_backend_releases;

/* the memory the fake backend says it has: 1 GiB */
#define	FAKE_MEMORY_BYTES	(1024ULL * 1024 * 1024)

/*
 * fake_describe - what the fake device can do: its name, its memory and
 * the page sizes it supports.
 */
static int
fake_describe(struct uvm_core_device *device, struct uvm_core_device_caps *caps)
{
	(void)device;
	memset(caps, 0, sizeof(*caps));
	strlcpy(caps->name, "fake device", sizeof(caps->name));
	caps->memory_bytes = FAKE_MEMORY_BYTES;
	caps->page_sizes = UVM_CORE_PAGE_SIZE_BIT_4K | UVM_CORE_PAGE_SIZE_BIT_64K |
	    UVM_CORE_PAGE_SIZE_BIT_2M;
	return (0);
}

/* fake_retain - the core took a reference on the fake device. */
static int
fake_retain(struct uvm_core_device *device)
{
	(void)device;
	fake_retains++;
	return (0);
}

/* fake_release - the last reference on the fake device went. */
static int
fake_release(struct uvm_core_device *device)
{
	(void)device;
	fake_releases++;
	return (0);
}

/* the operations the fake device offers the core */
static struct uvm_core_device_ops fake_device_ops = {
	.retain = fake_retain,
	.release = fake_release,
	.describe = fake_describe,
};

/*
 * fake_probe_by_identity - the core offers an identity; the fake backend
 * takes it when it is one of its own and says no otherwise.
 */
static int
fake_probe_by_identity(const void *identity, size_t identity_len, uvm_core_id_t id,
    struct uvm_core_device *device)
{
	const uint8_t *bytes = identity;

	if (identity == NULL || identity_len != FAKE_IDENTITY_LEN)
		return (ENODEV);
	if (bytes[0] != FAKE_FIRST_BYTE)
		return (ENODEV);
	fake_probes++;
	device->ops = &fake_device_ops;
	(void)id;
	return (0);
}

/* fake_backend_release - the core let the fake device go. */
static int
fake_backend_release(struct uvm_core_device *device)
{
	(void)device;
	fake_backend_releases++;
	return (0);
}

/* the backend itself */
static struct uvm_core_backend fake_backend = {
	.name = "testvendor",
	.probe_by_identity = fake_probe_by_identity,
	.release = fake_backend_release,
};

/* ---- the helpers ---- */

/* an identity of the fake backend's own making, numbered by `tag` */
static void
fake_identity(uint8_t *identity, uint8_t tag)
{
	memset(identity, 0, FAKE_IDENTITY_LEN);
	identity[0] = FAKE_FIRST_BYTE;
	identity[FAKE_IDENTITY_LEN - 1] = tag;
}

/* an identity no backend accepts */
static void
fake_foreign_identity(uint8_t *identity)
{
	memset(identity, 0, FAKE_IDENTITY_LEN);
	identity[0] = 0x11;
}

/* a callback for the walk over the devices: it counts them */
static int
fake_count_device(struct uvm_core_device *device, void *arg)
{
	unsigned *count = arg;

	(void)device;
	(*count)++;
	return (0);
}

/* #lizard forgives -- a list of independent checks, one branch each */
int
main(void)
{
	struct uvm_core_device *dev1, *dev2, *dev3, *dev4, *found;
	uint8_t identity1[FAKE_IDENTITY_LEN];
	uint8_t identity2[FAKE_IDENTITY_LEN];
	uint8_t identity3[FAKE_IDENTITY_LEN];
	uint8_t identity4[FAKE_IDENTITY_LEN];
	uint8_t foreign[FAKE_IDENTITY_LEN];
	unsigned counted = 0;
	int error;

	uvm_core_registry_init();

	/* ---- the backend registers itself ---- */
	error = uvm_core_backend_register(&fake_backend);
	CHECK(error == 0, "the backend registers itself");

	/* ---- the first identity becomes a device ---- */
	fake_identity(identity1, 1);
	fake_identity(identity2, 2);
	fake_identity(identity3, 3);
	fake_identity(identity4, 4);
	fake_foreign_identity(foreign);

	dev1 = NULL;
	error = uvm_core_device_probe(identity1, sizeof(identity1), &dev1);
	CHECK(error == 0, "an identity the backend accepts is probed");
	CHECK(dev1 != NULL, "the probed device is there");
	CHECK(dev1->id == 1, "the first device has the first id");
	CHECK(fake_probes == 1, "the backend was asked once");
	CHECK(dev1->caps.memory_bytes == FAKE_MEMORY_BYTES,
	    "the memory the device describes is in its capabilities");
	CHECK(dev1->caps.page_sizes == (UVM_CORE_PAGE_SIZE_BIT_4K | UVM_CORE_PAGE_SIZE_BIT_64K |
	    UVM_CORE_PAGE_SIZE_BIT_2M), "the page sizes the device describes are in its capabilities");

	/* ---- a second identity becomes a second device ---- */
	dev2 = NULL;
	error = uvm_core_device_probe(identity2, sizeof(identity2), &dev2);
	CHECK(error == 0, "a second identity is probed");
	CHECK(dev2 != NULL && dev2->id == 2, "the second device has the second id");
	CHECK(fake_probes == 2, "the backend was asked twice");

	/* ---- the same identity gives the same device, with one more reference ---- */
	found = NULL;
	error = uvm_core_device_probe(identity1, sizeof(identity1), &found);
	CHECK(error == 0, "the same identity is probed again");
	CHECK(found == dev1, "the same identity gives the same device");
	CHECK(fake_probes == 2, "the backend was not asked again for the same identity");
	CHECK(fake_retains == 1, "the extra reference was taken from the device");

	/* ---- an identity no backend accepts is refused ---- */
	found = dev1;
	error = uvm_core_device_probe(foreign, sizeof(foreign), &found);
	CHECK(error == ENODEV, "an identity no backend accepts is refused");
	CHECK(found == NULL, "a refused probe gives no device");
	CHECK(fake_probes == 2, "a refused probe asked no backend that accepts");

	/* ---- a device is found by its id ---- */
	CHECK(uvm_core_device_lookup(1) == dev1, "the first device is found by its id");
	CHECK(uvm_core_device_lookup(2) == dev2, "the second device is found by its id");
	CHECK(uvm_core_device_lookup(0) == NULL, "the processor id of the CPU is no device");
	CHECK(uvm_core_device_lookup(99) == NULL, "an id nobody has is no device");

	/* ---- the walk over the devices ---- */
	counted = 0;
	CHECK(uvm_core_device_foreach(fake_count_device, &counted) == 0, "the walk over the devices ends");
	CHECK(counted == 2, "the walk saw the two devices");

	/* ---- the last reference frees the id again ---- */
	uvm_core_device_release(dev2);
	CHECK(fake_releases == 1, "the device was told that its last reference went");
	CHECK(fake_backend_releases == 1, "the backend let the device go");
	CHECK(uvm_core_device_lookup(2) == NULL, "the id of the released device is free again");

	/* ---- a new device takes the lowest free id ---- */
	dev3 = NULL;
	error = uvm_core_device_probe(identity3, sizeof(identity3), &dev3);
	CHECK(error == 0, "a third identity is probed");
	CHECK(dev3 != NULL && dev3->id == 2, "the new device took the lowest free id");

	/* ---- the device with two references lives until both are gone ---- */
	uvm_core_device_release(dev1);
	CHECK(uvm_core_device_lookup(1) == dev1, "the device is still there with one reference left");
	CHECK(fake_releases == 1, "the device with a reference left is not released yet");
	uvm_core_device_release(dev1);
	CHECK(uvm_core_device_lookup(1) == NULL, "the id of the released device is free again");
	CHECK(fake_releases == 2, "the device was released when its last reference went");

	/* ---- and the id it had is the lowest free one again ---- */
	dev4 = NULL;
	error = uvm_core_device_probe(identity4, sizeof(identity4), &dev4);
	CHECK(error == 0, "a fourth identity is probed");
	CHECK(dev4 != NULL && dev4->id == 1, "the new device took the lowest free id again");

	/* ---- every device is let go, so the backend can go too ---- */
	uvm_core_device_release(dev3);
	uvm_core_device_release(dev4);
	CHECK(fake_backend_releases == 4, "the backend let every device go");

	/* ---- an unregistered backend is asked nothing more ---- */
	uvm_core_backend_unregister(&fake_backend);
	found = NULL;
	error = uvm_core_device_probe(identity1, sizeof(identity1), &found);
	CHECK(error == ENODEV, "no backend accepts an identity once the backend is gone");
	CHECK(fake_probes == 4, "the backend that is gone was asked nothing");

	/* ---- the same backend does not register twice ---- */
	error = uvm_core_backend_register(&fake_backend);
	CHECK(error == 0, "the backend registers itself again");
	error = uvm_core_backend_register(&fake_backend);
	CHECK(error == EEXIST, "the same backend does not register twice");
	uvm_core_backend_unregister(&fake_backend);

	uvm_core_registry_deinit();

	if (fails != 0) {
		printf("%d check(s) failed\n", fails);
		return (1);
	}
	printf("core_device_test: ok\n");
	return (0);
}
