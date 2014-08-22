#include <net/netmap.h>
#include <netmap/netmap_kern.h>

#define SOFTC_T internal_dev

#define VPORT_INTERNAL_TX_DESC 128
#define VPORT_INTERNAL_RX_DESC 128

static struct internal_dev *internal_dev_priv(struct net_device *netdev);

static int
#ifdef NETMAP_API_4
vport_internal_netmap_reg(struct net_device *dev, int onoff)
#else
vport_internal_netmap_reg(struct netmap_adapter *na, int onoff)
#endif
{
#ifdef NETMAP_API_4
	struct netmap_adapter *na = NA(dev);
#else
	struct net_device *dev = na->ifp;
#endif
	int error = 0;

	if (!(dev)->flags & IFF_UP) {
		D("Interface is down!");
		return EINVAL;
	}
	if (na == NULL)
		return EINVAL;
	if (onoff) {
#ifdef NETMAP_API_4
		dev->priv_flags |= IFCAP_NETMAP;
		na->if_transmit = (void *)dev->netdev_ops;
		dev->netdev_ops = &na->nm_ndo;
#else
		nm_set_native_flags(na);
#endif
		/* XXX do we need to store dev->features ? */
	} else {
#ifdef NETMAP_API_4
		dev->priv_flags &= ~IFCAP_NETMAP;
		dev->netdev_ops = (void *)na->if_transmit;
#else
		nm_clear_native_flags(na);
#endif
	}
	return error;
}
static struct rtnl_link_stats64 *internal_dev_get_stats(struct net_device *,
							struct rtnl_link_stats64 *);
static int internal_dev_change_mtu(struct net_device *, int);

#ifndef NETMAP_API_4
static int
vport_internal_update_config(struct netmap_adapter *na, u_int *txr, u_int *txd, u_int *rxr, u_int *rxd)
{
	*txr = na->num_tx_rings;
	*txd = na->num_tx_desc;
	*rxr = na->num_rx_rings;
	*rxd = na->num_rx_desc;
	return 0;
}
#endif /* NETMAP_API_4 */

static struct device_driver dummy = {.owner = THIS_MODULE};
static void
vport_internal_netmap_attach(struct SOFTC_T *sc)
{
	struct netmap_adapter na;

	bzero(&na, sizeof(na));

	na.ifp = netdev_vport_priv((const struct vport *)sc->vport)->dev;
	na.ifp->dev.driver = &dummy;
	na.num_tx_desc = VPORT_INTERNAL_TX_DESC;
	na.num_rx_desc = VPORT_INTERNAL_RX_DESC;
	na.nm_txsync = na.nm_rxsync = NULL;
	na.nm_register = vport_internal_netmap_reg;
	na.na_flags |= NAF_SW_ONLY;
#ifdef NETMAP_API_4
	netmap_attach(&na, 4);
	/* indicator of internal_dev */
	NA(na.ifp)->nm_ndo.ndo_get_stats64 = internal_dev_get_stats;
#else
	na.num_tx_rings = na.num_rx_rings = 1;
	na.nm_config = vport_internal_update_config;
	netmap_attach(&na);
	((struct netmap_hw_adapter *)NA(na.ifp))->nm_ndo.ndo_get_stats64 =
		internal_dev_get_stats;
#endif
	/* do we need these ?*/
#ifdef NETMAP_API_4
	NA(na.ifp)->nm_ndo.ndo_change_mtu = internal_dev_change_mtu;
	NA(na.ifp)->nm_ndo.ndo_set_mac_address = eth_mac_addr;
#endif
}
