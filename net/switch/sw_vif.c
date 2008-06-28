/*
 *    This file is part of Linux Multilayer Switch.
 *
 *    Linux Multilayer Switch is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU General Public License as published
 *    by the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    Linux Multilayer Switch is distributed in the hope that it will be 
 *    useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Linux Multilayer Switch; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sw_private.h"

struct net_device *sw_vif_find(struct net_switch *sw, int vlan) {
	struct net_switch_vif_priv *priv;
	struct list_head *search = &sw->vif[sw_vlan_hash(vlan)];

	list_for_each_entry(priv, search, lh) {
		if(priv->bogo_port.vlan == vlan)
			return priv->bogo_port.dev;
	}
	return NULL;
}

int sw_vif_open(struct net_device *dev) {
	netif_start_queue(dev);
	return 0;
}

int sw_vif_stop(struct net_device *dev) {
	netif_stop_queue(dev);
	return 0;
}

int sw_vif_hard_start_xmit(struct sk_buff *skb, struct net_device *dev) {
	struct net_switch_vif_priv *priv = netdev_priv(dev);
	struct skb_extra skb_e;
	unsigned long pkt_len = skb->data_len;

	skb_e.vlan = priv->bogo_port.vlan;
	skb_e.has_vlan_tag = 0;
	skb->mac.raw = skb->data;
	skb->mac_len = ETH_HLEN;
	skb->dev = dev;
	skb_pull(skb, ETH_HLEN);
	dbg("sw_vif_hard_xmit: skb=0x%p skb headroom: %d (head=0x%p data=0x%p)\n",
			skb, skb->data - skb->head, skb->head, skb->data);
	if(sw_forward(&priv->bogo_port, skb, &skb_e)) {
		priv->stats.tx_packets++;
		priv->stats.tx_bytes += pkt_len;
	} else {
		priv->stats.tx_errors++;
	}
	return 0;
}

void sw_vif_tx_timeout(struct net_device *dev) {
}

struct net_device_stats * sw_vif_get_stats(struct net_device *dev) {
	struct net_switch_vif_priv *priv = netdev_priv(dev);
	return &priv->stats;
}

int sw_vif_set_config(struct net_device *dev, struct ifmap *map) {
	return 0;
}

int sw_vif_do_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd) {
	return 0;
}

int sw_vif_addif(struct net_switch *sw, int vlan) {
	char buf[9];
	struct net_device *dev;
	struct net_switch_vif_priv *priv;
	int result;
	
	if(sw_invalid_vlan(vlan))
		return -EINVAL;
	if(sw_vif_find(sw, vlan))
		return -EEXIST;
	/* We can now safely create the new interface and this is no race
	   because this is called only from ioctl() and ioctls are
	   mutually exclusive (a semaphore in socket ioctl routine)
	 */
	sprintf(buf, "vlan%d", vlan);
	dbg("About to alloc netdev for vlan %d\n", vlan);
	dev = alloc_netdev(sizeof(struct net_switch_vif_priv), buf, ether_setup);
	if(dev == NULL)
		return -EINVAL;
	memcpy(dev->dev_addr, sw->vif_mac, ETH_ALEN);
	dev->dev_addr[ETH_ALEN - 2] ^= vlan / 0x100;
	dev->dev_addr[ETH_ALEN - 1] ^= vlan % 0x100;

	dev->open = sw_vif_open;
	dev->stop = sw_vif_stop;
	dev->set_config = sw_vif_set_config;
	dev->hard_start_xmit = sw_vif_hard_start_xmit;
	dev->do_ioctl = sw_vif_do_ioctl;
	dev->get_stats = sw_vif_get_stats;
	dev->tx_timeout = sw_vif_tx_timeout;
	dev->watchdog_timeo = HZ;
	
	priv = netdev_priv(dev);
	INIT_LIST_HEAD(&priv->bogo_port.lh); /* paranoid */
	priv->bogo_port.dev = dev;
	priv->bogo_port.sw = sw;
	priv->bogo_port.flags = 0;
	priv->bogo_port.vlan = vlan;
	priv->bogo_port.forbidden_vlans = NULL;
	priv->bogo_port.desc[0] = '\0';
	list_add_tail(&priv->lh, &sw->vif[sw_vlan_hash(vlan)]);
	if ((result = register_netdev(dev))) {
		dbg("vif: error %i registering netdevice %s\n", 
				result, dev->name);
	}
	else {
		dbg("vif: successfully registered netdevice %s\n", dev->name);
	}		
	if(sw_vdb_add_vlan_default(sw, vlan))
		sw_vdb_add_port(vlan, &priv->bogo_port);
	
	return 0;
}

static void __vif_delif(struct net_device *dev) {
	struct net_switch_vif_priv *priv;

	priv = netdev_priv(dev);
	list_del_rcu(&priv->lh);
	sw_vdb_del_port(priv->bogo_port.vlan, &priv->bogo_port);
	synchronize_sched();
	unregister_netdev(dev);
	free_netdev(dev);
}

int sw_vif_delif(struct net_switch *sw, int vlan) {
	struct net_device *dev;

	dbg("sw_vif_delif called (vlan=%d).\n", vlan);
	if(sw_invalid_vlan(vlan))
		return -EINVAL;
	if((dev = sw_vif_find(sw, vlan)) == NULL)
		return -ENOENT;

	__vif_delif(dev);
	return 0;
}

/* Administratively enable the virtual interface */
/* FIXME: bogo_port flags ? */
int sw_vif_enable(struct net_switch *sw, int vlan) {
	struct net_device *dev;
	
	if (sw_invalid_vlan(vlan))
		return -EINVAL;
	if ((dev = sw_vif_find(sw, vlan)) == NULL)
		return -ENOENT;
	
	sw_device_up(dev);
	return 0;
}

/* Administratively disable the virtual interface */
/* FIXME: bogo_port flags ? */
int sw_vif_disable(struct net_switch *sw, int vlan) {
	struct net_device *dev;
	
	if (sw_invalid_vlan(vlan))
		return -EINVAL;
	if ((dev = sw_vif_find(sw, vlan)) == NULL)
		return -ENOENT;

	sw_device_down(dev);
	return 0;
}

void sw_vif_cleanup(struct net_switch *sw) {
	struct net_switch_vif_priv *priv, *tmp;
	int i;
	
	for (i=0; i < SW_VIF_HASH_SIZE; i++)
		list_for_each_entry_safe(priv, tmp, &sw->vif[i], lh)
			__vif_delif(priv->bogo_port.dev);
}
