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

#ifndef __KDBUS_METADATA_H
#define __KDBUS_METADATA_H

/**
 * struct kdbus_meta - metadata buffer
 * @attached:		Flags for already attached data
 * @domain:		Domain the metadata belongs to
 * @data:		Allocated buffer
 * @size:		Number of bytes used
 * @allocated_size:	Size of buffer
 *
 * Used to collect and store connection metadata in a pre-compiled
 * buffer containing struct kdbus_item.
 */
struct kdbus_meta {
	u64 attached;
	struct pid_namespace *pid_namespace;
	struct user_namespace *user_namespace;
	struct kdbus_item *data;
	size_t size;
	size_t allocated_size;
};

struct kdbus_conn;

struct kdbus_meta *kdbus_meta_new(void);
struct kdbus_meta *kdbus_meta_dup(const struct kdbus_meta *orig);
int kdbus_meta_append_data(struct kdbus_meta *meta, u64 type,
			   const void *buf, size_t len);
int kdbus_meta_append(struct kdbus_meta *meta,
		      struct kdbus_conn *conn,
		      u64 seq,
		      u64 which);
void kdbus_meta_free(struct kdbus_meta *meta);
bool kdbus_meta_ns_eq(const struct kdbus_meta *meta_a,
		      const struct kdbus_meta *meta_b);
#endif
