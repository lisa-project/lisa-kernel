#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/skbuff.h>

#include <linux/net_lac.h>
#include "lac_private.h"
#include "lac_debug.h"

static struct net_device_stats *lac_dev_get_stats(struct net_device *dev)
{
	struct net_lac_aggr *agg = netdev_priv(dev);
	return &agg->statistics;
}

static void __lac_deliver(const struct net_lac_port *to, struct sk_buff *skb)
{
	skb->dev = to->dev;
	if (skb->len > skb->dev->mtu)
	{
		kfree_skb(skb);
		return;
	}
	
	skb_push(skb, ETH_HLEN);
	dev_queue_xmit(skb);
}

void lac_deliver(const struct net_lac_port *to, struct sk_buff *skb)
{
	if(skb->dev == to->dev || to->mux_state != Mux_Distributing )
	{
		kfree_skb(skb);
		return;
	}

	__lac_deliver(to, skb);
}

int lac_frame_distribution(struct sk_buff *skb, struct net_device *dev)
{
	Lac_aggr *agg = netdev_priv(dev);
	Lac_port *p;
	unsigned char *mac_h;

	agg->statistics.tx_packets++;
	agg->statistics.tx_bytes += skb->len;
	
	skb->mac.raw = skb->data;
	skb_pull(skb, ETH_HLEN);

	rcu_read_lock();
	
	if (agg->nports == 0)/* if aggregator has no ports */
		goto out;
	else if (agg->nports == 1)
	{
		p = agg->ready_ports[0];
	}
	else
	{
		mac_h = skb->data;
		p = agg->ready_ports[lac_hash_tcpudp(mac_h, skb->len, agg)];
	}

	if(!p)
		goto out;

	Lac_warn("\nPort %d chosen for distributing\n\n", p->port_no);
	
	/* livrare la domiciliu */
	lac_deliver(p, skb);

	rcu_read_unlock();
	return 0;

out:
	kfree_skb(skb);
	return 0;
}

/*deschide interfatza virtuala - reprezinta un agregator*/
static int lac_dev_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int lac_dev_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static int lac_change_mtu(struct net_device *dev, int new_mtu)
{
	return 0;
}

/**
 * Set aggregator's mac address to be the same as the port with lowest address
 */
int lac_set_min_mac_address(struct net_device *dev)
{
	Lac_aggr *agg = netdev_priv(dev);
	Lac_port *p;
	int err = -EADDRNOTAVAIL;
	
	spin_lock_bh(&agg->lock);
	
	if(agg->link_count == 0)
	{
		memset(dev->dev_addr, 0, ETH_ALEN);
		spin_unlock_bh(&agg->lock);
		return err;
	}

	list_for_each_entry(p, &agg->port_list, list)
	{
		memcpy(dev->dev_addr, p->dev->dev_addr, ETH_ALEN);
		err = 0;
		break;
	}
	if(agg->link_count > 1)
	{
		list_for_each_entry(p, &agg->port_list, list)
		{
			if(memcmp(dev->dev_addr, p->dev->dev_addr, ETH_ALEN) > 0)
				memcpy(dev->dev_addr, p->dev->dev_addr, ETH_ALEN);
		}
	}
	Lac_warn("Set MAC addresss for agg %d: 0x%02x-%02x-%02x-%02x-%02x-%02x\n", 
			agg->aggr_no, 
			dev->dev_addr[0],
			dev->dev_addr[1],
			dev->dev_addr[2],
			dev->dev_addr[3],
			dev->dev_addr[4],
			dev->dev_addr[5]);

	spin_unlock_bh(&agg->lock);
	return err;
}

/**
 * Set aggregator's mac address to be the same as any of the bound interfaces
 */
static int lac_set_mac_address(struct net_device *dev, void *a)
{
	Lac_aggr *agg = netdev_priv(dev);
	Lac_port *p;
	struct sockaddr *addr = a;
	int err = -EADDRNOTAVAIL;

	spin_lock_bh(&agg->lock);

	list_for_each_entry(p, &agg->port_list, list)
	{
		if(!memcmp(p->dev->dev_addr, addr->sa_data, ETH_ALEN))
		{
			memcpy(agg->dev->dev_addr, addr->sa_data, ETH_ALEN);
			
			Lac_warn("Set MAC addresss for agg %d: 0x%02x-%02x-%02x-%02x-%02x-%02x\n", 
			agg->aggr_no, 
			dev->dev_addr[0],
			dev->dev_addr[1],
			dev->dev_addr[2],
			dev->dev_addr[3],
			dev->dev_addr[4],
			dev->dev_addr[5]);

			err = 0;
			break;
		}

	}
	spin_unlock_bh(&agg->lock);
	return err;
}

static void lac_getinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	strcpy(info->driver, "Linux link Aggregation");
	strcpy(info->version, LA_VERSION);
	strcpy(info->fw_version, "N/A");
	strcpy(info->bus_info, "N/A");
}

static struct ethtool_ops lac_ethtool_ops = {
	.get_drvinfo = lac_getinfo,
	.get_link = ethtool_op_get_link,
	.get_sg = ethtool_op_get_sg,
	.get_tx_csum = ethtool_op_get_tx_csum,
	.get_tso = ethtool_op_get_tso,
};

void lac_dev_setup(struct net_device *dev)
{
	memset(dev->dev_addr, 0, ETH_ALEN);

	ether_setup(dev);

	//dev->do_ioctl = lac_dev_ioctl;
	dev->get_stats = lac_dev_get_stats;
	dev->hard_start_xmit = lac_frame_distribution;
	dev->open = lac_dev_open;
	dev->change_mtu = lac_change_mtu;
	dev->destructor = free_netdev;
	SET_MODULE_OWNER(dev);
 	SET_ETHTOOL_OPS(dev, &lac_ethtool_ops);
	dev->stop = lac_dev_stop;
	dev->tx_queue_len = 0;
	dev->set_mac_address = lac_set_mac_address;
	
	/*defined in if.c: Linux Link Aggregation*/
	dev->priv_flags = IFF_ELLAG;

 	dev->features = NETIF_F_SG | NETIF_F_FRAGLIST
 		| NETIF_F_HIGHDMA | NETIF_F_TSO | NETIF_F_IP_CSUM;
}
