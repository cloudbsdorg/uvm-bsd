/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * uvm_core_device.c - the device registry of the UVM core.
 *
 * The registry is a list of backends and a table of devices, kept under
 * one lock of the core's own.  A device is an object the core allocates:
 * the public struct uvm_core_device the caller sees, and beside it the
 * core's own fields (the reference count, the identity, the backend the
 * device came from), so that the core can find them again from the public
 * pointer.
 *
 * The registry names no device vendor: a backend is asked to take an
 * identity it alone understands, and the core only remembers what the
 * backend says about the device.
 */

#include "uvm_core_device.h"
#include "uvm_core_os.h"

#ifdef _KERNEL
#include <sys/errno.h>
#include <sys/libkern.h>
#else
#include <errno.h>
#include <string.h>
#endif

#ifdef _KERNEL
/* the zone the core's own memory comes from */
MALLOC_DEFINE(M_UVM_CORE, "uvm core", NULL);
#endif

/*
 * A device of the core: the public device first, so that the core gets
 * back from a public pointer to the private one, and the core's own
 * fields beside it.
 */
struct uvm_core_device_priv {
	struct uvm_core_device	pub;		/* first: the device the caller sees */
	unsigned		refs;		/* the core's own count */
	uint8_t			identity[UVM_CORE_IDENTITY_MAX];
	size_t			identity_len;
	struct uvm_core_backend	*backend;	/* the backend the device came from */
};

/* one backend the core holds */
struct uvm_core_backend_slot {
	struct uvm_core_backend	*backend;
};

/* the registry itself */
static struct uvm_core_rwlock uvm_core_registry_lock;
static struct uvm_core_backend_slot uvm_core_backends[UVM_CORE_BACKEND_MAX];
static struct uvm_core_device_priv *uvm_core_devices[UVM_CORE_DEVICE_ID_LAST + 1];

/* ---- the backends ---- */

/*
 * uvm_core_backend_taken - whether the backend is already registered,
 * by its own pointer or by its name.
 * Time: O(n) in the backends.  Space: O(1).
 */
static int
uvm_core_backend_taken(struct uvm_core_backend *backend)
{
	int i;

	for (i = 0; i < UVM_CORE_BACKEND_MAX; i++) {
		struct uvm_core_backend *other = uvm_core_backends[i].backend;

		if (other == NULL)
			continue;
		if (other == backend)
			return (1);
		if (backend->name != NULL && other->name != NULL &&
		    strcmp(other->name, backend->name) == 0)
			return (1);
	}
	return (0);
}

/*
 * uvm_core_backend_has_devices - whether devices of that backend are
 * still in the registry.
 * Time: O(n) in the devices.  Space: O(1).
 */
static int
uvm_core_backend_has_devices(struct uvm_core_backend *backend)
{
	int i;

	for (i = UVM_CORE_DEVICE_ID_FIRST; i <= UVM_CORE_DEVICE_ID_LAST; i++)
		if (uvm_core_devices[i] != NULL && uvm_core_devices[i]->backend == backend)
			return (1);
	return (0);
}

/*
 * uvm_core_backend_register - the backend joins the core.  EEXIST when
 * it is already there, ENOSPC when there is no room for it, EINVAL when
 * it cannot ask for a device at all.
 * Time: O(n) in the backends.  Space: O(1).
 */
int
uvm_core_backend_register(struct uvm_core_backend *backend)
{
	int i, error = 0;

	if (backend == NULL || backend->probe_by_identity == NULL)
		return (EINVAL);

	uvm_core_rwlock_wlock(&uvm_core_registry_lock);
	if (uvm_core_backend_taken(backend)) {
		error = EEXIST;
		goto out;
	}
	for (i = 0; i < UVM_CORE_BACKEND_MAX; i++)
		if (uvm_core_backends[i].backend == NULL) {
			uvm_core_backends[i].backend = backend;
			goto out;
		}
	error = ENOSPC;
out:
	uvm_core_rwlock_wunlock(&uvm_core_registry_lock);
	return (error);
}

/*
 * uvm_core_backend_unregister_checked - the backend leaves the core.
 * EBUSY when devices of that backend are still there, ENOENT when the
 * backend was not registered, 0 when it left.
 * Time: O(n) in the backends and in the devices.  Space: O(1).
 */
int
uvm_core_backend_unregister_checked(struct uvm_core_backend *backend)
{
	int i, error = 0;

	uvm_core_rwlock_wlock(&uvm_core_registry_lock);
	for (i = 0; i < UVM_CORE_BACKEND_MAX; i++) {
		if (uvm_core_backends[i].backend != backend)
			continue;
		if (uvm_core_backend_has_devices(backend)) {
			error = EBUSY;
			goto out;
		}
		uvm_core_backends[i].backend = NULL;
		goto out;
	}
	error = ENOENT;
out:
	uvm_core_rwlock_wunlock(&uvm_core_registry_lock);
	return (error);
}

/*
 * uvm_core_backend_unregister - the backend leaves the core; the answer
 * is not reported, so a backend that still has devices stays.
 * Time: O(n) in the backends and in the devices.  Space: O(1).
 */
void
uvm_core_backend_unregister(struct uvm_core_backend *backend)
{
	(void)uvm_core_backend_unregister_checked(backend);
}

/* ---- the devices ---- */

/*
 * uvm_core_device_free_id - the lowest id nobody has, or 0 when they are
 * all taken.
 * Time: O(n) in the ids.  Space: O(1).
 */
static uvm_core_id_t
uvm_core_device_free_id(void)
{
	int i;

	for (i = UVM_CORE_DEVICE_ID_FIRST; i <= UVM_CORE_DEVICE_ID_LAST; i++)
		if (uvm_core_devices[i] == NULL)
			return ((uvm_core_id_t)i);
	return (0);
}

/*
 * uvm_core_device_find_identity - the device that is already there for
 * those identity bytes, or NULL when there is none.
 * Time: O(n) in the devices.  Space: O(1).
 */
static struct uvm_core_device_priv *
uvm_core_device_find_identity(const void *identity, size_t len)
{
	int i;

	for (i = UVM_CORE_DEVICE_ID_FIRST; i <= UVM_CORE_DEVICE_ID_LAST; i++) {
		struct uvm_core_device_priv *priv = uvm_core_devices[i];

		if (priv == NULL || priv->identity_len != len)
			continue;
		if (memcmp(priv->identity, identity, len) == 0)
			return (priv);
	}
	return (NULL);
}

/*
 * uvm_core_device_take - one more reference on the device, which the
 * device's retain operation is told about.
 * Time: O(1).  Space: O(1).
 */
static int
uvm_core_device_take(struct uvm_core_device_priv *priv)
{
	int error;

	priv->refs++;
	if (priv->pub.ops != NULL && priv->pub.ops->retain != NULL) {
		error = priv->pub.ops->retain(&priv->pub);
		if (error != 0) {
			priv->refs--;
			return (error);
		}
	}
	return (0);
}

/*
 * uvm_core_device_free - the device is gone: its release operation is
 * called, the backend is told, and the memory of the device is given
 * back.  The id is freed by the caller.
 * Time: O(1).  Space: O(1).
 */
static void
uvm_core_device_free(struct uvm_core_device_priv *priv)
{
	if (priv->pub.ops != NULL && priv->pub.ops->release != NULL)
		(void)priv->pub.ops->release(&priv->pub);
	if (priv->backend != NULL && priv->backend->release != NULL)
		(void)priv->backend->release(&priv->pub);
	uvm_core_free(priv);
}

/*
 * uvm_core_backend_probe - the backends are asked, in the order they
 * registered themselves, to take the identity.  The one that takes it
 * fills the device; 0 and that backend when there is one, ENODEV when
 * none of them takes the identity.
 * Time: O(n) in the backends.  Space: O(1).
 */
static int
uvm_core_backend_probe(const void *identity, size_t len, uvm_core_id_t id,
    struct uvm_core_device *device, struct uvm_core_backend **out_backend)
{
	int i;

	for (i = 0; i < UVM_CORE_BACKEND_MAX; i++) {
		struct uvm_core_backend *backend = uvm_core_backends[i].backend;
		int error;

		if (backend == NULL)
			continue;
		error = backend->probe_by_identity(identity, len, id, device);
		if (error == 0) {
			*out_backend = backend;
			return (0);
		}
	}
	return (ENODEV);
}

/*
 * uvm_core_device_new - the device of an identity nobody has yet: the
 * lowest free id is taken, the backends are asked to take the identity,
 * and the device's describe operation fills its capabilities.
 * Time: O(n) in the ids and in the backends.  Space: O(1).
 */
static int
uvm_core_device_new(const void *identity, size_t len, struct uvm_core_device **out)
{
	struct uvm_core_device_priv *priv;
	struct uvm_core_backend *backend = NULL;
	uvm_core_id_t id;
	int error;

	id = uvm_core_device_free_id();
	if (id == 0)
		return (ENOSPC);
	priv = uvm_core_alloc(sizeof(*priv));
	if (priv == NULL)
		return (ENOMEM);
	memset(priv, 0, sizeof(*priv));
	priv->pub.id = id;
	memcpy(priv->identity, identity, len);
	priv->identity_len = len;

	error = uvm_core_backend_probe(identity, len, id, &priv->pub, &backend);
	if (error != 0) {
		uvm_core_free(priv);
		return (error);
	}
	priv->backend = backend;

	if (priv->pub.ops != NULL && priv->pub.ops->describe != NULL) {
		error = priv->pub.ops->describe(&priv->pub, &priv->pub.caps);
		if (error != 0) {
			uvm_core_device_free(priv);
			return (error);
		}
	}

	priv->refs = 1;		/* the reference the probe itself takes */
	uvm_core_devices[id] = priv;
	*out = &priv->pub;
	return (0);
}

/*
 * uvm_core_registry_init - the registry is ready for the backends.
 * Time: O(n) in the backends and in the ids.  Space: O(1).
 */
void
uvm_core_registry_init(void)
{
	memset(uvm_core_backends, 0, sizeof(uvm_core_backends));
	memset(uvm_core_devices, 0, sizeof(uvm_core_devices));
	uvm_core_rwlock_init(&uvm_core_registry_lock);
}

/*
 * uvm_core_registry_deinit - the registry is empty and its lock is gone.
 * A device that is still there is let go, so that the backend that
 * brought it knows.
 * Time: O(n) in the devices and in the ids.  Space: O(1).
 */
void
uvm_core_registry_deinit(void)
{
	int i;

	uvm_core_rwlock_wlock(&uvm_core_registry_lock);
	for (i = UVM_CORE_DEVICE_ID_FIRST; i <= UVM_CORE_DEVICE_ID_LAST; i++) {
		struct uvm_core_device_priv *priv = uvm_core_devices[i];

		if (priv == NULL)
			continue;
		uvm_core_devices[i] = NULL;
		uvm_core_device_free(priv);
	}
	memset(uvm_core_backends, 0, sizeof(uvm_core_backends));
	uvm_core_rwlock_wunlock(&uvm_core_registry_lock);
	uvm_core_rwlock_destroy(&uvm_core_registry_lock);
}

/*
 * uvm_core_device_probe - the device an identity names.
 * Time: O(n) in the devices and in the backends.  Space: O(1).
 */
int
uvm_core_device_probe(const void *identity, size_t identity_len, struct uvm_core_device **out)
{
	struct uvm_core_device_priv *priv;
	size_t len;
	int error;

	if (out != NULL)
		*out = NULL;
	if (identity == NULL || identity_len == 0)
		return (EINVAL);
	len = (identity_len > UVM_CORE_IDENTITY_MAX) ? UVM_CORE_IDENTITY_MAX : identity_len;

	uvm_core_rwlock_wlock(&uvm_core_registry_lock);
	priv = uvm_core_device_find_identity(identity, len);
	if (priv != NULL) {
		/* the device is already there: one more reference, no backend asked */
		error = uvm_core_device_take(priv);
		if (error == 0 && out != NULL)
			*out = &priv->pub;
		uvm_core_rwlock_wunlock(&uvm_core_registry_lock);
		return (error);
	}
	error = uvm_core_device_new(identity, len, out);
	uvm_core_rwlock_wunlock(&uvm_core_registry_lock);
	return (error);
}

/*
 * uvm_core_device_lookup - the device with the id `id`, or NULL when
 * there is none.
 * Time: O(1).  Space: O(1).
 */
struct uvm_core_device *
uvm_core_device_lookup(uvm_core_id_t id)
{
	struct uvm_core_device *dev = NULL;

	if (id < UVM_CORE_DEVICE_ID_FIRST || id > UVM_CORE_DEVICE_ID_LAST)
		return (NULL);
	uvm_core_rwlock_rlock(&uvm_core_registry_lock);
	if (uvm_core_devices[id] != NULL)
		dev = &uvm_core_devices[id]->pub;
	uvm_core_rwlock_runlock(&uvm_core_registry_lock);
	return (dev);
}

/*
 * uvm_core_device_retain - take a reference on the device.
 * Time: O(1).  Space: O(1).
 */
void
uvm_core_device_retain(struct uvm_core_device *dev)
{
	struct uvm_core_device_priv *priv;

	if (dev == NULL)
		return;
	priv = UVM_CORE_CONTAINEROF(dev, struct uvm_core_device_priv, pub);
	uvm_core_rwlock_wlock(&uvm_core_registry_lock);
	(void)uvm_core_device_take(priv);
	uvm_core_rwlock_wunlock(&uvm_core_registry_lock);
}

/*
 * uvm_core_device_release - give back a reference; the device is gone
 * when the last one goes.
 * Time: O(1).  Space: O(1).
 */
void
uvm_core_device_release(struct uvm_core_device *dev)
{
	struct uvm_core_device_priv *priv;

	if (dev == NULL)
		return;
	priv = UVM_CORE_CONTAINEROF(dev, struct uvm_core_device_priv, pub);
	uvm_core_rwlock_wlock(&uvm_core_registry_lock);
	if (priv->refs == 0) {
		uvm_core_rwlock_wunlock(&uvm_core_registry_lock);
		return;
	}
	priv->refs--;
	if (priv->refs == 0) {
		uvm_core_devices[priv->pub.id] = NULL;
		uvm_core_device_free(priv);
	}
	uvm_core_rwlock_wunlock(&uvm_core_registry_lock);
}

/*
 * uvm_core_device_foreach - walk over the devices and call `fn` on each
 * of them; the walk stops at the first answer that is not 0.
 * Time: O(n) in the devices.  Space: O(1).
 */
int
uvm_core_device_foreach(int (*fn)(struct uvm_core_device *, void *), void *arg)
{
	int i, error = 0;

	if (fn == NULL)
		return (EINVAL);
	uvm_core_rwlock_rlock(&uvm_core_registry_lock);
	for (i = UVM_CORE_DEVICE_ID_FIRST; i <= UVM_CORE_DEVICE_ID_LAST; i++) {
		struct uvm_core_device_priv *priv = uvm_core_devices[i];

		if (priv == NULL)
			continue;
		error = fn(&priv->pub, arg);
		if (error != 0)
			break;
	}
	uvm_core_rwlock_runlock(&uvm_core_registry_lock);
	return (error);
}
