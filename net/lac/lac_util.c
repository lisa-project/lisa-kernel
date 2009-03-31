#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/unistd.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/rtnetlink.h>

#include "lac_private.h"
#include "lac_protocol.h"
#include "lac_debug.h"

struct semaphore thread_sem;
struct completion thread_comp;
int thread_stop;

/**
*	Timers
*/
struct timer_list sys_timer;	/* system timer */
La_Timer first_timer;
La_Timer *tick_next;

void lac_start_timer(La_Timer *lac_timer, Time exp_time)
{
	if (lac_timer == NULL)
		return;

	lac_timer->exp_ticks = exp_time;
	lac_timer->ticks = 0;
	lac_timer->active = True;
}

void lac_stop_timer(La_Timer *lac_timer)
{
	if (lac_timer == NULL)
		return;

	lac_timer->exp_ticks = 0;
	lac_timer->ticks = 0;
	lac_timer->active = False;
}

void lac_tick_timer(La_Timer *lac_timer)
{
	if (lac_timer == NULL)
		return;

	if (!lac_timer->active)
		return;

	lac_timer->ticks++;
	if (lac_timer->ticks >= lac_timer->exp_ticks) 
	{
		lac_timer->ticks = 0;
		lac_timer->active = False;
	}
}

Boolean lac_is_timer_active(La_Timer *lac_timer)
{
	if (lac_timer == NULL)
		return False;

	return lac_timer->active;
}

/*
* Register timer to system list
*/
void lac_register_timer(La_Timer *lac_timer, void (*call_fn)(void *), void *with_arg)
{
	La_Timer *t;
	lac_timer->expiry_fn = call_fn;
	lac_timer->expiry_arg = with_arg;
	lac_timer->next_timer = NULL;

	for(t = &first_timer; t->next_timer != NULL; t = t->next_timer);

	t->next_timer = lac_timer;
}

/*
* Remove timer from timer system list
*/
void lac_unregister_timer(La_Timer *lac_timer)
{
	if (lac_timer == NULL)
		return;

	La_Timer *in_chain;
   
	in_chain = &first_timer;
	while (in_chain->next_timer != lac_timer)
	{
		in_chain = in_chain->next_timer;
	}

	in_chain->next_timer = in_chain->next_timer->next_timer;
}

void lac_register_timers(Lac_port *port)
{
	lac_register_timer(&port->tick_timer, lac_port_tick, port);
	lac_register_timer(&port->tx_scheduler, tx_opportunity, port);
}

void lac_unregister_timers(Lac_port *port)
{
	lac_unregister_timer(&port->tick_timer);
	lac_unregister_timer(&port->tx_scheduler);
}

void lac_sys_tick(void)
{
	tick_next = first_timer.next_timer;

	while (tick_next != NULL)
	{
		lac_tick_timer(tick_next);
		if (!lac_is_timer_active(tick_next))
		{
			tick_next->expiry_fn(tick_next->expiry_arg);
		}
		tick_next = tick_next->next_timer;
	}
	tick_next = first_timer.next_timer;
}

void lac_global_tick(unsigned long param)
{
	/* tick all timers from global list */
	lac_sys_tick();

	/* arm global timer */
	sys_timer.expires = jiffies + HZ; 
	add_timer(&sys_timer);
}

/**************************************************************************/

Boolean lac_find_port(int port_no, Lac_port **port)
{
	Lac_port *p;

	p = lac_ports;
	while (p)
	{
		if (p->port_no == port_no)
		{
			*port = p; 
			return True;
		}
		p = p->pNext;
	}
	return False;
}

int lac_set_admin_key(unsigned int port_no, La_Key key)
{
	Lac_aggr *agg;
	Lac_port *port;
	Boolean err;

	err=lac_find_port(port_no, &port);
	if(err==False)
	{
		return -EINVAL;
	}
	agg = port->agg;
	if(agg) spin_lock(&agg->lock);
	
	port->actor_admin.key = key;
	if(port->cfg_mask.pstat == Interface_link_off)
		port->actor.key = key;

	if(agg) spin_unlock(&agg->lock);

	schedule_work(&port->link_check);
	
	return 0;
}

int lac_set_port_priority(unsigned int port_no, unsigned short priority)
{
	Lac_port *port;
	Lac_aggr *agg;
	Boolean err;
	
	err=lac_find_port(port_no, &port);
	if(err==False)
	{
		return -EINVAL;
	}
	agg = port->agg;
	if(agg) spin_lock(&agg->lock);
	
	port->actor.port_priority = priority;
	port->actor_admin.port_priority = priority;

	if(agg) spin_unlock(&agg->lock);
	return 0;
}

int lac_set_mode(unsigned int port_no, Boolean activity)
{
	Lac_port *port;
	Lac_aggr *agg;
	Boolean err;
	
	err=lac_find_port(port_no, &port);
	if(err==False)
	{
		return -EINVAL;
	}
	agg = port->agg;
	if(agg) spin_lock(&agg->lock);

	port->actor.state.bmask.lacp_activity = activity;
	port->actor_admin.state.bmask.lacp_activity = activity;
	
	if(agg) spin_unlock(&agg->lock);
	return 0;
}

int lac_set_timeout(unsigned int port_no, Boolean timeout)
{
	Lac_aggr *agg;
	Lac_port *port;
	Boolean err;
	
	err=lac_find_port(port_no, &port);
	if(err==False)
	{
		return -EINVAL;
	}
	agg = port->agg;
	if(agg) spin_lock(&agg->lock);
	
	port->actor.state.bmask.lacp_timeout = timeout;
	port->actor_admin.state.bmask.lacp_timeout = timeout;

	if(agg) spin_unlock(&agg->lock);
	return 0;
}

int lac_set_port_sys_pri(Lac_port *p, System_priority pri)
{
	if(p->agg) spin_lock(&p->agg->lock);
	
	p->actor.system_priority = pri;
	p->actor_admin.system_priority = pri;

#if 0
	if(p->cfg_mask.lacp_enabled)
	{
		p->selected = Unselected;
		lac_mux_detached(p);
	}
#endif 

	if(p->agg) spin_unlock(&p->agg->lock);
	return 0;
}

int lac_set_sys_priority(System_priority pri)
{
	Lac_port *p = lac_ports;

	/* system lock */
	if(la_system.sPriority == pri)
		return 0;
	la_system.sPriority = pri;

	while(p)
	{
		lac_set_port_sys_pri(p, pri);
		p = p->pNext;
	}
	return 0;
}

int lac_set_sys_coll_delay(unsigned int value)
{
	/* system lock */
	la_system.collector_delay_time = value;
	return 0;
}

Boolean lac_compute_key(Lac_port *port)
{
	La_Key key, new_key;
	Boolean  key_change = False;

	key = port->actor_admin.key;

	if (port->cfg_mask.speed == Interface_10Mb)
	{
		new_key = key | LA_KEY_10MB; 
	}
	else if (port->cfg_mask.speed == Interface_100Mb)
	{
		new_key = key | LA_KEY_100MB; 
	}
	else if (port->cfg_mask.speed == Interface_1000Mb)
	{
		new_key = key | LA_KEY_1000MB; 
	}
	else if (port->cfg_mask.speed == Interface_10000Mb)
	{
		new_key = key | LA_KEY_10000MB; 
	}
	else
	{
		new_key = 0; 
	}

	if(port->actor.key != new_key)
	{
		port->actor.key = new_key;
		key_change = True;
	}
	return key_change;
}

/* Called from work queue to allow for calling functions that
 * might sleep (such as speed check)
 */
void lac_link_check(void *arg)
{
	Lac_port	*port = arg;
	Lac_aggr	*agg;
	int			port_link;
	int			port_duplx;
	int			port_speed;

	if(port == NULL)
		return;

	rtnl_lock();

	if (port->cfg_mask.lacp_enabled != Lacp_enabled)
		return;

	agg = port->agg;	
	
	/* get port physical information */
	lac_get_phy_info(port->dev, &port_link, &port_duplx, &port_speed);

	/* Update port status */
	if(port->cfg_mask.pstat != port_link)
	{
		port->cfg_mask.pstat = port_link;
	}

	if(port->cfg_mask.pstat == Interface_link_on)
	{
		/* update port speed */
		if(port->cfg_mask.speed != port_speed)
		{
			port->cfg_mask.speed = port_speed;
		}
	
		/* Update port duplex */
		if(port->cfg_mask.dplex != port_duplx)
		{
			port->cfg_mask.dplex = port_duplx;
		}

		if(port->cfg_mask.port_enabled == False)
		{
			if(agg)
			{
				port->cfg_mask.port_enabled = True;
			}
			else
			{
				lac_compute_key(port);
				lac_enable_port(port);
			}
		}
		else
		{
			if (port->cfg_mask.dplex == Interface_half_dup)
			{
				port->selected = Unselected;
				lac_mux_detached(port);

				/* Force timer to expire now */
				lac_stop_timer(&port->current_while_timer);
			}

			if(lac_compute_key(port))
			{
				Lac_warn("Operational key change for port %d\n", port->port_no);
				port->selected = Unselected;
				lac_mux_detached(port);
			}
		}
	}
	else   /* Link off */
	{
		if(port->cfg_mask.port_enabled == True)
		{
			Lac_warn("Link off, disable port %d\n", port->port_no);
			lac_disable_port(port);
			port->selected = Unselected;
			lac_mux_detached(port);
		}
	}
	rtnl_unlock();
}



void lac_check_all_links(void)
{
	Lac_port *p = lac_ports;
	while(p)
	{
		lac_link_check(p);
		p = p->pNext;
	}
}

int thread_monitor_links(void *arg)
{
	int sec_counter = 0;
	set_fs(KERNEL_DS);

	while(1)
	{
		down(&thread_sem);
		sec_counter++;

		if(sec_counter >= 10)
		{
			sec_counter = 0;
			Lac_printi("thread_monitor_links\n");
			/* check for status change for all ports */
			lac_check_all_links();
		}

		if(thread_stop)
		{
			complete_and_exit(&thread_comp, 0);
			return 0;
		}
	}
}

/**
 *	Change port aggregable state
 */
int lac_set_aggregable(unsigned int port_no, Boolean aggregate)
{
	Lac_port *port;
	Boolean cur_status;

	/* find port */
	if (!lac_find_port(port_no, &port))
		return -ERR_PORT_NOT_FOUND;

	cur_status = port->actor.state.bmask.aggregation == 1;

	if (cur_status == aggregate)
		return -ERR_ALREADY_SET;

	if (aggregate)
	{
		/* set port state aggregable */
		lac_enable_lacp(port);
	}
	else
	{
		/* set port state individual */
		lac_disable_lacp(port);
	}
	
	return 0;
}
