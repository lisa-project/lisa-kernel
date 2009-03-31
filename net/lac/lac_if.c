#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rtnetlink.h>
#include <linux/if_ether.h>
#include <net/sock.h>

#include <linux/net_lac.h>
#include "lac_private.h"
#include "lac_debug.h"

void del_nlp(struct net_lac_port *p);

/*
 * Need to simulate user ioctl because not all device's that support
 * ethtool, use ethtool_ops.  Also, since driver might sleep need to
 * not be holding any locks.
 */
Boolean lac_get_phy_info(struct net_device *dev, int *port_link, int *port_duplx, int *port_speed)
{
	struct ethtool_cmd ecmd = { ETHTOOL_GSET };
	struct ifreq ifr;
	mm_segment_t old_fs;
	int err;

	strncpy(ifr.ifr_name, dev->name, IFNAMSIZ);
	ifr.ifr_data = (void __user *) &ecmd;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = dev_ethtool(&ifr);
	set_fs(old_fs);

	*port_speed = Interface_100Mb;
	*port_duplx = Interface_full_dup;
	
	if((dev->flags & IFF_UP) && netif_carrier_ok(dev))
	{
		*port_link = Interface_link_on;
	}
	else /* link down */
	{
		Lac_warn("Interface %s is down\n", dev->name);
		*port_link = Interface_link_off;
	}

	if (!err) 
	{
		switch(ecmd.speed) 
		{
		case SPEED_10:
			*port_speed = Interface_10Mb;
			break;
		case SPEED_100:
			*port_speed = Interface_100Mb;
			break;
		case SPEED_1000:
			*port_speed = Interface_1000Mb;
			break;
		case SPEED_10000:
			*port_speed = Interface_10000Mb;
			break;
		}
		switch(ecmd.duplex) 
		{
		case DUPLEX_HALF:
			*port_speed = Interface_half_dup;
			break;
		case DUPLEX_FULL:
			*port_duplx = Interface_full_dup;
			break;
		}
	}

	return True;
}

/***********************************aggregators management*********************************/

/* find an available aggregator number */
static int find_aggno(void)
{
	int index;
	Lac_aggr *a;
	unsigned long *inuse;

	inuse = kmalloc(BITS_TO_LONGS(LAC_MAX_AGGRS)*sizeof(unsigned long), GFP_KERNEL);
	if (!inuse)
		return -ENOMEM;

	memset(inuse, 0, BITS_TO_LONGS(LAC_MAX_AGGRS)*sizeof(unsigned long));
	set_bit(0, inuse);	/* zero is reserved */
	list_for_each_entry(a, &lac_aggrs_list, list) 
	{
		set_bit(a->aggr_no, inuse);
	}

	index = find_first_zero_bit(inuse, LAC_MAX_AGGRS);
	kfree(inuse);

	return (index >= LAC_MAX_AGGRS) ? -EXFULL : index;
}

/* called with RTNL */
void del_aggr(struct net_lac_aggr *agg)
{
	struct net_lac_port *p, *n;
	/* remove all ports from aggregator */
	list_for_each_entry_safe(p, n, &agg->port_list, list) 
	{
		lac_del_sys_port(p);
		del_nlp(p);
	}

	/* remove from system list */
	list_del(&agg->list);

	Lac_warn("Aggregator %d removed\n", agg->aggr_no);
	unregister_netdevice(agg->dev);
}

static struct net_device *new_aggr_dev(const char *name)
{
	struct net_lac_aggr *agg;
	struct net_device *dev;

	dev = alloc_netdev(sizeof(Lac_aggr), name, lac_dev_setup);
	
	if (!dev)
		return NULL;
	
	agg = netdev_priv(dev);
	
	agg->dev = dev;
	spin_lock_init(&agg->lock);
	INIT_LIST_HEAD(&agg->list);
	INIT_LIST_HEAD(&agg->port_list);

	agg->aggr_no = find_aggno();
	return dev;
}

int lac_add_aggr(const char *name)
{
	struct net_device *dev;
	Lac_aggr *agg;
	int ret;

	dev = new_aggr_dev(name);
	if (!dev) 
		return -ENOMEM;
	
	rtnl_lock();

	Lac_warn("Adding aggregator <%s>\n", name);
	if (strchr(dev->name, '%')) {
		ret = dev_alloc_name(dev, dev->name);
		if (ret < 0)
		{
			Lac_warn("dev_alloc_name failed for '%s'\n", dev->name);
			goto err;
		}
	}

	ret = register_netdevice(dev);
	if (ret)
	{
		Lac_warn("Device not registered '%s'\n", dev->name);
		goto err;
	}

	/* network device kobject is not setup until
	 * after rtnl_unlock does it's hotplug magic.
	 * so hold reference to avoid race.
	 */
	dev_hold(dev);
	rtnl_unlock();

	dev_put(dev);
	
	/* add to system list */
	agg = netdev_priv(dev);
	list_add_tail(&agg->list, &lac_aggrs_list);
	
	return ret;

 err:
	free_netdev(dev);
	rtnl_unlock();
	return ret;
}

/* called through ioctl */
int lac_del_aggr(const char *name)
{
	struct net_device *dev;
	int ret = 0;

	rtnl_lock();
	dev = __dev_get_by_name(name);
	if (dev == NULL) 
	{
		ret =  -ENXIO; 	/* Could not find device */
	}
	else if (!(dev->priv_flags & IFF_ELLAG)) 
	{
		/* Attempt to delete non aggregation device! */
		Lac_warn("Attempt to delete non aggregation device!\n");
		ret = -EPERM;
	}
	else if (dev->flags & IFF_UP) 
	{
		Lac_warn("Device running!\n");
		ret = -EBUSY;
	} 
	else 
	{
		Lac_printi("Deleting aggregator <%s>\n", name);
		del_aggr(netdev_priv(dev));
	}
	rtnl_unlock();

	return ret;
}

#if 0
int lac_del_aggr_by_dev(struct net_device *dev)
{
	int ret = 0;

	rtnl_lock();
	
	if (!(dev->priv_flags & IFF_ELLAG)) 
	{
		/* Attempt to delete non aggregation device! */
		ret = -EPERM;
	}
	else if (dev->flags & IFF_UP) 
	{
		ret = -EBUSY;
	} 
	else 
	{
		del_aggr(netdev_priv(dev));
	}
	rtnl_unlock();

	return ret;
}
#endif

void lac_free_aggrs(void)
{
	Lac_aggr *agg, *nxt;
	rtnl_lock();
	list_for_each_entry_safe(agg, nxt, &lac_aggrs_list, list) 
	{
		if(agg->dev)
			del_aggr(agg);
	}
	rtnl_unlock();
}

void lac_free_ports(void)
{
	Lac_port *p = lac_ports;
	Lac_port *aux;

	rtnl_lock();
	while(p)
	{
		aux = p;
		p = p->pNext;
		del_nlp(aux);
	}
	rtnl_unlock();
}

/***********************************interfaces management*********************************/

/* MTU of the aggregator pseudo-device: ETH_DATA_LEN or the minimum of the ports */
int lac_min_mtu(const struct net_lac_aggr *agg)
{
	const struct net_lac_port *p;
	int mtu = 0;

	ASSERT_RTNL();

	if (list_empty(&agg->port_list))
		mtu = ETH_DATA_LEN;
	else {
		list_for_each_entry(p, &agg->port_list, list) {
			if (!mtu  || p->dev->mtu < mtu)
				mtu = p->dev->mtu;
		}
	}
	return mtu;
}
/* find an available port number */
static int find_portno(void)
{
	int index;
	struct net_lac_port *p = lac_ports;
	unsigned long *inuse;

	inuse = kmalloc(BITS_TO_LONGS(LAC_MAX_PORTS)*sizeof(unsigned long),	GFP_KERNEL);
	if (!inuse)
		return -ENOMEM;

	memset(inuse, 0, BITS_TO_LONGS(LAC_MAX_PORTS)*sizeof(unsigned long));
	set_bit(0, inuse);	/* zero is reserved */
	while(p)
	{
		set_bit(p->port_no, inuse);
		p = p->pNext;
	}
	index = find_first_zero_bit(inuse, LAC_MAX_PORTS);
	kfree(inuse);

	return (index >= LAC_MAX_PORTS) ? -EXFULL : index;
}

static int lac_find_agg_by_name(char *name)
{
	Lac_aggr *a;

	if (!name)
		return 0;

	list_for_each_entry(a, &lac_aggrs_list, list)
	{
		if(a->dev && a->dev->name && !strcmp(a->dev->name, name))
			return a->aggr_no;
	}

	return 0;
}

static int lac_find_port_by_name(char *ifname)
{
	Lac_port *p = lac_ports;

	while (p)
	{
		if (p->dev && p->dev->name && !strcmp(p->dev->name, ifname))
			return p->port_no;
		p = p->pNext;
	}
	return 0;
}

/* called with RTNL but without lac lock */
struct net_lac_port *new_nlp(struct net_lac_aggr *agg, struct net_device *dev)
{
	int index;
	struct net_lac_port *p;
	
	index = find_portno();
	if (index < 0)
		return ERR_PTR(index);

	p = kmalloc(sizeof(Lac_port), GFP_KERNEL);
	if (p == NULL)
		return ERR_PTR(-ENOMEM);

	memset(p, 0, sizeof(Lac_port));
	p->port_no = index;

	/*obtine device-ul*/
	dev_hold(dev);
	p->dev = dev;
	
	/* lacp init port */
	lac_init_default_port(p);

	/* init list */
	INIT_LIST_HEAD(&p->list);

	/* init work queue */
	INIT_WORK(&p->link_check, lac_link_check, p);
	
	return p;
}

static void destroy_nlp(Lac_port *p)
{
	struct net_device *dev = p->dev;

	p->agg = NULL;
	p->dev = NULL;
	dev_put(dev);
	kfree(p);
}

static void destroy_nlp_rcu(struct rcu_head *head)
{
	struct net_lac_port *p = container_of(head, struct net_lac_port, rcu);
	destroy_nlp(p);
}

void del_nlp(struct net_lac_port *p)
{
	struct net_device *dev = p->dev;
	Lac_aggr *agg;
	Lac_warn("del_nlp: Deleting port %d\n", p->port_no);

	lac_unregister_timers(p);
	dev_set_promiscuity(dev, -1);
	
	cancel_delayed_work(&p->link_check);

	/* delete pointer from net_dev structure */
	rcu_assign_pointer(dev->lac_port, NULL);

	/* remove from aggregator */
	agg = p->agg;
	if(agg)
	{
		/*spin_lock_bh(&p->agg->lock);*/
		agg->link_count--;
        list_del_rcu(&p->list);
		call_rcu(&p->rcu, destroy_nlp_rcu);
		lac_update_distrib_port(agg);
		/*spin_unlock_bh(&p->agg->lock);*/
	}
	else
	{
		dev_put(p->dev);
		kfree(p);
	}
}

/* called with RTNL */
int lac_add_if(struct net_lac_aggr *agg, struct net_device *dev)
{
	struct net_lac_port *p;
	
	if (dev->flags & IFF_LOOPBACK || dev->type != ARPHRD_ETHER)
		return -EINVAL;

	if (dev->hard_start_xmit == lac_frame_distribution)
	{
		Lac_warn("ERROR: %s ELOOP\n", dev->name);
		return -ELOOP;
	}

	if (dev->lac_port != NULL)
	{
		Lac_warn("ERROR: %s EBUSY\n", dev->name);
		return -EBUSY;
	}
	
	/* check if port is already in the system */
	if (lac_find_port_by_name(dev->name) || lac_find_agg_by_name(dev->name))
	{
		Lac_warn("device %s already in system\n", dev->name);
		return -EINVAL;
	}
	
	Lac_warn("creating new port in system\n");
	/* create new port in system */
	p = new_nlp(agg, dev);
	if (IS_ERR(p))
		return PTR_ERR(p);	

	rcu_assign_pointer(dev->lac_port, p);
	dev_set_promiscuity(dev, 1);
	
	/* add to system list */
	if(!lac_add_sys_port(p))
		goto error;
	
	lac_set_system_info(p);

	lac_init_port(p);

	schedule_delayed_work(&p->link_check, HZ/10);

	return 0;

error:
	kfree(p);
	return 0;
}

/* called with RTNL (from ioctl) */
int lac_del_if(struct net_device *dev)
{
	struct net_lac_port *p = dev->lac_port;
	
	if (!p) 
		return -EINVAL;

	/* lacp cleanup */
	lac_del_sys_port(p);

	del_nlp(p);

	return 0;
}
