#ifndef _LINUX_IF_LAC_H
#define _LINUX_IF_LAC_H

#include <linux/types.h>


typedef struct lac_cmd
{
	//int command;			/* LACTL_XXX */   //NU MAI E FOLOSITA      ADI
	void *param;			/* command param - cfg_cmd */
} lac_cmd_t;


#include <linux/net_switch.h>

#define LACTL_VERSION 1

#define LAC_BLANK_ZONE 0x30 //ADI zona nedefinita intre comenzile LISA si LAC
enum 
{
	LACTL_GET_VERSION = (SWCFG_GETVDB+LAC_BLANK_ZONE),
	LACTL_GET_AGGRS,
	LACTL_IF,
	LACTL_ADD_IF,
	LACTL_DEL_IF,

	LACTL_SET_AGG_STATE,
	LACTL_SET_AGG_STATE_ENABLED,
	LACTL_SET_AGG_STATE_DISABLED,

	LACTL_SET_MODE,
	LACTL_SET_MODE_ACTIVE,
	LACTL_SET_MODE_PASSIVE,

	LACTL_SET_ADMIN_KEY,
	LACTL_SET_PORT_PRIO,
	
	LACTL_SET_TIMEOUT,
	LACTL_SET_SHORT_TIMEOUT,
	LACTL_SET_LONG_TIMEOUT,
	
	LACTL_SET_SYS_PRIO,
	LACTL_SET_SYS_MAX_DELAY,

	LACTL_DEBUG_PORT,
	LACTL_DEBUG_RX,
	LACTL_DEBUG_TX,
	LACTL_DEBUG_STATE,
	LACTL_DEBUG_WARN,

	LACTL_GET_PORTS,
	LACTL_GET_AGG,
	LACTL_GET_SYSTEM,
	LACTL_GET_STATS,
	LACTL_GET_DEBUG,
	LACTL_ADD_AGG,
	LACTL_REM_AGG,
	LACTL_GET_PORT_BY_NAME  //ADI
};

/* 
 *	General command
 */


/*
 *	Configure command
 */
typedef struct cfg_cmd
{
	union
	{
		char *port_name;
		unsigned int port_no;
	} port;
	unsigned int value;
} lac_cfg_cmd_t;

/*
 *	System params
 */
typedef struct sys_params
{
	unsigned short sys_prio;		/* system priority */
	unsigned char sys_id[6];		/* system MAC */
	int collector_max_delay;		/* system collector max delay */
} sys_params_t;

#define MAX_AGG_NAME	8
#define MAX_PORT_NAME	8

/*
 *	Port params
 */
typedef struct port_params
{
	unsigned int	port_no;
	char			port_name[MAX_PORT_NAME];
	unsigned char	port_status;				/* enabled/disabled */
	unsigned char	timeout;					/* short/long timeout */
	unsigned char	ai;							/* aggregable/individual */

	unsigned char	actor_id[6];
	unsigned short	actor_port_prio;
	unsigned short	actor_admin_key;
	unsigned short	actor_oper_key;
	unsigned char	actor_state;
	
	unsigned int	part_port_no;
	unsigned char	part_sys_id[6];
	unsigned short	part_port_prio;
	unsigned short	part_admin_key;
	unsigned short	part_oper_key;
	unsigned char	part_state;

	unsigned short	agg_no;
	char			agg_name[MAX_AGG_NAME];
} port_params_t;

/**
 *	Aggregator params	
 */
typedef struct agg_params
{
	unsigned short agg_no;
	char agg_name[MAX_AGG_NAME];
	unsigned short agg_key;
	int ready;
	int agg_distrib_ports;
	unsigned short nports;
} agg_params_t;


#ifdef __KERNEL__

#include <linux/netdevice.h>

extern int (*lac_handle_frame_hook)(struct net_lac_port *p, struct sk_buff **pskb);

#endif

#endif
