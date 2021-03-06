/*
 * Copyright (C) 2013-2014 Kay Sievers
 * Copyright (C) 2013-2014 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013-2014 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013-2014 David Herrmann <dh.herrmann@gmail.com>
 * Copyright (C) 2013-2014 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "bus.h"
#include "connection.h"
#include "domain.h"
#include "endpoint.h"
#include "handle.h"
#include "item.h"
#include "message.h"
#include "policy.h"

/* endpoints are by default owned by the bus owner */
static char *kdbus_devnode_ep(struct device *dev, umode_t *mode,
			      kuid_t *uid, kgid_t *gid)
{
	struct kdbus_ep *ep = container_of(dev, struct kdbus_ep, dev);

	if (mode)
		*mode = ep->mode;
	if (uid)
		*uid = ep->uid;
	if (gid)
		*gid = ep->gid;

	return NULL;
}

static void kdbus_dev_release(struct device *dev)
{
	kfree(dev);
}

static struct device_type kdbus_devtype_ep = {
	.name		= "endpoint",
	.release	= kdbus_dev_release,
	.devnode	= kdbus_devnode_ep,
};

struct kdbus_ep *kdbus_ep_ref(struct kdbus_ep *ep)
{
	get_device(&ep->dev);
	return ep;
}

/**
 * kdbus_ep_disconnect() - disconnect an endpoint
 * @ep:			Endpoint
 */
void kdbus_ep_disconnect(struct kdbus_ep *ep)
{
	mutex_lock(&ep->lock);
	if (ep->disconnected) {
		mutex_unlock(&ep->lock);
		return;
	}
	ep->disconnected = true;
	mutex_unlock(&ep->lock);

	/* disconnect from bus */
	mutex_lock(&ep->bus->lock);
	list_del(&ep->bus_entry);
	mutex_unlock(&ep->bus->lock);

	if (device_is_registered(&ep->dev))
		device_del(&ep->dev);

	kdbus_minor_set(ep->dev.devt, KDBUS_MINOR_EP, NULL);

	/* disconnect all connections to this endpoint */
	for (;;) {
		struct kdbus_conn *conn;

		mutex_lock(&ep->lock);
		conn = list_first_entry_or_null(&ep->conn_list,
						struct kdbus_conn,
						ep_entry);
		if (!conn) {
			mutex_unlock(&ep->lock);
			break;
		}

		/* take reference, release lock, disconnect without lock */
		kdbus_conn_ref(conn);
		mutex_unlock(&ep->lock);

		kdbus_conn_disconnect(conn, false);
		kdbus_conn_unref(conn);
	}
}

static void __kdbus_ep_free(struct device *dev)
{
	struct kdbus_ep *ep = container_of(dev, struct kdbus_ep, dev);

	BUG_ON(!ep->disconnected);
	BUG_ON(!list_empty(&ep->conn_list));

	kdbus_policy_db_clear(&ep->policy_db);
	kdbus_minor_free(ep->dev.devt);
	kdbus_bus_unref(ep->bus);
	kdbus_domain_user_unref(ep->user);
	kfree(ep->name);
	kfree(ep);
}

struct kdbus_ep *kdbus_ep_unref(struct kdbus_ep *ep)
{
	if (ep)
		put_device(&ep->dev);
	return NULL;
}

static struct kdbus_ep *kdbus_ep_find(struct kdbus_bus *bus, const char *name)
{
	struct kdbus_ep *e;

	list_for_each_entry(e, &bus->ep_list, bus_entry)
		if (!strcmp(e->name, name))
			return e;

	return NULL;
}

/**
 * kdbus_ep_new() - create a new endpoint
 * @bus:		The bus this endpoint will be created for
 * @name:		The name of the endpoint
 * @mode:		The access mode for the device node
 * @uid:		The uid of the device node
 * @gid:		The gid of the device node
 * @policy:		Whether or not the endpoint should have a policy db
 *
 * This function will create a new enpoint with the given
 * name and properties for a given bus.
 *
 * Return: a new kdbus_ep on success, ERR_PTR on failure.
 */
struct kdbus_ep *kdbus_ep_new(struct kdbus_bus *bus, const char *name,
			      umode_t mode, kuid_t uid, kgid_t gid,
			      bool policy)
{
	struct kdbus_ep *e;
	int ret;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return ERR_PTR(-ENOMEM);

	e->disconnected = true;
	mutex_init(&e->lock);
	INIT_LIST_HEAD(&e->conn_list);
	kdbus_policy_db_init(&e->policy_db);
	e->uid = uid;
	e->gid = gid;
	e->mode = mode;
	e->has_policy = policy;

	device_initialize(&e->dev);
	e->dev.bus = &kdbus_subsys;
	e->dev.type = &kdbus_devtype_ep;
	e->dev.release = __kdbus_ep_free;

	e->name = kstrdup(name, GFP_KERNEL);
	if (!e->name) {
		ret = -ENOMEM;
		goto exit_put;
	}

	ret = dev_set_name(&e->dev, "%s/%s/%s",
			   bus->domain->devpath, bus->name, name);
	if (ret < 0)
		goto exit_put;

	ret = kdbus_minor_alloc(KDBUS_MINOR_EP, NULL, &e->dev.devt);
	if (ret < 0)
		goto exit_put;

	mutex_lock(&bus->lock);

	if (bus->disconnected) {
		mutex_unlock(&bus->lock);
		ret = -ESHUTDOWN;
		goto exit_put;
	}

	if (kdbus_ep_find(bus, name)) {
		mutex_unlock(&bus->lock);
		ret = -EEXIST;
		goto exit_put;
	}

	e->bus = kdbus_bus_ref(bus);
	list_add_tail(&e->bus_entry, &bus->ep_list);

	e->id = ++bus->ep_seq_last;

	/*
	 * Same as with domains, we have to mark it enabled _before_ running
	 * device_add() to avoid messing with state after UEVENT_ADD was sent.
	 */

	e->disconnected = false;
	kdbus_minor_set_ep(e->dev.devt, e);

	ret = device_add(&e->dev);

	mutex_unlock(&bus->lock);

	if (ret < 0) {
		kdbus_ep_disconnect(e);
		kdbus_ep_unref(e);
		return ERR_PTR(ret);
	}

	return e;

exit_put:
	put_device(&e->dev);
	return ERR_PTR(ret);
}

/**
 * kdbus_ep_policy_set() - set policy for an endpoint
 * @ep:			The endpoint
 * @items:		The kdbus items containing policy information
 * @items_size:		The total length of the items
 *
 * Return: 0 on success, negative errno on failure.
 */
int kdbus_ep_policy_set(struct kdbus_ep *ep,
			const struct kdbus_item *items,
			size_t items_size)
{
	return kdbus_policy_set(&ep->policy_db, items, items_size, 0, true, ep);
}

/**
 * kdbus_ep_policy_check_see_access_unlocked() - verify a connection can see
 *						 the passed name
 * @ep:			Endpoint to operate on
 * @conn:		Connection that lists names
 * @name:		Name that is tried to be listed
 *
 * This verifies that @conn is allowed to see the well-known name @name via the
 * endpoint @ep.
 *
 * Return: 0 if allowed, negative error code if not.
 */
int kdbus_ep_policy_check_see_access_unlocked(struct kdbus_ep *ep,
					      struct kdbus_conn *conn,
					      const char *name)
{
	int ret;

	/*
	 * Check policy, if the endpoint of the connection has a db.
	 * Note that policy DBs instanciated along with connections
	 * don't have SEE rules, so it's sufficient to check the
	 * endpoint's database.
	 *
	 * The lock for the policy db is held across all calls of
	 * kdbus_name_list_all(), so the entries in both writing
	 * and non-writing runs of kdbus_name_list_write() are the
	 * same.
	 */

	if (!ep->has_policy)
		return 0;

	ret = kdbus_policy_check_see_access_unlocked(&ep->policy_db,
						     conn, name);

	/* don't leak hints whether a name exists on a custom endpoint. */
	if (ret == -EPERM)
		return -ENOENT;

	return ret;
}

/**
 * kdbus_ep_policy_check_see_access() - verify a connection can see
 *					the passed name
 * @ep:			Endpoint to operate on
 * @conn:		Connection that lists names
 * @name:		Name that is tried to be listed
 *
 * This verifies that @conn is allowed to see the well-known name @name via the
 * endpoint @ep.
 *
 * Return: 0 if allowed, negative error code if not.
 */
int kdbus_ep_policy_check_see_access(struct kdbus_ep *ep,
				     struct kdbus_conn *conn,
				     const char *name)
{
	int ret;

	down_read(&ep->policy_db.entries_rwlock);
	mutex_lock(&conn->lock);

	ret = kdbus_ep_policy_check_see_access_unlocked(ep, conn, name);

	mutex_unlock(&conn->lock);
	up_read(&ep->policy_db.entries_rwlock);

	return ret;
}

/**
 * kdbus_ep_policy_check_notification() - verify a connection is allowed to see
 *					  the name in a notification
 * @ep:			Endpoint to operate on
 * @conn:		Connection connected to the endpoint
 * @kmsg:		The message carrying the notification
 *
 * This function verifies that @conn is allowed to see the well-known name
 * inside a name-change notification contained in @msg via the endpoint @ep.
 * If @msg is not a notification for name changes, this function does nothing
 * but return 0.
 *
 * Return: 0 if allowed, negative error code if not.
 */
int kdbus_ep_policy_check_notification(struct kdbus_ep *ep,
				       struct kdbus_conn *conn,
				       const struct kdbus_kmsg *kmsg)
{
	int ret = 0;

	if (kmsg->msg.src_id != KDBUS_SRC_ID_KERNEL || !ep->has_policy)
		return 0;

	switch (kmsg->notify_type) {
	case KDBUS_ITEM_NAME_ADD:
	case KDBUS_ITEM_NAME_REMOVE:
	case KDBUS_ITEM_NAME_CHANGE:
		ret = kdbus_ep_policy_check_see_access(ep, conn,
						       kmsg->notify_name);
		break;
	default:
		break;
	}

	return ret;
}

/**
 * kdbus_ep_policy_check_src_names() - check whether a connection's endpoint
 *				       is allowed to see any of another
 *				       connection's currently owned names
 * @ep:			Endpoint to operate on
 * @conn_src:		Connection that owns the names
 * @conn_dst:		Destination connection to check credentials against
 *
 * This function checks whether @ep is allowed to see any of the names
 * currently owned by @conn_src.
 *
 * Return: 0 if allowed, negative error code if not.
 */
int kdbus_ep_policy_check_src_names(struct kdbus_ep *ep,
				    struct kdbus_conn *conn_src,
				    struct kdbus_conn *conn_dst)
{
	struct kdbus_name_entry *e;
	int ret = -ENOENT;

	if (!ep->has_policy)
		return 0;

	down_read(&ep->policy_db.entries_rwlock);
	mutex_lock(&conn_src->lock);

	list_for_each_entry(e, &conn_src->names_list, conn_entry) {
		ret = kdbus_ep_policy_check_see_access_unlocked(ep, conn_dst,
								e->name);
		if (ret == 0)
			break;
	}

	mutex_unlock(&conn_src->lock);
	up_read(&ep->policy_db.entries_rwlock);

	return ret;
}

static int
kdbus_custom_ep_check_talk_access(struct kdbus_ep *ep,
				  struct kdbus_conn *conn_src,
				  struct kdbus_conn *conn_dst)
{
	int ret;

	if (!ep->has_policy)
		return 0;

	/* Custom endpoints have stricter policies */
	ret = kdbus_policy_check_talk_access(&ep->policy_db,
					     conn_src, conn_dst);

	/*
	 * Don't leak hints whether a name exists on a custom
	 * endpoint.
	 */
	if (ret == -EPERM)
		ret = -ENOENT;

	return ret;
}

static bool
kdbus_ep_has_default_talk_access(struct kdbus_conn *conn_src,
				 struct kdbus_conn *conn_dst)
{
	if (kdbus_bus_cred_is_privileged(conn_src->bus, conn_src->cred))
		return true;

	if (uid_eq(conn_src->cred->fsuid, conn_dst->cred->uid))
		return true;

	return false;
}

/**
 * kdbus_ep_policy_check_talk_access() - verify a connection can talk to the
 *					 the passed connection
 * @ep:			Endpoint to operate on
 * @conn_src:		Connection that tries to talk
 * @conn_dst:		Connection that is talked to
 *
 * This verifies that @conn_src is allowed to talk to @conn_dst via the
 * endpoint @ep.
 *
 * Return: 0 if allowed, negative error code if not.
 */
int kdbus_ep_policy_check_talk_access(struct kdbus_ep *ep,
				      struct kdbus_conn *conn_src,
				      struct kdbus_conn *conn_dst)
{
	int ret;

	/* First check the custom endpoint with its policies */
	ret = kdbus_custom_ep_check_talk_access(ep, conn_src, conn_dst);
	if (ret < 0)
		return ret;

	/* Then check if it satisfies the implicit policies */
	if (kdbus_ep_has_default_talk_access(conn_src, conn_dst))
		return 0;

	/* Fallback to the default endpoint policy */
	ret = kdbus_policy_check_talk_access(&ep->bus->policy_db,
					     conn_src, conn_dst);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * kdbus_ep_policy_check_broadcast() - verify a connection can send
 *				       broadcast messages to the
 *				       passed connection
 * @ep:			Endpoint to operate on
 * @conn_src:		Connection that tries to talk
 * @conn_dst:		Connection that is talked to
 *
 * This verifies that @conn_src is allowed to send broadcast messages
 * to @conn_dst via the endpoint @ep.
 *
 * Return: 0 if allowed, negative error code if not.
 */
int kdbus_ep_policy_check_broadcast(struct kdbus_ep *ep,
				    struct kdbus_conn *conn_src,
				    struct kdbus_conn *conn_dst)
{
	int ret;

	/* First check the custom endpoint with its policies */
	ret = kdbus_custom_ep_check_talk_access(ep, conn_src, conn_dst);
	if (ret < 0)
		return ret;

	/* Then check if it satisfies the implicit policies */
	if (kdbus_ep_has_default_talk_access(conn_src, conn_dst))
		return 0;

	/*
	 * If conn_src owns names on the bus, and the conn_dst does
	 * not own any name, then allow conn_src to signal to
	 * conn_dst. Otherwise fallback and perform the bus policy
	 * check on conn_dst.
	 *
	 * This way we allow services to signal on the bus, and we
	 * block broadcasts directed to services that own names and
	 * do not want to receive these messages unless there is a
	 * policy entry to permit it. By this we try to follow the
	 * same logic used for unicat messages.
	 */
	if (atomic_read(&conn_src->name_count) > 0 &&
	    atomic_read(&conn_dst->name_count) == 0)
		return 0;

	/* Fallback to the default endpoint policy */
	ret = kdbus_policy_check_talk_access(&ep->bus->policy_db,
					     conn_src, conn_dst);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * kdbus_ep_policy_check_own_access() - verify a connection can own the passed
 *					name
 * @ep:			Endpoint to operate on
 * @conn:		Connection that acquires a name
 * @name:		Name that is about to be acquired
 *
 * This verifies that @conn is allowed to acquire the well-known name @name via
 * the endpoint @ep.
 *
 * Return: 0 if allowed, negative error code if not.
 */
int kdbus_ep_policy_check_own_access(struct kdbus_ep *ep,
				     const struct kdbus_conn *conn,
				     const char *name)
{
	int ret;

	if (ep->has_policy) {
		ret = kdbus_policy_check_own_access(&ep->policy_db, conn, name);
		if (ret < 0)
			return ret;
	}

	if (kdbus_bus_cred_is_privileged(conn->bus, conn->cred))
		return 0;

	ret = kdbus_policy_check_own_access(&ep->bus->policy_db, conn, name);
	if (ret < 0)
		return ret;

	return 0;
}
