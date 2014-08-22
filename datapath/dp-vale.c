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

#ifdef DEV_NETMAP
#include "dp-vale.h"

/*
 * We cannot simply register OVS datapath to the VALE switch
 * at datapath initialization, because it requires at least
 * one port. We thus do this when the first port is attached.
 */
static int vale_dp_initialized = 0;

/**
 * Equivalent of netdev_frame_hook().
 * XXX skb is always private, so we use _skb_refdst to store the destination
 */
static unsigned int ovs_vale_lookup(struct nm_bdg_fwd *ft, u8 *dst_ring,
#ifdef NETMAP_API_4
		const struct netmap_adapter *na)
#else
		const struct netmap_vp_adapter *na)
#endif
{
#ifdef NETMAP_API_4
	struct net_device *dev = na->ifp;
#else
	struct net_device *dev;
#endif
	struct vport *vport;
	struct sk_buff *skb;
	struct sk_buff sskb;
	struct ovs_vale_dst dst = {1, NM_BDG_NOPORT};
	u_int shlen = SKB_DATA_ALIGN( sizeof(struct skb_shared_info) -
		sizeof(skb_frag_t) * MAX_SKB_FRAGS ); /* XXX no multi frags */
	u_int buf_len = ft->ft_len;
	uint8_t *buf = ft->ft_buf;

#ifndef NETMAP_API_4
	dev = netmap_vp_to_ifp(na);
        if (buf_len == na->virt_hdr_len) {
                ft++;
                buf = ft->ft_buf;
                buf_len = ft->ft_len;
        } else {
                buf += na->virt_hdr_len;
                buf_len -= na->virt_hdr_len;
        }
#endif
	if (ovs_is_internal_dev(dev))
		vport = ovs_internal_dev_get_vport(dev);
	else
		vport = ovs_netdev_get_vport(dev);
	if (unlikely(!vport))
		return NM_BDG_NOPORT;

	/* We use the last bytes of buf for skb_shared_info with no
	 * frags.
	 * So, make sure that there is no other fragments and GSO
	 * processing is not applied.
	 */
	bzero(&sskb, sizeof(sskb));
	sskb.dev = dev;
	sskb.len += buf_len;
#ifdef NETMAP_API_4
	sskb.truesize = NETMAP_BUF_SIZE - shlen;
#else
	sskb.truesize = NETMAP_BUF_SIZE(&na->up) - shlen;
#endif
	sskb.head_frag = 1;
	sskb.data = sskb.head = (unsigned char *)buf;
	skb_reset_tail_pointer(&sskb);
	/* Ensure sskb.end points shinfo */
#ifdef NETMAP_API_4
	sskb.end = sskb.tail + NETMAP_BUF_SIZE - shlen;
#else
	sskb.end = sskb.tail + NETMAP_BUF_SIZE(&na->up) - shlen;
#endif
	sskb.mac_header = (typeof(sskb.mac_header))~0U;
	sskb.transport_header = (typeof(skb->transport_header))~0U;

	sskb.protocol = eth_type_trans(&sskb, dev);
	atomic_set(&sskb.users, 2); /* not to be real-freed */

	bzero(skb_shinfo(&sskb), shlen);
	atomic_set(&skb_shinfo(&sskb)->dataref, 1);
	skb = &sskb;

	OVCB_SET(skb, &dst);

	skb_push(skb, ETH_HLEN);
	ovs_skb_postpush_rcsum(skb, skb->data, ETH_HLEN);
	rcu_read_lock();
	ovs_vport_receive(vport, skb, NULL /* no tunnel key */);
	rcu_read_unlock();
	OVCB_SET(skb, NULL);
	*dst_ring = 0;
	return dst.port;
}

#define OVS_VALE_PREFIX "vale0:"

static struct netmap_bdg_ops ovs_vale_ops = {
	.lookup = ovs_vale_lookup,
	.config = NULL,
	.dtor = NULL,
};


/*
 * we pass indication of an internal device as argument because
 * is_internal_dev() does not work until the datapath is fully
 * initialized
 */
int ovs_vale_ctl(const char *name, int internal, int onoff)
{
	struct nmreq nmr;
	int error;

	memset(&nmr, 0, sizeof(nmr));

	/* create request string */
	strncpy(nmr.nr_name, OVS_VALE_PREFIX, sizeof(nmr.nr_name));
	if (strlen(nmr.nr_name) + strlen(name) + 1 >= IFNAMSIZ)
		return EINVAL;
	strcat(nmr.nr_name, name);

	nmr.nr_cmd = onoff ? NETMAP_BDG_ATTACH : NETMAP_BDG_DETACH;
	if (onoff && internal)
		nmr.nr_arg1 = NETMAP_BDG_HOST;
	error = netmap_bdg_ctl(&nmr, NULL);
	if (error) {
		D("failed to %s %s (internal:%d)",
		    onoff?"attach":"detach", name, internal);
		return error;
	}

	if (onoff && !vale_dp_initialized) {
		/* reuse nr_name */
		nmr.nr_cmd = NETMAP_BDG_REGOPS;
		error = netmap_bdg_ctl(&nmr, &ovs_vale_ops);
		if (error) {
			nmr.nr_cmd = NETMAP_BDG_DETACH;
			if (netmap_bdg_ctl(&nmr, NULL)) /* XXX handle error... */
				D("error on BDG_DETACH after BDG_REGOPS failure");
		} else {
			vale_dp_initialized = 1;
			D("datapath is registered to %s", OVS_VALE_PREFIX);
		}

	} else if (!onoff && vale_dp_initialized) { /* last interface? */
		nmr.nr_cmd = NETMAP_BDG_LIST;
		memset(nmr.nr_name, 0, sizeof(nmr.nr_name));
		strncpy(nmr.nr_name, OVS_VALE_PREFIX, sizeof(nmr.nr_name));
		if (netmap_bdg_ctl(&nmr, NULL) == ENOENT) {
			vale_dp_initialized = 0;
			D("%s is destroyed", OVS_VALE_PREFIX);
		}
	}
	return error;
}

int ovs_vale_send(struct net_device *dev, struct sk_buff *skb)
{
	struct ovs_vale_dst *dst = OVCB(skb)->dst;
#ifndef NETMAP_API_4
	struct netmap_vp_adapter *vpna;
#endif
	u_int dport;

	if (!skb_owned_by_vale(skb))
		return 0;
#ifdef NETMAP_API_4
	if (unlikely(!(dev->priv_flags & IFCAP_NETMAP))) { /* e.g., VALE port */
#else
	if (netmap_ifp_to_vp(dev) == NULL) { /* Not a VALE port */
#endif
		dst->port = NM_BDG_NOPORT;
		goto consumed;
	}
	/* set the destination index of the bridge.
	 * XXX If it is already set, we broadcast this packet
	 */
#ifdef NETMAP_API_4
	dport = ovs_is_internal_dev(dev) ? SWNA(dev)->bdg_port :
					NA(dev)->bdg_port;
#else
	vpna = ovs_is_internal_dev(dev) ? netmap_ifp_to_host_vp(dev) :
		netmap_ifp_to_vp(dev);
	if (unlikely(!vpna)) {
		D("%s is not attached to the bridge", dev->name);
		return 0;
	}
	dport = vpna->bdg_port;
#endif
	if (dst->port == NM_BDG_NOPORT)
		dst->port = dport;
	else if (likely(dst->port != dport))
		dst->port = NM_BDG_BROADCAST;
	/* destroyed if this is cloned in action's loop, otherwise
	 * drops the extra refcount
	 */
consumed:
	kfree_skb(skb);
	return 1;
}
#endif /* DEV_NETMAP */
