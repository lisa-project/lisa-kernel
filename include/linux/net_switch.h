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

#define SWCFG_ADDIF			0x01	/* add interface to switch */
#define SWCFG_DELIF			0x02	/* remove interface from switch */
#define SWCFG_ADDVLAN		0x03	/* add vlan to vlan database */
#define SWCFG_DELVLAN		0x04	/* delete vlan from vlan database */
#define SWCFG_RENAMEVLAN	0x05	/* rename vlan from vlan database */
#define SWCFG_ADDVLANPORT	0x06	/* add a port to a vlan (trunk mode) */
#define SWCFG_DELVLANPORT 	0x07	/* remove a port from a vlan (trunk mode) */
#define SWCFG_SETACCESS		0x08	/* put a port in access mode */
#define SWCFG_SETTRUNK		0x09	/* put a port in trunk mode */
#define SWCFG_SETPORTVLAN 	0x0a	/* add a port in a vlan (non-trunk mode) */
#define SWCFG_CLEARMACINT	0x0b	/* clear all macs for a given port */
#define SWCFG_SETAGETIME	0x0c	/* set fdb entry aging time interval (in ms) */
#define SWCFG_MACSTATIC		0x0d	/* add static mac */
#define SWCFG_DELMACSTATIC	0x0e	/* delete static mac */
#define SWCFG_GETIFCFG		0x0f	/* get physical port configuration and status */
#define SWCFG_ADDVIF		0x10	/* add virtual interface for vlan */
#define SWCFG_DELVIF		0x11	/* remove virtual interface for vlan */
#define SWCFG_DISABLEPORT	0x12	/* administratively disable port */
#define SWCFG_ENABLEPORT	0x13	/* enable port */
#define SWCFG_SETTRUNKVLANS	0x14	/* set the bitmap of forbidden trunk ports */
#define SWCFG_ADDTRUNKVLANS	0x15	/* add ports to the bitmap of forbidden trunk ports */
#define SWCFG_DELTRUNKVLANS	0x16	/* remove ports from the bitmap of forbidden trunk ports */
#define SWCFG_SETIFDESC		0x17	/* set interface description */
#define SWCFG_SETSPEED		0x18	/* set port speed parameter */
#define SWCFG_SETDUPLEX		0x19	/* set port duplex parameter */
#define SWCFG_GETMAC		0x20	/* fetch mac addresses from the fdb */
#define SWCFG_DELMACDYN		0x21	/* clear dynamic mac addresses from the fdb */
#define SWCFG_ENABLEVIF		0x22	/* administratively enable virtual interface */
#define SWCFG_DISABLEVIF	0x23	/* administratively disable virtual interface */
#define SWCFG_GETAGETIME	0x24	/* get fdb aging time interval */
#define SWCFG_GETVDB		0x30	/* copy the whole vlan database to userspace */
#define SWCFG_SETSWPORT		0x40	/* set port type to switched (1) or routed (0) */

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

struct net_switch_mac {
	unsigned char addr[ETH_ALEN];
	int addr_type;
	int vlan;
	char port[IFNAMSIZ];
};

struct net_switch_mac_arg {
	unsigned char addr[ETH_ALEN];
	int buf_size;
	int actual_size;
	int addr_type;
	char *buf;
};

#define SW_DEFAULT_AGE_TIME 300

#define SW_MAX_VLAN_NAME	31

struct net_switch_usr_vdb {
	int vlan;
	char name[SW_MAX_VLAN_NAME + 1];
};

struct net_switch_usr_vdb_arg {
	int buf_size;
	int vdb_entries;
	char *buf;
};

struct net_switch_ioctl_arg {
	unsigned char cmd;
	char *if_name;
	int vlan;
	union {
		int access;
		int trunk;
		int nsec;
		unsigned char *mac;
		unsigned char *bmp;
		char *vlan_desc;
		char *iface_desc;
		int speed;
		int duplex;
		struct net_switch_ifcfg cfg;
		struct net_switch_mac_arg marg;
		struct net_switch_usr_vdb_arg varg;
		int switchport;
	} ext;
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
