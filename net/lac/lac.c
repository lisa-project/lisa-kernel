#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/if_ether.h>
#include <asm/semaphore.h>
#include <linux/timer.h>

#include "lac_private.h"
#include "../switch/sw_private.h"
#include "lac_debug.h"

int max_aggrs = 1;
module_param(max_aggrs, int, 1);
MODULE_PARM_DESC(max_aggrs, "Maximum aggregators");

MODULE_DESCRIPTION("Link Aggregation");
MODULE_AUTHOR("us");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

/* declared in dev.c */
extern int (*lac_handle_frame_hook)(struct net_lac_port *p, struct sk_buff **pskb);
extern int (*lac_dev_ioctl_hook)(struct net_device *dev, lac_cmd_t user_cmd,char *if_name,int command);

/* Module initialization */
static int lac_module_init(void) 
{
	int i;
	char aggr_name[8];
	struct net_device *dev;//ADI

	lac_dev_ioctl_hook=lac_dev_ioctl;//ADI

	lac_handle_frame_hook = lac_handle_frame;//ADI

	register_netdevice_notifier(&lac_device_notifier);

	/* init system timer */
	init_timer(&sys_timer);
	sys_timer.expires = jiffies + HZ; /* 1000 msec */
	sys_timer.function = lac_global_tick;
	add_timer(&sys_timer);

	/* system ports */
	lac_ports = NULL;

	/* aggrs list */
	INIT_LIST_HEAD(&lac_aggrs_list);
	
	for(i = 0; i < max_aggrs; i++)
	{
		sprintf(aggr_name, "aggr%d", i);
		lac_add_aggr(aggr_name);
	}
	
	lac_set_default_sys_info();

	/* aggregators are created through ioctl calls, also */

	/* init kernel thread */
	thread_stop = 0;
	sema_init(&thread_sem, 0);
	init_completion(&thread_comp);
	kernel_thread(thread_monitor_links, NULL, 0);
	
	//ADI adaug la switch pe aggr0
	if((dev = dev_get_by_name(&init_net, "aggr0")) != NULL)//ADI
	{
		//deci sa initializat aggr0
		int err;
		err=sw_addif(dev);
		if(err!=0)
		{
			Lac_printd("Could not add default agg to switch\n");
		}
	}
	return 0;
}

/* Module cleanup */
static void lac_module_exit(void) 
{
	unregister_netdevice_notifier(&lac_device_notifier);
	lac_dev_ioctl_hook=NULL;//ADI
	/* remove global timer */
	del_timer(&sys_timer);
	//lac_handle_frame_hook = NULL;
	
	/* destroy aggregators */
	lac_free_aggrs();
	/* destroy unattached ports*/
	lac_free_ports();

	/* terminate thread monitor links*/
	thread_stop = 1;
	up(&thread_sem);
	wait_for_completion(&thread_comp);

	printk(KERN_INFO "Link Aggregation module unloaded\n");
}

module_init(lac_module_init);
module_exit(lac_module_exit);
