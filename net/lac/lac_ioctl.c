#include <linux/capability.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>

#include "lac_private.h"
#include "lac_debug.h"

#if 0
/* add/remove link (port or interface, they are the same thing)*/
static int add_del_if(Lac_aggr *lag, int ifindex, int isadd)
{
	struct net_device *dev;
	int ret;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	dev = dev_get_by_index(ifindex);
	if (dev == NULL)
		return -EINVAL;

	if (isadd)
		ret = lac_add_if(lag, dev);
	else
		ret = lac_del_if(dev);

	dev_put(dev);
	return ret;
}
#endif

int add_del_if_by_name(Lac_aggr *lag, char* ifname, int isadd)
{
	struct net_device *dev;
	int ret;
	
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	dev = dev_get_by_name(ifname);
	if (dev == NULL)
	{
		Lac_warn("device not obtained\n");
		return -EINVAL;
	}
	if(dev->sw_port==NULL)
	{
		//ADI nu se afla in switch deci nu am voie sa il adaug nici in LAC
		return -EINVAL;
	}
	
	if (isadd)
		ret = lac_add_if(lag, dev);
	else
		ret = lac_del_if(dev);

	dev_put(dev);
	Lac_warn("finished with device %s\n", ifname);
	return ret;
}
/*
int lac_ioctl_deviceless_stub(unsigned int cmd, void __user *uarg)
{
	switch (cmd) 
	{
		case SIOCLACADDAGG:
		case SIOCLACDELAGG:
		{
			char buf[IFNAMSIZ];

			if (!capable(CAP_NET_ADMIN))
				return -EPERM;

			if (copy_from_user(buf, uarg, IFNAMSIZ))
				return -EFAULT;

			buf[IFNAMSIZ-1] = 0;

			if (cmd == SIOCLACADDAGG)
			{
				return lac_add_aggr(buf);
			}
			else if (cmd == SIOCLACDELAGG)
			{
				return lac_del_aggr(buf);
			}
		}
	}

	return -EOPNOTSUPP;
}
*/
int lac_add_rem_agg (char *if_name,int cmd)
{
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (cmd == LACTL_ADD_AGG)
	{
		return lac_add_aggr(if_name);
	}
	else if (cmd == LACTL_REM_AGG)
	{
		return lac_del_aggr(if_name);
	}

	return -EINVAL;//ADI desi nu ar trebui sa ajung aici niciodata
}

/**
 *	Get system params; send to user
 */
void lac_get_sys_params(lac_cmd_t *user_cmd)
{
	sys_params_t sys_params;
	
	/* get system params */
	memcpy(&sys_params.sys_id, la_system.sysId, sizeof(System_id));
	sys_params.sys_prio = la_system.sPriority;
	sys_params.collector_max_delay = la_system.collector_delay_time;

	/* copy to user */
	copy_to_user((void __user *)user_cmd->param, &sys_params, sizeof(sys_params_t));
}

/**
 *	Get all ports' params; send to user
 */
void lac_get_port_params(lac_cmd_t *user_cmd)
{
	int i;
	port_params_t port_params;
	struct net_lac_port *port;

	port = lac_ports;
	if (!port) 
		return;
	
	i = 0;
	while (port)
	{
		/* port */
		port_params.port_no = port->port_no;
		strncpy(port_params.port_name, port->dev->name, MAX_PORT_NAME);
		port_params.port_status = port->cfg_mask.port_enabled;
		port_params.ai = port->actor.state.bmask.aggregation;
		port_params.timeout = port->actor.state.bmask.lacp_timeout;

		/* aggregator */
		if (port->agg)
		{
			port_params.agg_no = port->agg->aggr_no;
			strncpy(port_params.agg_name, port->agg->dev->name, MAX_AGG_NAME);
		}
		else 
		{
			port_params.agg_no = 0;
			sprintf(port_params.agg_name, "none");
		}

		/* actor */
		port_params.actor_admin_key = port->actor_admin.key;
		memcpy(&port_params.actor_id, port->dev->dev_addr, sizeof(System_id));
		port_params.actor_oper_key = port->actor.key;
		port_params.actor_port_prio = port->actor.port_priority;
		port_params.actor_state = port->actor.state.state;
		
		/* partner */
		port_params.part_port_no = port->partner.port_no;
		memcpy(&port_params.part_sys_id, port->partner.system_id, sizeof(System_id));
		port_params.part_admin_key = port->partner_admin.key;
		port_params.part_oper_key = port->partner.key;
		port_params.part_port_prio = port->partner.port_priority;
		port_params.part_state = port->partner.state.state;

		/* copy data to user */
		copy_to_user((void __user *)user_cmd->param + i*sizeof(port_params_t), 
			&port_params, sizeof(port_params_t));
		
		/* go to next port */
		i++;
		port = port->pNext;
	}
}
void lac_get_port_by_name(lac_cmd_t *user_cmd)
{
	int i,err,found;
	port_params_t port_params;
	struct net_lac_port *port;
	char name[MAX_PORT_NAME];
	err=copy_from_user(name,((port_params_t*)user_cmd->param)->port_name,MAX_PORT_NAME);
	if(err!=0)
	{
		return;
	}


	port = lac_ports;
	if (!port) 
		return;
	
	i = 0;
	found=0;
	while (port)
	{
		/* port */
		if(strncmp(name,port->dev->name,MAX_PORT_NAME)==0)
		{
			port_params.port_no = port->port_no;
			strncpy(port_params.port_name, port->dev->name, MAX_PORT_NAME);
			port_params.port_status = port->cfg_mask.port_enabled;
			port_params.ai = port->actor.state.bmask.aggregation;
			port_params.timeout = port->actor.state.bmask.lacp_timeout;

			/* aggregator */
			if (port->agg)
			{
				port_params.agg_no = port->agg->aggr_no;
				strncpy(port_params.agg_name, port->agg->dev->name, MAX_AGG_NAME);
			}
			else 
			{
				port_params.agg_no = 0;
				sprintf(port_params.agg_name, "none");
			}

			/* actor */
			port_params.actor_admin_key = port->actor_admin.key;
			memcpy(&port_params.actor_id, port->dev->dev_addr, sizeof(System_id));
			port_params.actor_oper_key = port->actor.key;
			port_params.actor_port_prio = port->actor.port_priority;
			port_params.actor_state = port->actor.state.state;
			
			/* partner */
			port_params.part_port_no = port->partner.port_no;
			memcpy(&port_params.part_sys_id, port->partner.system_id, sizeof(System_id));
			port_params.part_admin_key = port->partner_admin.key;
			port_params.part_oper_key = port->partner.key;
			port_params.part_port_prio = port->partner.port_priority;
			port_params.part_state = port->partner.state.state;

			/* copy data to user */
			copy_to_user((void __user *)user_cmd->param,&port_params, sizeof(port_params_t));

			//get out
			found=1;
			break;
		}
		/* go to next port */
		i++;
		port = port->pNext;
	}

	if(found==0)//not found
	{
		strncpy(port_params.port_name, name, MAX_PORT_NAME);
		port_params.port_no = -1;
		copy_to_user((void __user *)user_cmd->param,&port_params, sizeof(port_params_t));
	}
}

void lac_get_agg_params(lac_cmd_t *user_cmd)
{
	int i = 0;
	agg_params_t ap;
	struct net_lac_aggr *agg;

	list_for_each_entry(agg, &lac_aggrs_list, list)
	{
		/* get agg info */
		ap.agg_no = agg->aggr_no;
		strncpy(ap.agg_name, agg->dev->name, MAX_AGG_NAME);
		ap.agg_key = agg->key;
		ap.agg_distrib_ports = agg->nports;
		ap.nports = agg->link_count;
		ap.ready = agg->ready;

		/* copy data to user */
		copy_to_user((void __user *)user_cmd->param + i*sizeof(agg_params_t),
			&ap, sizeof(agg_params_t));
		
		i++;
	}
}

/* device configure commands */             //ADI TOATE FUNCTIA
//int lac_dev_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
int lac_dev_ioctl(struct net_device *dev, lac_cmd_t user_cmd,char *if_name,int command)
{
	struct net_lac_aggr *agg = netdev_priv(dev);
	lac_cfg_cmd_t *cfg_cmd;
	int err=0;

	/*TODO: for every cmd use proper locking mechanism*/
	switch (command) 
	{
	case LACTL_ADD_IF:
	case LACTL_DEL_IF:
		cfg_cmd = (lac_cfg_cmd_t *)user_cmd.param;	
		err=add_del_if_by_name(agg, cfg_cmd->port.port_name, command == LACTL_ADD_IF);
		break;

	case LACTL_SET_AGG_STATE_ENABLED:
	case LACTL_SET_AGG_STATE_DISABLED:
		/* set port aggregable state */
		cfg_cmd = (lac_cfg_cmd_t *)user_cmd.param;		
		err=lac_set_aggregable(cfg_cmd->port.port_no, command == LACTL_SET_AGG_STATE_ENABLED);
		break;

	case LACTL_SET_MODE_ACTIVE:
	case LACTL_SET_MODE_PASSIVE:
		cfg_cmd = (lac_cfg_cmd_t *)user_cmd.param;
		err=lac_set_mode(cfg_cmd->port.port_no, command == LACTL_SET_MODE_ACTIVE);
		break;

	case LACTL_SET_ADMIN_KEY:
		cfg_cmd = (lac_cfg_cmd_t *)user_cmd.param;
		err=lac_set_admin_key(cfg_cmd->port.port_no, cfg_cmd->value);
		break;

	case LACTL_SET_PORT_PRIO:
		cfg_cmd = (lac_cfg_cmd_t *)user_cmd.param;
		err=lac_set_port_priority(cfg_cmd->port.port_no, cfg_cmd->value);
		break;

	case LACTL_SET_SHORT_TIMEOUT:
	case LACTL_SET_LONG_TIMEOUT:
		cfg_cmd = (lac_cfg_cmd_t *)user_cmd.param;
		err=lac_set_timeout(cfg_cmd->port.port_no, command == LACTL_SET_SHORT_TIMEOUT);
		break;

	case LACTL_SET_SYS_PRIO:
		cfg_cmd = (lac_cfg_cmd_t *)user_cmd.param;
		err=lac_set_sys_priority(cfg_cmd->value);
		break;

	case LACTL_SET_SYS_MAX_DELAY:
		cfg_cmd = (lac_cfg_cmd_t *)user_cmd.param;
		err=lac_set_sys_coll_delay(cfg_cmd->value);
		break;

	case LACTL_GET_SYSTEM:
		/* acquire la_system lock */
		
		lac_get_sys_params(&user_cmd);
		
		/* release la_system lock */
		break;

	case LACTL_GET_PORTS:
		/* ports lock */

		lac_get_port_params(&user_cmd);

		/* release ports lock */
		
		break;
	case LACTL_GET_PORT_BY_NAME:
		/* ports lock */
		lac_get_port_by_name(&user_cmd);
		/* release ports lock */
		break;


	case LACTL_GET_AGG:
		lac_get_agg_params(&user_cmd);
		break;
	case LACTL_DEBUG_PORT:
		/* port debug */
		cfg_cmd = (lac_cfg_cmd_t *)user_cmd.param;
		LA_SET_PORT_DEB(cfg_cmd->port.port_no);
		break;

	case LACTL_DEBUG_RX:
		/* rx debug */
		debug_flag_change(DEB_LAC_RX_PDU);
		break;

	case LACTL_DEBUG_TX:
		/* tx debug */
		debug_flag_change(DEB_LAC_TX_PDU);
		break;

	case LACTL_DEBUG_STATE:
		/* state debug */
		debug_flag_change(DEB_LAC_STATE);
		break;
	case LACTL_DEBUG_WARN:
		/* warnings */
		debug_flag_change(DEB_LAC_WARN);
		break;
	case LACTL_ADD_AGG:
	case LACTL_REM_AGG:
		err=lac_add_rem_agg(if_name,command);
		break;

	default:
		return -EINVAL;//desi daca am facut bine caseul de la sw_ioctl nu ar trebui sa ajung aici niciodata
	}
	
	return err;
}












