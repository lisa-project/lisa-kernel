#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/unistd.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/timer.h>
#include "lac_private.h"
#include "lac_debug.h"

const unsigned char Slow_Protocols_Multicast[ETH_ALEN] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x02 };
Lac_system la_system;
struct net_lac_port *lac_ports; /* TODO: Trebuie sa fie safe*/
struct list_head lac_aggrs_list;

void tx_machine(Lac_port *port, Lac_event event);
void lac_register_timer(La_Timer *lac_timer, void (*call_fn)(void *), void *with_arg);
void rxm_lacp_disabled(Lac_port *port);

/**
 * LACP Globals
 */

/**
 * Add port to system list
 */
Boolean lac_add_sys_port(Lac_port *port)
{
	Lac_port *p = NULL;
	Lac_port *q = lac_ports;
	/* insert in sorted list */
	while (q && q->port_no < port->port_no)
	{
		p = q;
		q = q->pNext;
	}

	if (q && q->port_no == port->port_no)
	{
		Lac_warn("port %d already in system list\n", port->port_no);
		return False;
	}
	
	if(LA_PORT_DEB_ON(port->port_no))
	{
		Lac_printi("Port %d added to sistem list\n", port->port_no);
	}

	if (p == NULL) 
	{
		/* insert first node */
		port->pNext = lac_ports;
		lac_ports = port;
	}
	else
	{
		p->pNext = port;
		port->pNext = q;
	}
	return True;
}

/**
 * Remove port from system list
 */
void lac_del_sys_port(Lac_port *port)
{
	Lac_port *p = NULL;
	Lac_port *q = lac_ports;

	/* remove port from system list */
	while (q && q->port_no < port->port_no)
	{
		p = q;
		q = q->pNext;
	}

	if (q && q->port_no == port->port_no)
	{
		if (p)
			p->pNext = q->pNext;
		else
		{
			q = q->pNext;
			lac_ports = q;
		}

		if(LA_PORT_DEB_ON(port->port_no))
		{
			Lac_printi("Port %d removed from sistem list\n", port->port_no);
		}
	}
	
}

/* called under aggregator lock */
Lac_port *lac_get_port(Lac_aggr *agg, int port_no)
{
	Lac_port *p;

	list_for_each_entry_rcu(p, &agg->port_list, list) {
		if (p->port_no == port_no)
			return p;
	}

	return NULL;
}

/**
*	LACP functions
*/

/**
*	Copy port information.
*/
static void la_copy_info(Lac_info *from, Lac_info *to)
{
   memcpy((char *)&to->system_id, (char *)&from->system_id, sizeof(System_id));
   to->port_no               = from->port_no;
   to->port_priority         = from->port_priority;
   to->system_priority       = from->system_priority;
   to->key                   = from->key;
   to->state                 = from->state;
}

Boolean la_cmp_port_id(Lac_info *lac, Lac_info *another_lac)
{
	if (lac->port_no			== another_lac->port_no &&
		lac->port_priority		== another_lac->port_priority )
		return True;

	return False;
}

static Boolean la_same_port(Lac_info *lac, Lac_info *another_lac)
{
	if (lac->port_no == another_lac->port_no &&
		!memcmp(&lac->system_id, &another_lac->system_id, sizeof(System_id)))
		return True;

	return False;
}

/**
 * Compare port information.
 * Returns true if equal.
 */
static Boolean la_cmp_info(Lac_info * lac, Lac_info * another_lac)
{
	if (lac->port_no			== another_lac->port_no &&
		lac->port_priority		== another_lac->port_priority &&
		!strcmp((char*)&lac->system_id, (char*)&another_lac->system_id) &&
		lac->system_priority	== another_lac->system_priority &&
		lac->key				== another_lac->key &&
		LA_INFO_GET_BIT(lac, LA_AGGREGABLE_STATE_BIT) == 
			LA_INFO_GET_BIT(another_lac, LA_AGGREGABLE_STATE_BIT))
	{
			return True;
	}
	return False;
}

static Boolean la_cmp_info_ext(Lac_info *lac, Lac_info *another_lac)
{
	if (la_cmp_info(lac, another_lac) &&
		LA_INFO_GET_BIT(lac, LA_ACTIVITY_STATE_BIT) ==
			LA_INFO_GET_BIT(another_lac, LA_ACTIVITY_STATE_BIT) &&
		LA_INFO_GET_BIT(lac, LA_TIMEOUT_STATE_BIT) ==
			LA_INFO_GET_BIT(another_lac, LA_TIMEOUT_STATE_BIT) &&
		LA_INFO_GET_BIT(lac, LA_SYNC_STATE_BIT) == 
			LA_INFO_GET_BIT(another_lac, LA_SYNC_STATE_BIT))
	{
		return True;
	}

	return False;
}

/* TODO: set key from ioctl */
void lac_init_default_port(Lac_port *port)
{
	port->selected				= Unselected;
	port->mux_state				= Mux_Detached;
	port->cfg_mask.lacp_enabled = True;	/* aggregable */
	port->actor.port_no			= port->port_no;

	port->agg = NULL;
	port->pNext = NULL;

	/* default values */
	memset(&port->actor, 0, sizeof(Lac_info));
	port->actor_admin.key							= Default_key;
	port->actor_admin.port_priority					= Default_port_priority;
	port->actor_admin.state.bmask.lacp_activity		= Default_lacp_activity;
	port->actor_admin.state.bmask.lacp_timeout		= Default_lacp_timeout;
	port->actor_admin.state.bmask.aggregation		= Default_aggregation;
	port->actor_admin.state.bmask.synchronization	= False;
	port->actor_admin.state.bmask.defaulted			= True;
	port->actor_admin.state.bmask.expired			= False;

	memset(&port->partner, 0, sizeof(Lac_info));
	port->partner_admin.port_priority				= Default_port_priority;
	port->partner_admin.port_no						= port->port_no;
	port->partner_admin.system_priority				= Default_system_priority;
	memset(port->partner_admin.system_id, 0, sizeof(System_id));
	port->partner_admin.key							= Default_key;
	memset(port->partner_admin.system_id, 0, ETH_ALEN);
	port->partner_admin.state.bmask.lacp_activity   = False; /* Passive      */
	port->partner_admin.state.bmask.lacp_timeout    = False; /* Long timeout */
	port->partner_admin.state.bmask.aggregation     = True; /* Individual   */
	port->partner_admin.state.bmask.synchronization = True;
	port->partner_admin.state.bmask.collecting      = True;
	port->partner_admin.state.bmask.distributing    = True;
	port->partner_admin.state.bmask.defaulted       = True;
	port->partner_admin.state.bmask.expired         = False;
	
	/* register timers */
	lac_register_timers(port);
}

void lac_set_default_sys_info(void)
{
	Lac_port *p = lac_ports;
	memset(la_system.sysId, 0, sizeof(System_id));
	la_system.sPriority = Default_system_priority;
	
	memset(la_system.sysId, 0, sizeof(System_id));
	/* change system id for all ports */
	while(p)
	{
		memset(p->actor.system_id, 0, sizeof(System_id));
		memset(p->actor_admin.system_id, 0, sizeof(System_id));
		p->actor.system_priority = Default_system_priority;
		p->actor_admin.system_priority = Default_system_priority;
		p = p->pNext;
	}
}

void lac_set_system_info(Lac_port *port)
{
	Lac_port *p = lac_ports;
	/* system gets mac address from the first number port */
	if(port->port_no == 1)
	{
		memcpy(la_system.sysId, port->dev->dev_addr, sizeof(System_id));

		/* change system id for all ports */
		while(p)
		{
			memcpy(p->actor.system_id, la_system.sysId, sizeof(System_id));
			memcpy(p->actor_admin.system_id, la_system.sysId, sizeof(System_id));
			p = p->pNext;
		}
	}
	else
	{
		memcpy(port->actor.system_id, la_system.sysId, sizeof(System_id));
		memcpy(port->actor_admin.system_id, la_system.sysId, sizeof(System_id));
	}

}

void lac_init_port(Lac_port *port)
{
	tx_machine(port, Lac_init);
	periodic_tx_machine(port, Lac_init);
	mux_control(port, Lac_init);
	rx_machine(port, NULL, Lac_init);

	lac_stop_timer(&port->tick_timer);
	lac_start_timer(&port->tick_timer, Lac_ticks);
}

void lac_enable_port(Lac_port *port)
{
	port->cfg_mask.port_enabled = True;

	mux_control(port, Lac_new_info);
	rx_machine(port, NULL, Lac_port_enabled);
}

void lac_disable_port(Lac_port *port)
{
	Lac_warn("lac_port_disabled\n");
	port->cfg_mask.port_enabled = False;

	tx_machine(port, Lac_port_disabled);
	periodic_tx_machine(port, Lac_port_disabled);
	rx_machine(port, NULL, Lac_port_disabled);
	mux_control(port, Lac_new_info);
}


void lac_disable_lacp(Lac_port *port)
{
	port->actor.state.bmask.aggregation = False;
	port->cfg_mask.lacp_enabled = False;
	rxm_lacp_disabled(port);
}

void lac_enable_lacp(Lac_port *port)
{
	port->actor.state.bmask.aggregation = True;
	port->cfg_mask.lacp_enabled = True;
	lac_init_port(port);
	schedule_work(&port->link_check);
}

/**
 * Saves parameter values for the Actor carried in the LACPDU 
 * as the current partner operational parameter values.
 * Updates partner synchronization state.
 **/
static void recordPDU(Lac_port *port, Lac_packet *pdu)
{
	int is_active_link = 0;

	la_copy_info(&pdu->actor, &port->partner);
	ACT_OPER_CLR_BIT(port, LA_DEFAULTED_STATE_BIT);

	/*updates partner oper synchronization state*/
	is_active_link = (ACT_OPER_GET_BIT(pdu, LA_ACTIVITY_STATE_BIT) == True) ||
					 (ACT_OPER_GET_BIT(port, LA_ACTIVITY_STATE_BIT) == True && 
					 PART_OPER_GET_BIT(pdu, LA_ACTIVITY_STATE_BIT) == True);

	PART_OPER_CLR_BIT(port, LA_SYNC_STATE_BIT);
	if (la_cmp_info(&pdu->partner, &port->actor) &&
		ACT_OPER_GET_BIT(pdu, LA_SYNC_STATE_BIT) == True &&
		is_active_link
	   )
	{
			PART_OPER_SET_BIT(port, LA_SYNC_STATE_BIT);
	}
	
	if (ACT_OPER_GET_BIT(pdu, LA_AGGREGABLE_STATE_BIT) == False && /*individual link*/
		ACT_OPER_GET_BIT(pdu, LA_SYNC_STATE_BIT) == True &&
		is_active_link
	    )
	{
		PART_OPER_SET_BIT(port, LA_SYNC_STATE_BIT);
	}
}

static void actor_default(Lac_port *port)
{
   la_copy_info(&port->actor_admin, &port->actor);
}

/**
 * Set Partner operational parameter values to the values carried 
 * in the Partner admin parameteres.
 */	
static void recordDefault(Lac_port * port)
{
	la_copy_info(&port->partner_admin, &port->partner);
	ACT_OPER_SET_BIT(port, LA_DEFAULTED_STATE_BIT);
}

/**
 * Updates selected variable, using parameters from a newly received LACPDU
 */
static void update_Selected(Lac_port * port, Lac_packet *pdu)
{
	Lac_info *pdu_actor = &pdu->actor;

	if (!la_cmp_info(pdu_actor, &port->partner))
	{
		if(DEB_LAC_STATE_ON)
			lac_deb_print_state(port, "update_selected(Unselected)");
		port->selected = Unselected;
	}
	else
		if(DEB_LAC_STATE_ON)
			lac_deb_print_state(port, "update_selected");
}

/**
 * Updates Selected variable, using Partner administrative parameteres.
 */
static void update_Default_Selected(Lac_port * port)
{
	if (!la_cmp_info(&port->partner_admin, &port->partner))
	{
		Lac_warn("update_Default_Selected: Unselected");
		port->selected = Unselected;
	}
}

/**
 * Updates NTT variable, using parameter values from newly received LACPDU
 */
static void update_NTT(Lac_port *port, Lac_packet *pdu)
{
	Lac_info *pdu_partner = &pdu->partner;

	if (!la_cmp_info_ext(pdu_partner, &port->actor))
	{
		if(DEB_LAC_STATE_ON)
			lac_deb_print_state(port, "update_NTT:TRUE");
		tx_machine(port, Lac_tx_ntt);
	}
	else
	{
		if(DEB_LAC_STATE_ON)
			lac_deb_print_state(port, "update_NTT");
	}
}

/**
 * 
 */
static void ena_collecting(Lac_port *port)
{
	ACT_OPER_SET_BIT(port, LA_COLL_STATE_BIT);
}

/**
 * Port's aggregators stops collecting frames froms the port.
 */
static void dis_collecting(Lac_port *port)
{
	ACT_OPER_CLR_BIT(port, LA_COLL_STATE_BIT);
}

/* 
 * Update ready_ports field from aggregator. (All ports in distributing state)
 */
void lac_update_distrib_port(Lac_aggr *agg)
{
	int k = 0; /* num of ports which attaches this aggregator */
	Lac_port *p;

	/* spin_lock_bh(&agg->lock); */
	agg->nports = 0;
	
	list_for_each_entry(p, &agg->port_list, list)
	{
		if(p->mux_state == Mux_Distributing && ACT_OPER_GET_BIT(p, LA_DIST_STATE_BIT))
		{
			Lac_warn("Port %d ready for distributing\n", p->port_no);
			//rcu_assign_pointer(p, agg->ready_ports[k++]);
			agg->ready_ports[k++] = p;
		}
		else
		{
			Lac_warn("Port %d disabled from distributing\n", p->port_no);
		}
	}
	agg->nports = k;
	/* spin_unlock_bh(&agg->lock); */
}

/**
 * 
 */
static void ena_distributing(Lac_port *port)
{
	ACT_OPER_SET_BIT(port, LA_DIST_STATE_BIT);
	if(port->agg)
		lac_update_distrib_port(port->agg);
	else
		Lac_warn("Unattached port %d wants to distribute.\n", port->port_no);
}

/**
 * 
 */
static void dis_distributing(Lac_port *port)
{
	ACT_OPER_CLR_BIT(port, LA_DIST_STATE_BIT);

	if(port->agg)
		lac_update_distrib_port(port->agg);
}

/* called with rcu_read_lock */
void lac_rx(Lac_port *port, Lac_packet *pdu)
{
	Lac_port *p = lac_ports;

	rx_machine(port, pdu, Lac_received);
	mux_control(port, Lac_received); 

	while(p)
	{
		if (p != port)
			rx_machine(p, pdu, Lac_check_moved);
		p = p->pNext;
	}
	return;
}

/**
*	State Machines
*/

/**
*	Receive Machine
*/
void rxm_initialize(Lac_port *port)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Initialize");

	port->selected = Unselected;
	actor_default(port);
	recordDefault(port);
	ACT_OPER_CLR_BIT(port, LA_EXPIRED_STATE_BIT);
	port->cfg_mask.port_moved = False;

	/* set state */
	port->rxm = Rxm_initialize;
}

void rxm_port_disabled(Lac_port *port)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Port_disabled");
	PART_OPER_CLR_BIT(port, LA_SYNC_STATE_BIT); 
	
	/* set state */
	port->rxm = Rxm_port_disabled;
}

void rxm_expired(Lac_port *port)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Expired");
	PART_OPER_CLR_BIT(port, LA_SYNC_STATE_BIT); 
	/* set short timeout */
	PART_OPER_SET_BIT(port, LA_TIMEOUT_STATE_BIT);
	lac_start_timer(&port->current_while_timer, Short_Timeout_Time);
	ACT_OPER_SET_BIT(port, LA_EXPIRED_STATE_BIT);

	/* set state */
	port->rxm = Rxm_expired;
}

void rxm_lacp_disabled(Lac_port *port)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Lacp_disabled");

	port->selected = Unselected;
	recordDefault(port);
	PART_OPER_CLR_BIT(port, LA_AGGREGABLE_STATE_BIT);
	ACT_OPER_CLR_BIT(port, LA_EXPIRED_STATE_BIT); 

	/* set state */
	port->rxm = Rxm_lacp_disabled;
}

void rxm_defaulted(Lac_port *port)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Defaulted");

	update_Default_Selected(port);
	recordDefault(port);
	ACT_OPER_CLR_BIT(port, LA_EXPIRED_STATE_BIT);

	/* set state */
	port->rxm = Rxm_defaulted;
}

/* called after successfull receive.
 * called with rcu_read_lock 
 */
void rxm_current(Lac_port *port, Lac_packet *pdu)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Current");

	if(port->actor.port_no != port->port_no)
		port->actor.port_no = port->port_no;

	update_Selected(port, pdu);
	update_NTT(port, pdu);
	recordPDU(port, pdu);
	/* get lacp_timeout */
	if (ACT_OPER_GET_BIT(port, LA_TIMEOUT_STATE_BIT) == Short_timeout)
		lac_start_timer(&port->current_while_timer, Short_Timeout_Time);
	else
		lac_start_timer(&port->current_while_timer, Long_Timeout_Time);
	ACT_OPER_CLR_BIT(port, LA_EXPIRED_STATE_BIT);

	/* set state */
	port->rxm = Rxm_current;
}

void rx_machine(Lac_port *port, Lac_packet *pdu, Lac_event event)
{
	switch(event)
	{
	case Lac_check_moved:/* called with rcu_read_lock */
		if (port->rxm != Rxm_port_disabled ||
			!la_same_port(&port->partner, &pdu->actor))
			break;

		/* continue to init */		
	case Lac_init:
		rxm_initialize(port);
		/* UCT */
	case Lac_port_disabled:
		port->cfg_mask.port_enabled = False;
		rxm_port_disabled(port);
		break;

	case Lac_port_enabled:
		if (port->rxm != Rxm_port_disabled) 
			break;

		if (port->cfg_mask.lacp_enabled)
			rxm_expired(port);
		else
			rxm_lacp_disabled(port);
		break;

	case Lac_tick:
		//if(DEB_LAC_STATE_ON)
		//	lac_deb_print_state(port, "la_rxm_tick");
		lac_tick_timer(&port->current_while_timer);
		if (!lac_is_timer_active(&port->current_while_timer))
		{
			if (port->rxm == Rxm_current)
				rxm_expired(port);
			else if (port->rxm == Rxm_expired)
				rxm_defaulted(port);
		}
		break;

	case Lac_received:/* called with rcu_read_lock */
		if ((port->rxm == Rxm_lacp_disabled) || (port->rxm == Rxm_port_disabled))
			break;

		/* enter CURRENT state */
		rxm_current(port, pdu);
		break;

	default:
		Lac_warn("rx_machine: event not handled\n");
		break;
	}
}

/**
*	Selection Logic
*/

void sl_set_ready(Lac_port *port)
{
	Lac_aggr *agg = port->agg;
	Lac_port *p;
	
	if(!agg || agg->ready == True)
		return;

	agg->ready = True;
	list_for_each_entry_rcu(p, &agg->port_list, list)
		if(!p->cfg_mask.ready_N)
		{
			agg->ready = False;
			break;
		}
}

static Boolean lac_partners_loop(Lac_aggr *a, Lac_port *port)
{
   Lac_port *p;

   list_for_each_entry_rcu(p, &a->port_list, list)
   {
	   if(la_same_port(&p->actor, &port->partner))
		   return True;
   }
   return False;
}

/* ports can aggregate */
static Boolean lac_port_aggregable(Lac_port *p, Lac_port *port)
{
   if (	(p->actor.system_priority     == port->actor.system_priority    ) &&
		(!memcmp(&p->actor.system_id, &port->actor.system_id, sizeof(System_id))) &&
		(p->actor.key                 == port->actor.key                ) &&
		(p->partner.system_priority   == port->partner.system_priority  ) &&
		(!memcmp(&p->partner.system_id, &port->partner.system_id, sizeof(System_id))) &&
		(p->partner.key               == port->partner.key              ) &&
		(ACT_OPER_GET_BIT(p, LA_AGGREGABLE_STATE_BIT) && PART_OPER_GET_BIT(p, LA_AGGREGABLE_STATE_BIT)) &&
		(ACT_OPER_GET_BIT(port, LA_AGGREGABLE_STATE_BIT)&& PART_OPER_GET_BIT(port, LA_AGGREGABLE_STATE_BIT)) &&
		(p->cfg_mask.port_enabled)
	  )
   {
	   Lac_printd("lac_port_aggregable: ports %d, %d aggregable\n", p->port_no, port->port_no);
	   return True;
   }
   
   Lac_printd("lac_port_aggregable: ports %d, %d NOT aggregable\n", p->port_no, port->port_no);
   return False;
}


/**
*	Mux Machine
*/
static Boolean lac_mux_verify_agg(Lac_aggr *agg, Lac_port *port)
{
	Lac_port *p;

	if (!agg || !agg->key)
		return False;

	if (DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "lac_mux_verify_agg");
	
	list_for_each_entry_rcu(p, &agg->port_list, list)
	{
		if(p->port_no != port->port_no)
		/* still selected in aggregator */
		if (p->selected == Selected)
		{
			if (lac_port_aggregable(p, port) == True && lac_partners_loop(agg, port) == False)
				return True;
			else
				return False;
		}
	}

	return True;
}

Boolean lac_agg_contains_port(Lac_aggr *agg, Lac_port *port)
{
	Lac_port *p;
	list_for_each_entry_rcu(p, &agg->port_list, list)
	{
		if (p->port_no == port->port_no)
			return True;
	}
	return False;
}

void lac_add_port_to_agg(Lac_aggr *agg, Lac_port *port)
{
	list_add_rcu(&port->list, &agg->port_list);
	agg->link_count++;
	port->agg = agg;

	/* set mac address for pseudo-device of aggregator */
	lac_set_min_mac_address(agg->dev);
	
	/*TODO: MTU check */
	//dev_set_mtu(agg->dev, lac_min_mtu(agg));
}

/**
 * Find proper aggregator for port.
 * Add port to aggregator port list.
 */
static Boolean lac_mux_find_aggr(Lac_port *port)
{
	Lac_aggr *a;
	
	if (DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "lac_mux_find_aggr");

	/* iterate through all aggrs in system */
	list_for_each_entry(a, &lac_aggrs_list, list)
	{
		/* check aggregator */
		if (lac_mux_verify_agg(a, port))
		{
			/* check if already in aggregator */
			if (lac_agg_contains_port(a, port))
				return True;

			/* attach port to aggregator */
			lac_add_port_to_agg(a, port);
			/* TODO: maybe process standby links */
			return True;
		}
	}

    /* Find an unused aggregator for this port. */
	Lac_warn("find an unused aggregator\n");
	list_for_each_entry_rcu(a, &lac_aggrs_list, list)
	{
		if (a->key == 0)
		{
			a->key = port->actor.key;
			lac_add_port_to_agg(a, port);
			Lac_warn("aggregator %d found for port %d\n", a->aggr_no, port->port_no);
			return True;
		}
	}

	return False;
}

static Boolean lac_mux_remove_aggr(Lac_port *port)
{
	Lac_aggr *agg = port->agg;
	Lac_port *p;

	if (!agg)
		return True;

	/* remove from aggregator list */
	list_del_rcu(&port->list);
	port->agg = NULL;
	if (agg->link_count)
		agg->link_count--;

	if (agg->link_count == 0)
	{
		agg->key = 0;
		agg->ready = False;
	}
	
	lac_set_min_mac_address(agg->dev);

	Lac_warn("lac_mux_remove_aggr: Removed port %d from aggr %d\nPort list: ", port->port_no, agg->aggr_no);
	list_for_each_entry_rcu(p, &agg->port_list, list)
	{
		Lac_warn("%d ", p->port_no);
	}
	Lac_warn("\n");
	return True;
}

/**
 * Mux state machine
 */
void lac_mux_detached(Lac_port *port)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Detached");

	ACT_OPER_CLR_BIT(port, LA_SYNC_STATE_BIT);
	dis_distributing(port);
	dis_collecting(port);
	lac_mux_remove_aggr(port);
	tx_machine(port, Lac_tx_ntt);

	port->mux_state = Mux_Detached;
}

void lac_mux_waiting(Lac_port *port)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Waiting");

	if(ACT_OPER_GET_BIT(port, LA_AGGREGABLE_STATE_BIT) == True && PART_OPER_GET_BIT(port, LA_AGGREGABLE_STATE_BIT) == True)
	{
		lac_start_timer(&port->wait_while_timer, Aggregate_Wait_Time);
	}
	else
	{
		Lac_warn("start wait_while_timer ZERO\n");
		lac_start_timer(&port->wait_while_timer, 0);
	}

	/* set Mux_Waiting state */
	port->mux_state = Mux_Waiting;
}

void lac_mux_attached(Lac_port *port)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Attached");
	ACT_OPER_SET_BIT(port, LA_SYNC_STATE_BIT);
	dis_collecting(port);
	tx_machine(port, Lac_tx_ntt);

	port->mux_state = Mux_Attached;
}

void lac_mux_collecting(Lac_port *port)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Collecting");

	port->mux_state = Mux_Collecting;

	ena_collecting(port);
	dis_distributing(port);
	tx_machine(port, Lac_tx_ntt);
}

void lac_mux_distributing(Lac_port *port)
{
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "Distributing");

	port->mux_state = Mux_Distributing;
	ena_distributing(port);
}

static void lac_mux_attach(Lac_port *port)
{
	Lac_port *p;
	Lac_aggr * agg = port->agg;
	
	if(!agg)
		Lac_warn("lac_mux_attach: port %d has no aggregator\n", port->port_no);

	/* check for detaching or waiting ports */
	sl_set_ready(port);

	if(agg->ready == True)
	{
		list_for_each_entry_rcu(p, &agg->port_list, list)
		{
			if( p->selected == Selected && p->cfg_mask.ready_N == True &&
				(/* p->mux_state == Mux_Detached || */ p->mux_state == Mux_Waiting) )
			{
				p->cfg_mask.ready_N = False;

				lac_mux_attached(p);
			}
		}
	}
}

static void lac_mux_select(Lac_port *port)
{
	Lac_aggr *agg = port->agg;

	if (agg != NULL) /* still linked to previous aggregator */
	{
		/* remove from previous aggregator */
		lac_mux_remove_aggr(port);
	}

	if (agg != NULL)
		return;

	if (port->cfg_mask.lacp_enabled == False)
		return;
	
	if(DEB_LAC_STATE_ON)
		lac_deb_print_state(port, "lac_mux_select(selected)");

	/* search new aggregator
	 * add port cu aggregator's port list
	*/
	if (!lac_mux_find_aggr(port))
	{
		Lac_warn("port %d: lac_mux_find_aggr failed\n", port->port_no);	
		return;
	}

	port->selected = Selected;
	
	/* go to Waiting state*/
	lac_mux_waiting(port);
}

static void lac_mux_update_state(Lac_port *port)
{
	Lac_state_mask *as = &port->actor.state.bmask;
	Lac_state_mask *ps = &(port->partner.state.bmask);

	unsigned int actor_sync = (port->selected == Selected) && 
		(port->mux_state != Mux_Detached) && (port->mux_state != Mux_Waiting);

	unsigned int partner_sync = ps->synchronization;

	//Lac_warn(">> lac_mux_update_state\n");

	if (as->synchronization != actor_sync)
	{
			as->synchronization = actor_sync;
			tx_machine(port, Lac_tx_ntt);
	}

	ps->synchronization = partner_sync;
	
	/* Collecting -> Distributing */
	if (ps->synchronization && ps->collecting &&
		as->synchronization && as->collecting && !as->distributing)
	{
		Lac_warn("port %d: Collecting -> Distributing\n", port->port_no);
		lac_mux_distributing(port);
	}
	/* Attached -> Collecting */
	else if (ps->synchronization && as->synchronization && !as->collecting && port->mux_state != Mux_Collecting)
	{
		Lac_warn("port %d: Attached -> Collecting\n", port->port_no);
		lac_mux_collecting(port);
	}
	/* Distributing -> Collecting */
	else if ((!ps->synchronization || !ps->collecting || !as->synchronization) && 
			  as->distributing)
	{
		Lac_warn("port %d: Distributing -> Collecting\n", port->port_no);
		lac_mux_collecting(port);
	}
	/* Collecting -> Attached */
	else if ((!ps->synchronization || !as->synchronization) &&  as->collecting)
	{
		Lac_warn("port %d: Collecting -> Attached\n", port->port_no);
		as->collecting = False;     
		tx_machine(port, Lac_tx_ntt);
	}
	/* Attached | Waiting -> Detached */
	else if ((port->selected != Selected) && (port->mux_state != Mux_Detached) )
	{
		Lac_warn("port %d: Attached | Waiting -> Detached\n", port->port_no);
		lac_mux_detached(port);
	}
	/* Detached -> Waiting */
	else if (port->cfg_mask.lacp_enabled == True && 
			 port->cfg_mask.port_enabled == True && 
			 port->selected != Selected			 && 
			 port->mux_state == Mux_Detached)
	{
		Lac_warn("port %d: Detached -> Waiting\n", port->port_no);
		lac_mux_select(port); 
	}

	/* Waiting -> Attached */
	if (port->selected == Selected && !lac_is_timer_active(&port->wait_while_timer) && 
		port->mux_state == Mux_Waiting)
	{
		Lac_warn("port %d: Waiting -> Attached\n", port->port_no);
		lac_mux_attach(port);
	}  
}

/**
 * Mux external events 
 */

/* check all ports from system */
static void lac_mux_change_lag(Lac_port *port)
{
	Lac_port  *p = lac_ports;
	while (p)
	{
		if (p == port)
		{
			p = p->pNext;
			continue;
		}

		if (p->cfg_mask.lacp_enabled && p->selected == Selected && lac_mux_verify_agg(p->agg, p) == False)
		{
			p->selected = Unselected;
			lac_mux_update_state(p);
		}
		p = p->pNext;
	}
}

static void lac_mux_init(Lac_port *port)
{
	port->actor.port_no = port->port_no;
	port->selected    = Unselected;
	port->agg = NULL;
	ACT_OPER_CLR_BIT(port, LA_COLL_STATE_BIT);
	ACT_OPER_CLR_BIT(port, LA_DIST_STATE_BIT);
	port->mux_state = Mux_Detached;
	lac_stop_timer(&port->wait_while_timer);
	return;
}

void lac_mux_tick(Lac_port *port)
{
	lac_tick_timer(&port->wait_while_timer);

	if(!lac_is_timer_active(&port->wait_while_timer) && port->cfg_mask.ready_N == False)
		port->cfg_mask.ready_N = True;
}

/* Port status has changed: enable/disable */
static void lac_mux_new_info(Lac_port *port)
{
	/* port must be enabled to participate to protocol */
	if(port->selected == Unselected && port->cfg_mask.port_enabled == True)
	{
		/* check aggregator match for all ports in system */
		lac_mux_change_lag(port);
	}

	/* TODO: check standby links */
}

/* called with rcu_read_lock */
static void lac_mux_received(Lac_port *port)
{
	if(port->selected == Unselected && port->cfg_mask.port_enabled == True)
	{
		/* check aggregator match for all ports in system */
		lac_mux_change_lag(port);
	}
}

void mux_control(Lac_port *port, Lac_event event)
{
	switch(event)
	{
	case Lac_init:
		lac_mux_init(port);
		return;

	case Lac_tick:
		lac_mux_tick(port);
		break;

	case Lac_received:/* LACPDU received */
		lac_mux_received(port);
		break;

	case Lac_new_info:
		lac_mux_new_info(port);
		break;

	default:
		break;
	}

	lac_mux_update_state(port);
}

/**
*	Transmit Machine
*/

static int hash_mac(unsigned char *mac_h, int k)
{
	__u32 hash;
	struct ethhdr *eh = (struct ethhdr *)mac_h;

	hash = *(__u32 *)(&eh->h_dest[ETH_ALEN - 4]) ^ *(__u32 *)(&eh->h_source[ETH_ALEN - 4]);
	hash ^= hash >> 16;
	hash ^= hash >> 8;
	return hash % k;
}

#define IPH_MF 0x2000

int lac_get_unified_hash(struct iphdr *iph, int hash, Lac_aggr *agg)
{
	Lac_hash_table **dt, *dp, *dpp;
	int ret = hash;

	dt = &(agg->dht[iph->id % 32]);
	for (dp = *dt; dp; dp = dp->next)
	{
		if (dp->id == iph->id && dp->daddr == iph->daddr && dp->saddr == iph->saddr)
		{
			if (dp->hash >= agg->link_count)
				dp->hash = 0;
			ret = dp->hash;

			if (!(iph->frag_off & IPH_MF))
				/* delete entry */
				for (dpp = *dt; dpp; dpp = dpp->next)
				{
					if (dpp->next == dp)
					{
						dpp->next = dp->next;
						kfree(dp);
						break;
					}
				}
			return ret;
		}
	}

	/* not found in the table. register it if some fragments follow it. */
	if (iph->frag_off & IPH_MF)
	{
		dp = *dt;
		*dt = kmalloc(sizeof(struct dist_hash_table), GFP_ATOMIC);
		(*dt)->next = dp;
		(*dt)->hash = hash;
		(*dt)->id = iph->id;
		(*dt)->saddr = iph->saddr;
		(*dt)->daddr = iph->daddr;
	}
	return ret;
}

#define IPPROTO_TCP		6
#define IPPROTO_UDP	17 

int lac_hash_tcpudp(unsigned char *mac, int len, Lac_aggr *agg)
{
	int ret;
	__u32 hash;
	struct ethhdr *eh;
	struct iphdr *iph;
	struct tcphdr *tcph;
	struct udphdr *udph;

	eh = (struct ethhdr *)mac;

	/* check if IP datagram is contained */
	if (ntohs(eh->h_proto) != ETH_P_IP)
	{
		return hash_mac(mac, agg->link_count);
	}

	iph = (struct iphdr *)(mac + ETH_HLEN);

	/* check if TCP segment or UDP datagram is contained */
	if (iph->protocol == IPPROTO_TCP)
	{
		tcph = (struct tcphdr *)(mac + ETH_HLEN + iph->ihl * 4);
		hash = tcph->source ^ tcph->dest;
		hash ^= hash >> 8;
		ret = hash % agg->link_count;
		return lac_get_unified_hash(iph, ret, agg);
	} 
	else if (iph->protocol == IPPROTO_UDP)
	{
		udph = (struct udphdr *)(mac + ETH_HLEN + iph->ihl * 4);
		hash = udph->source ^ udph->dest;
		hash ^= hash >> 8;
		ret = hash % agg->link_count;
		return lac_get_unified_hash(iph, ret, agg);
	}

	return hash_mac(mac, agg->link_count);
}


static Boolean lac_xmit_lacpdu(Lac_port *port)
{
	struct sk_buff *skb;
	Lac_pdu pdu;
	int size;
	struct ethhdr *eh;

	int pdu_len = LACPDU_LEN;
	size = pdu_len + 2*ETH_ALEN + 2;
	if (size < 60)
		size = 60;

	memset(&pdu, 0x0, pdu_len);

	/* build LACPDU */
	pdu.subtype = 1;//LACP_Subtype;
	pdu.version_number = LACP_Version;

	/* Actor Information */
	pdu.first_tlv_type = 0x1; 
	pdu.actor_info_len = 0x14;
	pdu.actor_sys_pri = htons(port->actor.system_priority);
	memcpy(pdu.actor_sys, port->actor.system_id, ETH_ALEN);
	pdu.actor_key = htons(port->actor.key);
	pdu.actor_port_pri = htons(port->actor.port_priority);
	pdu.actor_port = htons(port->port_no);
	pdu.actor_state = port->actor.state.state;

	/* Partner Information */
	pdu.second_tlv_type = 0x2; 
	pdu.partner_info_len = 0x14;
	pdu.partner_sys_pri = htons(port->partner.system_priority);
	memcpy(pdu.partner_sys, port->partner.system_id, ETH_ALEN);
	pdu.partner_key = htons(port->partner.key);
	pdu.partner_port_pri = htons(port->partner.port_priority);
	pdu.partner_port = htons(port->partner.port_no);
	pdu.partner_state = port->partner.state.state;

	/* Collector Information */
	pdu.third_tlv_type = 0x3; 
	pdu.collector_info_len = 0x10;
	pdu.collector_max_del = htons(la_system.collector_delay_time);

	/* Terminator */
	pdu.fourth_tlv_type = 0x0; 
	pdu.terminator_len = 0x0;

	skb = dev_alloc_skb(size);

	if (!skb){
		return False;
	}

	skb->dev = port->dev;
	skb_put(skb, size);
	skb_set_mac_header(skb, size);
	memcpy(skb_mac_header(skb), Slow_Protocols_Multicast, ETH_ALEN);
	memcpy(skb_mac_header(skb) + ETH_ALEN, port->dev->dev_addr, ETH_ALEN);

	eh = (struct ethhdr *)skb_mac_header(skb);
	eh->h_proto = htons(Slow_Protocols_Ethertype);

	skb->network_header = skb_mac_header(skb) + 2*ETH_ALEN + 2;
	memcpy(skb_network_header(skb), &pdu, pdu_len);
	memset(skb_network_header(skb) + pdu_len, 0xa5, size - pdu_len - 2*ETH_ALEN - 2);
	Lac_warn("LACPDU sent on port %d\n\n", port->port_no);
	dev_queue_xmit(skb);
	return True;
}

void tx_machine(Lac_port *port, Lac_event event)
{
	switch (event)
	{
	case Lac_init:
	case Lac_port_disabled:
		lac_stop_timer(&port->tx_scheduler);
		port->cfg_mask.ntt = False;
		lac_stop_timer(&port->ntt_delay_timer);
		port->ntt_count = 0;
		break;

	case Lac_tx_ntt:
		/* do we need to check if it's Fast_Periodic_Timeout */
		if( !port->cfg_mask.ntt && port->ntt_count < Max_tx_per_interval )
		{
			lac_start_timer(&port->tx_scheduler, Lac_tx_scheduling_ticks);
		}
		port->cfg_mask.ntt = True;
		break;

	case Lac_tick:
		lac_tick_timer(&port->ntt_delay_timer);
		if (!lac_is_timer_active(&port->ntt_delay_timer))
		{
			if (port->cfg_mask.ntt && (port->ntt_count >= Max_tx_per_interval))
				lac_start_timer(&port->tx_scheduler, Lac_tx_scheduling_ticks);

			lac_stop_timer(&port->ntt_delay_timer);
		}
		break;

	case Lac_txd:
		if ( port->cfg_mask.ntt			&& 
			port->cfg_mask.lacp_enabled && 
			port->cfg_mask.port_enabled &&
			(ACT_OPER_GET_BIT(port, LA_ACTIVITY_STATE_BIT) || PART_OPER_GET_BIT(port, LA_ACTIVITY_STATE_BIT))
		   )
		{
			if (lac_xmit_lacpdu(port))
			{
				lac_start_timer(&port->ntt_delay_timer, Tx_interval_ticks);
				port->ntt_count ++;
				port->cfg_mask.ntt = False;
			}
			else 
				lac_start_timer(&port->tx_scheduler, Lac_tx_scheduling_ticks);            
		}
		break;

	default:
		Lac_warn("tx_machine: event not handled\n");
		break;
	}
}

void tx_opportunity(void *p)
{
	Lac_port *port = (Lac_port *)p;
	tx_machine(port, Lac_txd);
}

/**
*	Periodic Transmission Machine
*/
void prm_no_periodic(Lac_port *port)
{
	/* stop periodic timer */
	lac_stop_timer(&port->periodic_timer);
}

void prm_fast_periodic(Lac_port *port)
{
	/* start periodic timer - fast periodic time */
	lac_start_timer(&port->periodic_timer, Fast_Periodic_Time);
}

void prm_slow_periodic(Lac_port *port)
{
	/* start periodic timer - slow periodic time */
	lac_start_timer(&port->periodic_timer, Slow_Periodic_Time);
}

void periodic_tx_machine(Lac_port *port, Lac_event event)
{
	switch (event) 
	{
	case Lac_init:
	case Lac_port_disabled:
		prm_no_periodic(port);
		break;
	case Lac_tick:
		if (!port->cfg_mask.port_enabled ||	!port->cfg_mask.lacp_enabled ||
			(ACT_OPER_GET_BIT(port, LA_ACTIVITY_STATE_BIT) == False &&
			PART_OPER_GET_BIT(port, LA_ACTIVITY_STATE_BIT) == False))
		{
			prm_no_periodic(port);
			break;
		}

		lac_tick_timer(&port->periodic_timer);
		if (!lac_is_timer_active(&port->periodic_timer))
		{
			/* ntt = true */
			tx_machine(port, Lac_tx_ntt);
			/* reinit timer */
			if (PART_OPER_GET_BIT(port, LA_TIMEOUT_STATE_BIT) == Short_timeout)
			{
				prm_fast_periodic(port);
			}
			else
			{
				prm_slow_periodic(port);
			}
		}
		break;
	default:
		Lac_warn("periodic_tx_machine: wrong event\n");
		break;
	}
}

/* tick function for every machine */
void lac_port_tick(void *p)
{
	Lac_port *port = (Lac_port *)p;
	rx_machine(port, NULL, Lac_tick);
	mux_control(port, Lac_new_info);
	mux_control(port, Lac_tick);
	periodic_tx_machine(port, Lac_tick);
	tx_machine(port, Lac_tick);
	lac_start_timer(&port->tick_timer, Lac_ticks);
}

