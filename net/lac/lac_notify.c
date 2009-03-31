#include <linux/kernel.h>

#include "lac_private.h"
#include "lac_debug.h"

static int lac_device_event(struct notifier_block *unused, unsigned long event, void *ptr);

struct notifier_block lac_device_notifier = {
	.notifier_call = lac_device_event
};

/*
 * Handle changes in state of network devices introduced in system.
 * Device could be attacehd to a aggregator or not.
 */
static int lac_device_event(struct notifier_block *unused, unsigned long event, void *ptr)
{
	struct net_device *dev = ptr;
	Lac_port *p = dev->lac_port;

	/* not a port of a bridge */
	if (p == NULL)
		return NOTIFY_DONE;

	switch (event) 
	{
	case NETDEV_CHANGEMTU:
		/*TODO: change mtu for aggregator also, check if exists*/
		//dev_set_mtu(agg->dev, lac_min_mtu(agg));
		break;

	case NETDEV_CHANGEADDR:
		/*TODO: change address for aggregator, also, if needed*/
		break;

	case NETDEV_CHANGE:
		Lac_warn("lac_device_event: NETDEV_CHANGE\n");
			schedule_delayed_work(&p->link_check, HZ/10);
		break;

	case NETDEV_DOWN:
		Lac_warn("lac_device_event: NETDEV_DOWN\n");
		if(p->cfg_mask.port_enabled == True)
		{
			p->selected = Unselected;
			lac_disable_port(p);
			lac_mux_detached(p);
		}
		break;

	case NETDEV_UP:
		Lac_warn("lac_device_event: NETDEV_UP\n");
		if (netif_carrier_ok(dev) && dev->flags & IFF_UP) 
		{
			Lac_warn("Enable port %d (%s)\n", p->port_no, p->dev->name);
			schedule_work(&p->link_check);
		}
		break;

	case NETDEV_UNREGISTER:
		lac_del_if(dev);
		goto done;
	}

 done:
	return NOTIFY_DONE;
}
