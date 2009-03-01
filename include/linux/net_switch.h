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

#ifndef _NET_SWITCH_H
#define _NET_SWITCH_H

enum {
	/* Generic port manipulation */
	SWCFG_ADDIF,			/* add interface to switch */
	SWCFG_DELIF,			/* remove interface from switch */
	SWCFG_SETSWPORT,		/* set port type to switched (1) or routed (0) */
	SWCFG_GETIFLIST,		/* get a list of all known interfaces */

	/* Generic switched interface manipulation */
	SWCFG_SETIFDESC,		/* set interface description */
	SWCFG_SETSPEED,			/* set port speed parameter */
	SWCFG_SETDUPLEX,		/* set port duplex parameter */

	/* VDB manipulation */
	SWCFG_ADDVLAN,			/* add vlan to vlan database */
	SWCFG_DELVLAN,			/* delete vlan from vlan database */
	SWCFG_RENAMEVLAN,		/* rename vlan from vlan database */
	SWCFG_GETVDB,			/* copy the whole vlan database to userspace */

	/* Vlan-related switched interface manipulation */
	SWCFG_ADDVLANPORT,		/* add a port to a vlan (trunk mode) */
	SWCFG_DELVLANPORT ,		/* remove a port from a vlan (trunk mode) */
	SWCFG_SETACCESS,		/* put a port in access mode */
	SWCFG_SETTRUNK,			/* put a port in trunk mode */
	SWCFG_SETPORTVLAN ,		/* add a port in a vlan (non-trunk mode) */
	SWCFG_ADDTRUNKVLANS,	/* add ports to the bitmap of forbidden trunk ports */
	SWCFG_DELTRUNKVLANS,	/* remove ports from the bitmap of forbidden trunk ports */
	SWCFG_SETTRUNKVLANS,	/* set the bitmap of forbidden trunk ports */

	/* FDB manipulation */
	SWCFG_GETMAC,			/* fetch mac addresses from the fdb */
	SWCFG_GETAGETIME,		/* get fdb aging time interval */
	SWCFG_SETAGETIME,		/* set fdb entry aging time interval (in ms) */
	SWCFG_CLEARMACINT,		/* clear all macs for a given port */
	SWCFG_MACSTATIC,		/* add static mac */
	SWCFG_DELMACSTATIC,		/* delete static mac */
	SWCFG_DELMACDYN,		/* clear dynamic mac addresses from the fdb */

	/* Generic interface functions */
	SWCFG_GETIFCFG,			/* get physical port configuration and status */
	SWCFG_GETIFTYPE,		/* determine interface relation to switch */
	SWCFG_DISABLE_IF,		/* administratively disable port or vif */
	SWCFG_ENABLE_IF,		/* enable port or vif */

	/* VIF manipulation */
	SWCFG_ADDVIF,			/* add virtual interface for vlan */
	SWCFG_DELVIF,			/* remove virtual interface for vlan */
};

enum {
	SW_IF_NONE		= 0x00,	/* interface is not related to switch */
	SW_IF_SWITCHED	= 0x01,	/* interface is a standard switched port */
	SW_IF_ROUTED	= 0x02,	/* interface is registered to switch, but routed */
	SW_IF_VIF		= 0x04	/* interface is lisa vlan virtual interface */
};

#define SW_PFL_DISABLED     0x01
#define SW_PFL_ACCESS		0x02
#define SW_PFL_TRUNK		0x04
#define SW_PFL_DROPALL		0x08
#define SW_PFL_ADMDISABLED	0x10
#define SW_PFL_NOSWITCHPORT	0x20

#define SW_SPEED_AUTO		0x01
#define SW_SPEED_10			0x02
#define SW_SPEED_100		0x03
#define SW_SPEED_1000		0x04

#define SW_DUPLEX_AUTO		0x01
#define SW_DUPLEX_HALF		0x02
#define SW_DUPLEX_FULL		0x03

#ifdef __KERNEL__
#include <linux/if.h>
#else
#ifndef _LINUX_IF_H
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/if.h>
#include <linux/if_ether.h>

#endif
#endif

struct net_switch_ifcfg {
	int flags;
	int access_vlan;
	unsigned char *forbidden_vlans;
	char *description;
	int speed;
	int duplex;
};

/**
 * FDB query result.
 *
 * Only used to fill userspace buffer on ioctl() queries.
 */
struct net_switch_mac {
	unsigned char addr[ETH_ALEN];
	int type;
	int vlan;
	int ifindex;
};

#define SW_DEFAULT_AGE_TIME 300

#define SW_MAX_VLAN_NAME	31

/**
 * VDB query result.
 *
 * Only used to fill userspace buffer on ioctl() queries.
 */
struct net_switch_vdb {
	int vlan;
	char name[SW_MAX_VLAN_NAME + 1];
};

/**
 * Interface list query result
 *
 * Only used to fill userspace buffer on ioctl() queries.
 */
struct net_switch_dev {
	char name[IFNAMSIZ];
	int ifindex;
	int type;
	int vlan; /* only used for VIFs */
};

struct swcfgreq {
	int cmd;
	int ifindex;
	int vlan;
	union {
		int access;
		int trunk;
		int nsec;
		unsigned char *bmp;
		char *vlan_desc;
		char *iface_desc;
		int speed;
		int duplex;
		struct net_switch_ifcfg cfg;
		struct {
			unsigned char addr[ETH_ALEN];
			int type;
		} mac;
		int switchport;
	} ext;

	/* Userspace supplied buffer for dumping large data such as
	 * mac list or vdb entry list */
	struct {
		char *addr;
		int size;
	} buf;
};

/* Mac Address types (any, static, dynamic) */
#define SW_FDB_DYN	0
#define SW_FDB_STATIC 1
#define SW_FDB_ANY 2

/* Minimum number a vlan may have */
#define SW_MIN_VLAN 1

/* Maximum number a vlan may have. Note that vlan-related vectors
   must be at least SW_MAX_VLAN + 1 sized, because SW_MAX_VLAN
   should be a valid index.
 */
#define SW_MAX_VLAN 4094

/* Number of octet bitmaps that are necessary to store binary
   information about vlans (i.e. allowed or forbidden on a certain
   port).
 */
#define SW_VLAN_BMP_NO (SW_MAX_VLAN / 8 + 1)

#define sw_valid_vlan(vlan) \
	((vlan) >= SW_MIN_VLAN && (vlan) <= SW_MAX_VLAN)
#define sw_invalid_vlan(vlan) \
	((vlan) < SW_MIN_VLAN || (vlan) > SW_MAX_VLAN)
#define sw_is_default_vlan(vlan) \
	((vlan) == 1 || ((vlan) >= 1002 && (vlan) <= 1005))

#define sw_allow_vlan(bitmap, vlan) ((bitmap)[(vlan) / 8] &= ~(1 << ((vlan) % 8)))
#define sw_forbid_vlan(bitmap, vlan) ((bitmap)[(vlan) / 8] |= (1 << ((vlan) % 8)))
#define sw_forbidden_vlan(bitmap, vlan) ((bitmap)[(vlan) / 8] & (1 << ((vlan) % 8)))
#define sw_allowed_vlan(bitmap, vlan) (!sw_forbidden_vlan(bitmap, vlan))

/* Maximum length of port description */
#define SW_MAX_PORT_DESC	31

#define is_mcast_mac(ptr) \
	((ptr)[0] == 0x01 && (ptr)[1] == 0x00 && (ptr)[2] == 0x5e)
#define is_l2_mac(ptr) \
	((ptr)[0] == 0x01 && (ptr)[1] == 0x80 && (ptr)[2] == 0xc2)
#define is_null_mac(ptr) \
	(((ptr)[0] | (ptr)[1] | (ptr)[2] | (ptr)[3] | (ptr)[4] | (ptr)[5]) == 0)
#define is_bcast_mac(ptr) \
	(((ptr)[0] & (ptr)[1] & (ptr)[2] & (ptr)[3] & (ptr)[4] & (ptr)[5]) == 0xff)

/* Dummy "ethernet" protocol types (in addition to those defined in
 * include/linux/if_ether.h.
 *
 * We use these for our custom protocol family implementation. They
 * have no meaning whatsoever with respect to the protocol field of
 * ethernet frames.
 */
#define ETH_P_CDP	0x0020
#define ETH_P_VTP	0x0021
#define ETH_P_STP	0x0022

struct sockaddr_sw {
	unsigned short			ssw_family;
	char					ssw_if_name[IFNAMSIZ];
	int						ssw_proto;
};

#endif
