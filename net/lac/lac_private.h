#ifndef _LAC_PRIVATE_H
#define _LAC_PRIVATE_H

#include <linux/netdevice.h>
#include <linux/miscdevice.h>
#include <linux/if_lac.h>
#include <linux/net_lac.h>

#include "lac_protocol.h"

#define LA_VERSION	"1.0"

#define LAC_PORT_BITS	10
#define LAC_MAX_PORTS	50
#define LAC_MAX_AGGRS LAC_MAX_PORTS

typedef struct net_lac_aggr Lac_aggr;
typedef struct net_lac_port Lac_port;

/**
 * Aggregator data structure
 */
struct net_lac_aggr
{
	struct net_device		*dev;
	struct list_head		list;
	struct net_device_stats	statistics;
	spinlock_t				lock;
	La_Short				aggr_no;	/* Aggregator number */
	
	La_Key					key;
	Boolean					ready;
	La_Short				link_count;
	struct list_head		port_list;	/* Ports that belong to the same LAG */

	Lac_hash_table		   *dht[32];
	Lac_port			   *ready_ports[LAC_MAX_PORTS];
	int						nports;
};



/**
 * LA : Port data structure
 */
struct net_lac_port
{
	struct net_device *dev;
	struct list_head list;
	int port_no;							/* actor port number */
	
	Lac_port       *pNext;					/* for system list */

	Lac_info        actor;
	Lac_info        actor_admin;
	Lac_info        partner;
	Lac_info        partner_admin;
 
	La_Byte         rxm;					/* Rx state */
	La_Byte         selected;				/* Port_select */

	/* Timers */
	La_Timer        current_while_timer;	/* received protocol info has expired */
	La_Timer        periodic_timer;			/* generate periodic transmission */
	La_Timer        wait_while_timer;		/* checks stabilization in aggregation change */ /* inlocuit cu wait_while */
	La_Timer		ntt_delay_timer;

	int				ntt_count;				/* no. of LACPDUs transmitted in current Fast_Periodic_Interval */

	/* Real timers */
	La_Timer		tick_timer;
	La_Timer		tx_scheduler;


	/* Aggregator control parameters */
	La_Byte         mux_state;				/* Mux_state */
	Lac_aggr *agg;					   		/* my aggregator */

	/* Configuration parameters. */
	Lac_port_cfg_mask	cfg_mask;

	/* Collector information. */
	La_Short collector_max_delay;     

	/* State information. */
	La_Byte last_event;

	/* work queue for defering jobs that might sleep.
	 * used in getting speed with ioctl ethtool */
	struct work_struct		link_check;
	struct work_struct		pdu_rx;
	struct rcu_head			rcu;
};

/* lac_protocol.c */
extern struct list_head		lac_aggrs_list;
extern struct net_lac_port *lac_ports;
extern Lac_port_stats		lac_port_stats[LAC_MAX_PORTS];
extern Lac_system la_system;

extern Boolean	lac_add_sys_port(Lac_port *port);
extern void		lac_del_sys_port(Lac_port *port);
extern void		lac_init_default_port(Lac_port *port);
extern void		lac_init_port(Lac_port *port);
extern void		lac_enable_port(Lac_port *port);
extern void		lac_disable_port(Lac_port *port);
extern void		lac_set_system_info(Lac_port *port);
extern void		lac_set_default_sys_info(void);

extern void		tx_opportunity(void *port);
extern void		lac_port_tick(void *port);

extern void		periodic_tx_machine(Lac_port *port, Lac_event event);
extern void		tx_machine(Lac_port *port, Lac_event event);
extern void		rx_machine(Lac_port *port, Lac_packet *pdu, Lac_event event);
extern void		mux_control(Lac_port *port, Lac_event event);
extern void		lac_rx(Lac_port *port, Lac_packet *pdu);
extern int		lac_hash_tcpudp(unsigned char *mac, int len, Lac_aggr *agg);

extern void lac_mux_detached(Lac_port *port);
extern void lac_stop_timer(La_Timer *lac_timer);
extern void lac_disable_lacp(Lac_port *port);
extern void lac_enable_lacp(Lac_port *port);
extern void lac_update_distrib_port(Lac_aggr *agg);

/*lac_device.c*/
extern void lac_dev_setup(struct net_device *dev);
extern int lac_frame_distribution(struct sk_buff *skb, struct net_device *dev);
extern int lac_set_min_mac_address(struct net_device *dev);

/*lac_ioctl.c*/
extern int lac_ioctl_deviceless_stub(unsigned int cmd, void __user *uarg);
extern int lac_dev_ioctl(struct net_device *dev, lac_cmd_t user_cmd,char *if_name,int command);
extern int lac_add_aggr(const char *name);
extern int lac_del_aggr(const char *name);
extern int add_del_if_by_name(Lac_aggr *lag, char* ifname, int isadd);
extern int lac_min_mtu(const struct net_lac_aggr *agg);

/*lac_if.c*/
extern int lac_add_if(struct net_lac_aggr *agg, struct net_device *dev);
extern int lac_del_if(struct net_device *dev);
extern void lac_free_aggrs(void);
extern void lac_free_ports(void);

extern Boolean lac_get_phy_info(struct net_device *dev, int *port_link, int *port_duplx, int *port_speed);


/*lac_input.c*/
extern int lac_handle_frame(struct net_lac_port *port, struct sk_buff **pskb);

/* lac_util.c */

extern struct semaphore		thread_sem;
extern struct completion	thread_comp;

extern int					thread_monitor_links(void *arg);
extern int					thread_stop;

extern struct timer_list	sys_timer;	/* system timer */
extern La_Timer				first_timer;
extern La_Timer				*tick_next;

extern void		lac_check_all_links(void);
extern void		lac_link_check(void *arg);
extern int		lac_set_aggregable(unsigned int port_no, Boolean aggregate);

extern void		lac_start_timer(La_Timer *lac_timer, Time exp_time);
extern void		lac_stop_timer(La_Timer *lac_timer);
extern void		lac_tick_timer(La_Timer *lac_timer);
extern Boolean	lac_is_timer_active(La_Timer *lac_timer);
extern void		lac_register_timers(Lac_port *port);
extern void		lac_unregister_timers(Lac_port *port);
extern void		lac_global_tick(unsigned long param);

extern int		lac_set_sys_coll_delay(unsigned int value);
extern int		lac_set_sys_priority(System_priority pri);
extern int		lac_set_timeout(unsigned int port_no, Boolean timeout);
extern int		lac_set_mode(unsigned int port_no, Boolean mode);
extern int		lac_set_port_priority(unsigned int port_no, unsigned short priority);
extern int		lac_set_admin_key(unsigned int port_no, La_Key key);

/* lac_notify.c */
extern struct notifier_block lac_device_notifier;

enum
{
	ERR_PORT_NOT_FOUND = 1,
	ERR_ALREADY_SET
};

#endif
