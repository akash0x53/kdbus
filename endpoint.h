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

#ifndef __KDBUS_ENDPOINT_H
#define __KDBUS_ENDPOINT_H

#include <linux/device.h>
#include "limits.h"
#include "names.h"
#include "policy.h"
#include "util.h"

/*
 * struct kdbus_endpoint - enpoint to access a bus
 * @dev:		Device
 * @bus:		Bus behind this endpoint
 * @name:		Name of the endpoint
 * @id:			ID of this endpoint on the bus
 * @mode:		File mode of this endpoint device node
 * @uid:		UID owning this endpoint
 * @gid:		GID owning this endpoint
 * @conn_list:		Connections of this endpoint
 * @bus_entry:		bus' endpoints
 * @lock:		Endpoint data lock
 * @user:		Custom enpoints account against an anonymous user
 * @policy_db:		Uploaded policy
 * @disconnected:	Invalidated data
 * @has_policy:		The policy-db is valid and should be used
 *
 * An enpoint offers access to a bus; the default device node name is "bus".
 * Additional custom endpoints to the same bus can be created and they can
 * carry their own policies/filters.
 */
struct kdbus_ep {
	struct device dev;
	struct kdbus_bus *bus;
	const char *name;
	u64 id;
	umode_t mode;
	kuid_t uid;
	kgid_t gid;
	struct list_head conn_list;
	struct list_head bus_entry;
	struct mutex lock;
	struct kdbus_domain_user *user;
	struct kdbus_policy_db policy_db;

	bool disconnected : 1;
	bool has_policy : 1;
};

struct kdbus_ep *kdbus_ep_new(struct kdbus_bus *bus, const char *name,
			      umode_t mode, kuid_t uid, kgid_t gid,
			      bool policy);
struct kdbus_ep *kdbus_ep_ref(struct kdbus_ep *ep);
struct kdbus_ep *kdbus_ep_unref(struct kdbus_ep *ep);
void kdbus_ep_disconnect(struct kdbus_ep *ep);
int kdbus_ep_policy_set(struct kdbus_ep *ep,
			const struct kdbus_item *items,
			size_t items_size);

int kdbus_ep_policy_check_see_access_unlocked(struct kdbus_ep *ep,
					      struct kdbus_conn *conn,
					      const char *name);
int kdbus_ep_policy_check_see_access(struct kdbus_ep *ep,
				     struct kdbus_conn *conn,
				     const char *name);
int kdbus_ep_policy_check_notification(struct kdbus_ep *ep,
				       struct kdbus_conn *conn,
				       const struct kdbus_kmsg *kmsg);
int kdbus_ep_policy_check_src_names(struct kdbus_ep *ep,
				    struct kdbus_conn *conn_src,
				    struct kdbus_conn *conn_dst);
int kdbus_ep_policy_check_talk_access(struct kdbus_ep *ep,
				      struct kdbus_conn *conn_src,
				      struct kdbus_conn *conn_dst);
int kdbus_ep_policy_check_broadcast(struct kdbus_ep *ep,
				    struct kdbus_conn *conn_src,
				    struct kdbus_conn *conn_dst);
int kdbus_ep_policy_check_own_access(struct kdbus_ep *ep,
				     const struct kdbus_conn *conn,
				     const char *name);

#endif
