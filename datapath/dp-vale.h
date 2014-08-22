/*
 * Copyright (c) 2014 NEC Europe Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#include "datapath.h"
#include "vport-netdev.h"
#include "vport-internal_dev.h"
#include <bsd_glue.h>
#include <net/netmap.h>
#include <netmap/netmap_kern.h>

struct ovs_vale_dst {
	unsigned char active;
	unsigned char port;
};
struct ovs_vale_cb {
	struct ovs_vale_dst *dst;
};
#define OVCB(skb)	((struct ovs_vale_cb *)(((skb)->cb + sizeof(struct ovs_skb_cb))))
#define OVCB_SET(skb, p)	OVCB((skb))->dst = (p)

static inline int skb_owned_by_vale(const struct sk_buff *skb)
{
	return OVCB(skb)->dst && OVCB(skb)->dst->active;
}

static inline struct sk_buff *convert_to_real(struct sk_buff *fake)
{
	struct sk_buff *skb = skb_copy(fake, GFP_ATOMIC);

	return skb;
}

/* This is stupid as this should be done using ovs_dp_name().
 * However, it only works after completion of initiation of the datapath...
 */
/*
static inline const char *ovs_vale_prefix(const struct vport *vport)
{
	if (vport->port_no == OVSP_LOCAL)
		return vport->ops->get_name(vport);
	else
		return ovs_dp_name(vport->dp);
}
*/

static inline int vale_prefix(const char *name)
{
	return !strncmp(NM_NAME, name, strlen(NM_NAME));
}

int ovs_vale_ctl(const char *, int, int);
int ovs_vale_send(struct net_device *, struct sk_buff *skb);
