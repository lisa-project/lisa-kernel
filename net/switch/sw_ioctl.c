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
#include <net/sock.h>

#include "sw_private.h"

inline void dump_mem(void *m, int len) {
	int j;
	char buf[65];
	unsigned char *mem= m;

	while(len) {
		for(j = 0; j < 32 &&len; j++, len--) {
			sprintf(buf + 2 * j, "%02hx", *mem);
			mem++;
		}
		dbg("bmp: %s\n", buf);
	}
}

/* Set a forbidden vlan mask to allow the default vlans */
static inline void __sw_allow_default_vlans(unsigned char *forbidden_vlans) {
	sw_allow_vlan(forbidden_vlans, 1);
	sw_allow_vlan(forbidden_vlans, 1002);
	sw_allow_vlan(forbidden_vlans, 1003);
	sw_allow_vlan(forbidden_vlans, 1004);
	sw_allow_vlan(forbidden_vlans, 1005);
}

/* Effectively remove a port from all allowed vlans in a bitmap of
   forbidden vlans.
 */
static inline void __sw_remove_from_vlans(struct net_switch_port *port) {
	int n, vlan = 0;
	unsigned char mask, *bmp = port->forbidden_vlans;
	for(n = 0; n < SW_VLAN_BMP_NO; n++, bmp++) {
		for(mask = 1; mask; mask <<= 1, vlan++) {
			if(*bmp & mask)
				continue;
			sw_vdb_del_port(vlan, port);
		}
	}
}

/* Add an interface to the switch. The switch configuration mutex must
   be acquired from outside.
 */
static int sw_addif(struct net_device *dev) {
	struct net_switch_port *port;

	if (sw_is_vif(dev))
		return -EINVAL;

	if(rcu_dereference(dev->sw_port) != NULL) {
		/* dev->sw_port shouldn't be changed elsewhere, so
		   we don't necessarily need rcu_dereference here
		 */
		return -EBUSY;
	}
	if((port = kzalloc(sizeof(struct net_switch_port), GFP_KERNEL)) == NULL)
		return -ENOMEM;
	if((port->mrouters = kzalloc(SW_VLAN_BMP_NO, GFP_KERNEL)) == NULL) {
		kfree(port);
		return -ENOMEM;
	}
	if((port->forbidden_vlans = kzalloc(SW_VLAN_BMP_NO, GFP_KERNEL)) == NULL) {
		kfree(port->mrouters);
		kfree(port);
		return -ENOMEM;
	}
	port->dev = dev;
	port->sw = &sw;
    port->vlan = 1; /* By default all ports are in vlan 1 */
	port->desc[0] = '\0';
	port->flags = SW_PFL_DISABLED;
	INIT_LIST_HEAD(&port->sock_cdp);
	INIT_LIST_HEAD(&port->sock_vtp);
#ifdef NET_SWITCH_TRUNKDEFAULTVLANS
	memset(port->forbidden_vlans, 0xff, SW_VLAN_BMP_NO);
	__sw_allow_default_vlans(port->forbidden_vlans);
#else
	sw_forbid_vlan(port->forbidden_vlans, 0);
	sw_forbid_vlan(port->forbidden_vlans, 4095);
#endif
    sw_vdb_add_port(1, port);
	list_add_tail_rcu(&port->lh, &sw.ports);
	rcu_assign_pointer(dev->sw_port, port);
	dev_hold(dev);
	rtnl_lock();
	dev_set_promiscuity(dev, 1);
	rtnl_unlock();
	sw_enable_port(port);
	dbg("Added device %s to switch\n", dev->name);
	return 0;
}

/* Remove an interface from the switch. Appropriate locks must be held
   from outside to ensure that nobody tries to remove the same interface
   at the same time.
 */
int sw_delif(struct net_device *dev) {
	struct net_switch_port *port;
	int do_unlock;

	if((port = rcu_dereference(dev->sw_port)) == NULL)
		return -EINVAL;

	/* First disable promiscuous mode, so that there be less chances to
	   still receive packets on this port
	 */
	do_unlock = rtnl_trylock();
	dev_set_promiscuity(dev, -1);
	if (do_unlock)
		rtnl_unlock();

	/* Now let all incoming queue processors know that frames on this port
	   are not handled by the switch anymore.
	 */
	rcu_assign_pointer(dev->sw_port, NULL);
	/* dev->sw_port is now NULL, so no instance of sw_handle_frame() will
	   process incoming packets on this port.

	   However, at the time we changed the pointer there might have been
	   instances of sw_handle_frame() that were processing incoming
	   packets on this port. Frame processing can add entries to the fdb
	   that reference this port, so we have to wait for all running
	   instances to finish.
	 */
	synchronize_sched();
	/* Now nobody can add references to this port, so we can safely clean
	   up all existing references from the fdb
	 */
	fdb_cleanup_port(port, SW_FDB_ANY);
	/* Clean up vlan references: if the port was non-trunk, remove it from
	   its single vlan; otherwise use the disallowed vlans bitmap to remove
	   it from all vlans
	 */
	if(port->flags & SW_PFL_TRUNK) {
		__sw_remove_from_vlans(port);
	} else {
		sw_vdb_del_port(port->vlan, port);
	}
	list_del_rcu(&port->lh);
	/* free port memory and release interface */
	kfree(port->mrouters);
	kfree(port->forbidden_vlans);
	kfree(port);
	dev_put(dev);
	dbg("Removed device %s from switch\n", dev->name);
	return 0;
}

/* Effectively add a port to all allowed vlans in a bitmap of
   forbidden vlans.
 */
static inline void __sw_add_to_vlans(struct net_switch_port *port) {
	int n, vlan = 0;
	unsigned char mask, *bmp = port->forbidden_vlans;
	for(n = 0; n < SW_VLAN_BMP_NO; n++, bmp++) {
		for(mask = 1; mask; mask <<= 1, vlan++) {
			if(*bmp & mask)
				continue;
			sw_vdb_add_port(vlan, port);
		}
	}
}

/* Set a port's trunk mode and make appropriate changes to the
   vlan database.
 */
static int sw_set_port_trunk(struct net_switch_port *port, int trunk) {
	int status;

	if (!port)
		return -EINVAL;
	if (port->flags & SW_PFL_NOSWITCHPORT)
		return -EACCES;

	if(port->flags & SW_PFL_TRUNK) {
		if(trunk)
			return -EEXIST;
		sw_set_port_flag_rcu(port, SW_PFL_DROPALL);
		__sw_remove_from_vlans(port);
		sw_res_port_flag(port, SW_PFL_TRUNK);
		fdb_cleanup_port(port, SW_FDB_MAC_DYNAMIC);
		status = sw_vdb_add_port(port->vlan, port);
#if NET_SWITCH_NOVLANFORIF == 2
		if(status)
			sw_disable_port(port);
		/* FIXME FIXME FIXME
		 * i think CDP packets should still be received even if this
		 * happens. maybe we do not need to put the device down, but
		 * just set a special flag to drop all switched packets.
		 * this applies to all places where NET_SWITCH_NOVLANFORIF
		 * is tested
		 */
#endif
		sw_res_port_flag(port, SW_PFL_DROPALL);
	} else {
		if(!trunk)
			return -EEXIST;
		sw_set_port_flag_rcu(port, SW_PFL_DROPALL);
		sw_vdb_del_port(port->vlan, port);
		sw_set_port_flag(port, SW_PFL_TRUNK);
		sw_res_port_flag(port, SW_PFL_ACCESS);
		fdb_cleanup_port(port, SW_FDB_MAC_DYNAMIC);
		__sw_add_to_vlans(port);
		/* Make sure it was not disabled by assigning a non-existent vlan */
		sw_enable_port(port);
		sw_res_port_flag(port, SW_PFL_DROPALL);
	}
	return 0;
}

static int sw_set_port_access(struct net_switch_port *port, int access) {
	int status;

	if (!port)
		return -EINVAL;
	if (port->flags & SW_PFL_NOSWITCHPORT)
		return -EACCES;

	if(access) {
		/* cmd: switchport mode access */
		if((status = sw_set_port_trunk(port, 0)))
			return status;
		sw_set_port_flag(port, SW_PFL_ACCESS);
	} else {
		/* cmd: no switchport mode access
		 * Just reset the flag. Note that if we are in trunk mode,
		 * then "no switchport mode access" leaves us in trunk mode.
		 * Just resetting the flag is ok regardless of the current
		 * access mode.
		 */
		sw_res_port_flag(port, SW_PFL_ACCESS);
	}
	return 0;
}

static int sw_set_switchport(struct net_switch_port *port, int switchport) {
	int status;

	if (!port)
		return -EINVAL;
	if (port->flags & SW_PFL_NOSWITCHPORT) {
		/* port is routed and we change it to switched */
		if (!switchport)
			return -EEXIST;
		sw_set_port_flag_rcu(port, SW_PFL_DROPALL);
		if (port->flags & SW_PFL_TRUNK)
			__sw_add_to_vlans(port);
		else {
			status = sw_vdb_add_port(port->vlan, port);
#if NET_SWITCH_NOVLANFORIF == 2
			if(status)
				sw_disable_port(port);
#endif
		}
		sw_res_port_flag(port, SW_PFL_DROPALL);
	} else {
		/* port is switched and we change it to routed */
		if (switchport)
			return -EEXIST;
		sw_set_port_flag_rcu(port, SW_PFL_DROPALL);
		if (port->flags & SW_PFL_TRUNK)
	        __sw_remove_from_vlans(port);
		else
			sw_vdb_del_port(port->vlan, port);
		fdb_cleanup_port(port, SW_FDB_MAC_DYNAMIC);
		/* FIXME FIXME FIXME
		   - scoaterea portului din vlan-uri opreste flood-ul catre
		     portul respectiv
		   - stergerea mac-urilor dinamice opreste unicast catre
		     portul respectiv
		   - RAMANE UNICAST pe MAC STATIC!!! - la unicast ar trebui
		     pusa si conditia de switched port inainte de a trimite
			 pachetul atunci cand am gasit un mac care se potriveste
		   - nu e clarificat multicast-ul -- ramane de vazut cum se
		     va implementa igmp snooping
		 */
		sw_set_port_flag(port, SW_PFL_NOSWITCHPORT);
		/* Make sure it was not disabled by assigning a non-existent vlan */
		sw_enable_port(port);
		sw_res_port_flag(port, SW_PFL_DROPALL);
	}
	return 0;
}

/* Change a port's bitmap of forbidden vlans and, if necessary,
   make appropriate changes to the vlan database.
 */
static int sw_set_port_forbidden_vlans(struct net_switch_port *port,
		unsigned char *forbidden_vlans) {
	unsigned char *new = forbidden_vlans;
	unsigned char *old = port->forbidden_vlans;
	unsigned char mask;
	int n, vlan = 0;

	if (!port)
		return -EINVAL;
	if (port->flags & SW_PFL_NOSWITCHPORT)
		return -EACCES;

#ifdef NET_SWITCH_TRUNKDEFAULTVLANS
	__sw_allow_default_vlans(forbidden_vlans);
#endif
	/* FIXME hardcoded 0 and 4095; normally we should forbid
	   all vlans below SW_VLAN_MIN and above SW_VLAN_MAX
	 */
	sw_forbid_vlan(forbidden_vlans, 0);
	sw_forbid_vlan(forbidden_vlans, 4095);
	if(port->flags & SW_PFL_TRUNK) {
		for(n = 0; n < SW_VLAN_BMP_NO; n++, old++, new++) {
			for(mask = 1; mask; mask <<= 1, vlan++) {
				if(!((*old ^ *new) & mask))
					continue;
				if(*new & mask)
					sw_vdb_del_port(vlan, port);
				else
					sw_vdb_add_port(vlan, port);
			}
		}
	}
	memcpy(port->forbidden_vlans, forbidden_vlans, SW_VLAN_BMP_NO);
	return 0;
}

/* Update a port's bitmap of forbidden vlans by allowing vlans from a
   given bitmap of forbidden vlans. If necessary, make the appropriate
   changes to the vlan database.
 */
static int sw_add_port_forbidden_vlans(struct net_switch_port *port,
		unsigned char *forbidden_vlans) {
	unsigned char bmp[SW_VLAN_BMP_NO];
	unsigned char *p = bmp;
	unsigned char *new = forbidden_vlans;
	unsigned char *old = port->forbidden_vlans;
	int n;

	if (!port)
		return -EINVAL;
	if (port->flags & SW_PFL_NOSWITCHPORT)
		return -EACCES;

	for(n = 0; n < SW_VLAN_BMP_NO; n++, old++, new++, p++)
		*p = *old & *new;
	return sw_set_port_forbidden_vlans(port, bmp);
}

/* Update a port's bitmap of forbidden vlans by disallowing those vlans
   that are allowed by a given bitmap (of forbidden vlans). If necessary,
   make the appropriate changes to the vlan database.
 */
static int sw_del_port_forbidden_vlans(struct net_switch_port *port,
		unsigned char *forbidden_vlans) {
	unsigned char bmp[SW_VLAN_BMP_NO];
	unsigned char *p = bmp;
	unsigned char *new = forbidden_vlans;
	unsigned char *old = port->forbidden_vlans;
	int n;

	if (!port)
		return -EINVAL;
	if (port->flags & SW_PFL_NOSWITCHPORT)
		return -EACCES;

	for(n = 0; n < SW_VLAN_BMP_NO; n++, old++, new++, p++)
		*p = *old | ~*new;
	return sw_set_port_forbidden_vlans(port, bmp);
}

static int __add_vlan_default(struct net_switch *sw, int vlan) {
	int status;

	if(sw_vdb_vlan_exists(sw, vlan))
		return 0;
	if((status = sw_vdb_add_vlan_default(sw, vlan)))
		return status;
	/* TODO Notification to userspace for the cli */
	return 0;
}

/* Change a port's non-trunk vlan and make appropriate changes to the vlan
   database if necessary.
 */
static int sw_set_port_vlan(struct net_switch_port *port, int vlan) {
	int status;

	if (!port)
		return -EINVAL;
	if (port->flags & SW_PFL_NOSWITCHPORT)
		return -EACCES;
	if(port->vlan == vlan)
		return 0;

	if(port->flags & SW_PFL_TRUNK) {
		port->vlan = vlan;
#if NET_SWITCH_NOVLANFORIF == 1
		__add_vlan_default(port->sw, vlan);
#endif
	} else {
		sw_set_port_flag_rcu(port, SW_PFL_DROPALL);
		sw_vdb_del_port(port->vlan, port);
		status = sw_vdb_add_port(vlan, port);
		if(status) {
#if NET_SWITCH_NOVLANFORIF == 1
			status = __add_vlan_default(port->sw, vlan);
			if(status) {
				port->vlan = vlan;
				smp_wmb();
				sw_res_port_flag(port, SW_PFL_DROPALL);
				return status;
			}
			status = sw_vdb_add_port(vlan, port);
#elif NET_SWITCH_NOVLANFORIF == 2
			sw_disable_port(port);
#endif
		}
		port->vlan = vlan;
		smp_wmb();
		sw_res_port_flag(port, SW_PFL_DROPALL);
	}
	return 0;
}

static int sw_get_mac_loop(int hash_pos, struct swcfgreq *arg,
		struct net_switch_port *port, int len) {
	struct net_switch_fdb_entry *entry;
	struct net_switch_mac mac;
	int cmp_mac = !is_null_mac(arg->ext.mac.addr);
	int vlan = arg->vlan;

	list_for_each_entry_rcu(entry, &sw.fdb[hash_pos].entries, lh) {
		if (cmp_mac && memcmp(arg->ext.mac.addr, entry->mac, ETH_ALEN))
			continue;
		if (vlan && vlan != entry->vlan)
			continue;
		if (port && port != entry->port)
			continue;
		if (arg->ext.mac.type != SW_FDB_ANY && arg->ext.mac.type != entry->type)
			continue;
		if (len + sizeof(struct net_switch_mac) > arg->buf.size) {
			dbg("sw_get_mac_loop: insufficient buffer space\n");
			len = -ENOMEM;
			break;
		}
		memcpy(mac.addr, entry->mac, ETH_ALEN);
		mac.type = entry->type;
		mac.vlan = entry->vlan;
		mac.ifindex = entry->port->dev->ifindex;
		rcu_read_unlock();
		if (copy_to_user(arg->buf.addr + len, &mac, sizeof(struct net_switch_mac))) {
			rcu_read_lock();
			dbg("copy_to_user failed (hash_pos=%d)\n", hash_pos);
			len = -EFAULT;
			break;
		}
		else 
			rcu_read_lock();
		len += sizeof(struct net_switch_mac);
	}

	return len;
}

static int sw_get_mac(struct swcfgreq *arg, struct net_switch_port *port) {
	int i, ret = 0;

	rcu_read_lock();
	if (!is_null_mac(arg->ext.mac.addr)) 
		ret = sw_get_mac_loop(sw_mac_hash(arg->ext.mac.addr), arg, port, ret);
	else 
		for (i=0; i<SW_HASH_SIZE; i++) {
			ret = sw_get_mac_loop(i, arg, port, ret);
			if (ret < 0)
				break;
		}
	rcu_read_unlock();

	return ret;
}

int sw_get_vdb(struct swcfgreq *arg, int vlan_id, char *vlan_desc) {
	int size = 0;
	struct net_switch_vdb entry;
	int vlan, min = SW_MIN_VLAN, max = SW_MAX_VLAN;

	if (vlan_id) {
		if (sw_invalid_vlan(vlan_id))
			return -EINVAL;
		min = max = vlan_id;
	}

	for(vlan = min; vlan <= max; vlan++) {
		rcu_read_lock();
		if(sw.vdb[vlan] == NULL) {
			rcu_read_unlock();
			continue;
		}
		if (vlan_desc && strcmp(vlan_desc, sw.vdb[vlan]->name)) {
			rcu_read_unlock();
			continue;
		}
		entry.vlan = vlan;
		strcpy(entry.name, sw.vdb[vlan]->name);
		rcu_read_unlock();
		push_to_user_buf(entry, arg, size);
	}

	return size;
}

int sw_getvlanif(struct swcfgreq *arg)
{
	int size = 0;
	int ifindex;
	struct net_switch_vdb_link *link;

	dbg("sw_getvlanif, vlan=%d\n", arg->vlan);

	if (sw_invalid_vlan(arg->vlan))
		return -EINVAL;
	if (sw.vdb[arg->vlan] == NULL)
		return -ENOENT;

	list_for_each_entry(link, &sw.vdb[arg->vlan]->non_trunk_ports, lh) {
		ifindex = link->port->dev->ifindex;
		push_to_user_buf(ifindex, arg, size);
	}

	return size;
}

int sw_getiflist(struct swcfgreq *arg)
{
	int size = 0;
	struct net_switch_dev entry;
	struct net_switch_port *port;

	if (arg->ext.switchport & (SW_IF_SWITCHED | SW_IF_ROUTED))
		list_for_each_entry(port, &sw.ports, lh) {
			entry.type = port->flags & SW_PFL_NOSWITCHPORT ?
				SW_IF_ROUTED : SW_IF_SWITCHED;
			if (!(entry.type & arg->ext.switchport))
				continue;
			strncpy(entry.name, port->dev->name, IFNAMSIZ);
			entry.ifindex = port->dev->ifindex;
			entry.vlan = 0;
			push_to_user_buf(entry, arg, size);
		}

	if (arg->ext.switchport & SW_IF_VIF) {
		int i;
		struct net_switch_vif_priv *vif_priv;

		for (i = 0; i < SW_VIF_HASH_SIZE; i++)
			list_for_each_entry(vif_priv, &sw.vif[i], lh) {
				port = &vif_priv->bogo_port;
				strncpy(entry.name, port->dev->name, IFNAMSIZ);
				entry.ifindex = port->dev->ifindex;
				entry.type = SW_IF_VIF;
				entry.vlan = port->vlan;
				push_to_user_buf(entry, arg, size);
			}
	}

	return size;
}

int sw_getmrouters(struct swcfgreq *arg)
{
	struct net_switch_port *port;
	struct net_switch_mrouter tmp;
	unsigned char mask;
	int i, size = 0;

	list_for_each_entry(port, &sw.ports, lh) {
		tmp.ifindex = port->dev->ifindex;
		for(i = 0; i < SW_VLAN_BMP_NO; i++) {
			if(!port->mrouters[i])
				continue;
			tmp.vlan = i*8;
			for(mask = 1; mask; mask <<= 1, tmp.vlan++) {
				if((port->mrouters[i] & mask)) {
					push_to_user_buf(tmp, arg, size);
					dbg("%s: adding (ifindex, vlan) = (%d, %d)\n",
							__func__, tmp.ifindex, tmp.vlan);
				}
			}
		}
	}
	dbg("%s: returning %d\n", __func__, size);
	return size;
}

#define DEV_GET if(1) {\
	if ((dev = dev_get_by_index(net, arg.ifindex)) == NULL) {\
		err = -ENODEV;\
		break;\
	}\
	do_put = 1;\
}

#define __PORT_GET if(1) {\
	port = rcu_dereference(dev->sw_port);\
	if(!port) {\
		err = -EINVAL;\
		break;\
	}\
}

#define PORT_GET if(1) {\
	DEV_GET;\
	__PORT_GET;\
}

/* Handle "deviceless" ioctls. These ioctls are not specific to a certain
   device; they control the switching engine as a whole.
 */
int sw_deviceless_ioctl(struct socket *sock, unsigned int cmd, void __user *uarg) {
	struct net_device *dev = NULL, *rdev;
	struct net_switch_port *port = NULL;
	struct swcfgreq arg;
	unsigned char bitmap[SW_VLAN_BMP_NO];
	int err = -EINVAL;
	int do_put = 0;
	unsigned long age_time;
	char vlan_desc[SW_MAX_VLAN_NAME+1];
	struct net *net = &init_net;

	if (cmd != SIOCSWCFG)
		return -ENOIOCTLCMD;

	if(!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	memset(bitmap, 0xFF, SW_VLAN_BMP_NO);

	switch(arg.cmd) {
	case SWCFG_ADDIF:
		DEV_GET;
		err = sw_addif(dev);
		break;
	case SWCFG_DELIF:
		DEV_GET;
		err = sw_delif(dev);
		break;
	case SWCFG_ADDVLAN:
		if (!strncpy_from_user(vlan_desc, arg.ext.vlan_desc, SW_MAX_VLAN_NAME)) {
			err = -EFAULT;
			break;
		}
		vlan_desc[SW_MAX_VLAN_NAME] = '\0';
		err = sw_vdb_add_vlan(&sw, arg.vlan, vlan_desc);
		break;
	case SWCFG_DELVLAN:
		if (sw_is_default_vlan(arg.vlan)) {
			err = -EACCES;
			break;
		}
		err = sw_vdb_del_vlan(&sw, arg.vlan);
		break;
	case SWCFG_RENAMEVLAN:
		if (!strncpy_from_user(vlan_desc, arg.ext.vlan_desc, SW_MAX_VLAN_NAME)) {
			err = -EFAULT;
			break;
		}
		vlan_desc[SW_MAX_VLAN_NAME] = '\0';
		err = sw_vdb_set_vlan_name(&sw, arg.vlan, vlan_desc);
		break;
	case SWCFG_ADDVLANPORT:
		DEV_GET;
		sw_allow_vlan(bitmap, arg.vlan);
		err = sw_add_port_forbidden_vlans(rcu_dereference(dev->sw_port), bitmap);
		break;
	case SWCFG_DELVLANPORT:
		DEV_GET;
		/* use sw_allow_vlan here because sw_del_port_forbidden_vlans
		   negates the mask
		 */
		sw_allow_vlan(bitmap, arg.vlan);	
		err = sw_del_port_forbidden_vlans(rcu_dereference(dev->sw_port), bitmap);
		break;
	case SWCFG_SETACCESS:
		DEV_GET;
		err = sw_set_port_access(rcu_dereference(dev->sw_port), arg.ext.access);
		break;
	case SWCFG_SETTRUNK:
		DEV_GET;
		err = sw_set_port_trunk(rcu_dereference(dev->sw_port), arg.ext.trunk);
		break;
	case SWCFG_SETPORTVLAN:	
		DEV_GET;
		err = sw_set_port_vlan(rcu_dereference(dev->sw_port), arg.vlan);	
		break;
	case SWCFG_CLEARMACINT:
		PORT_GET;
		fdb_cleanup_port(port, SW_FDB_MAC_DYNAMIC);
		err = 0;
		break;
	case SWCFG_SETAGETIME:
		/* FIXME use constants for arg.ext.nsec range */
		if (arg.ext.nsec < 10 || arg.ext.nsec > 1000000) { 
			err = -EINVAL;
			break;
		}
		atomic_set(&sw.fdb_age_time, arg.ext.nsec * HZ);
		err = 0;
		break;
	case SWCFG_GETAGETIME:
		age_time = atomic_read(&sw.fdb_age_time);
		arg.ext.nsec = age_time / HZ;
		err = 0;
		if(copy_to_user(uarg, &arg, sizeof(arg))) {
			err = -EFAULT;
			break;
		}
		break;
	case SWCFG_MACSTATIC:
		PORT_GET;
		/* FIXME: do we need to add multicast mac addresses from here ? */
		if (is_mcast_mac(arg.ext.mac.addr)) {
			err = -EINVAL;
			break;
		}
		err = fdb_learn(arg.ext.mac.addr, port, arg.vlan, SW_FDB_STATIC);
		break;
	case SWCFG_DELMACSTATIC:
		PORT_GET;
		if (is_null_mac(arg.ext.mac.addr)) {
			err = -EINVAL;
			break;
		}
		err = fdb_del(&sw, arg.ext.mac.addr, port, arg.vlan, SW_FDB_STATIC) ? 0 : -ENOENT;
		break;
	case SWCFG_ADDVIF:
		err = sw_vif_addif(&sw, arg.vlan, &rdev);
		if (!err || err == -EEXIST) {
			arg.ifindex = rdev->ifindex;
			err = copy_to_user(uarg, &arg, sizeof(arg)) ? -EFAULT : err;
		}
		if (!err)
			err = sw_vif_enable(rdev);
		break;
	case SWCFG_DELVIF:
		err = sw_vif_delif(&sw, arg.vlan);
		/* FIXME FIXME FIXME nu e un pic cam tarziu sa dam cu disable?
		if (!err)
			err = sw_vif_disable(&sw, arg.vlan);
		*/
		break;
	case SWCFG_DISABLE_IF:
		err = 0;
		DEV_GET;

		if (sw_is_vif(dev)) {
			sw_vif_disable(dev);
			break;
		}

		__PORT_GET;
		sw_set_port_flag(port, SW_PFL_ADMDISABLED);
		sw_disable_port(port);
		break;
	case SWCFG_ENABLE_IF:
		err = 0;
		DEV_GET;

		if (sw_is_vif(dev)) {
			sw_vif_enable(dev);
			break;
		}

		__PORT_GET;
		sw_res_port_flag(port, SW_PFL_ADMDISABLED);
		sw_enable_port(port);
		break;
	case SWCFG_ADDMROUTER:
		PORT_GET;
		err = sw_set_mrouter(port->mrouters, arg.vlan);
		break;
	case SWCFG_SETTRUNKVLANS:
		PORT_GET;
		if(copy_from_user(bitmap, arg.ext.bmp, SW_VLAN_BMP_NO)) {
			err = -EFAULT;
			break;
		}
		err = sw_set_port_forbidden_vlans(port, bitmap);
		break;
	case SWCFG_ADDTRUNKVLANS:
		PORT_GET;
		if(copy_from_user(bitmap, arg.ext.bmp, SW_VLAN_BMP_NO)) {
			err = -EFAULT;
			break;
		}
		err = sw_add_port_forbidden_vlans(port, bitmap);
		break;
	case SWCFG_DELTRUNKVLANS:
		PORT_GET;
		if(copy_from_user(bitmap, arg.ext.bmp, SW_VLAN_BMP_NO)) {
			err = -EFAULT;
			break;
		}
		err = sw_del_port_forbidden_vlans(port, bitmap);
		break;
	case SWCFG_SETIFDESC:
		PORT_GET;
		if(!strncpy_from_user(port->desc, arg.ext.iface_desc,
					SW_MAX_PORT_DESC)) {
			err = -EFAULT;
			break;
		}
		port->desc[SW_MAX_PORT_DESC] = '\0';
		err = 0;
		break;
	case SWCFG_GETIFCFG:
		PORT_GET;
		arg.ext.cfg.flags = port->flags;
		arg.ext.cfg.access_vlan = port->vlan;
		if(arg.ext.cfg.forbidden_vlans != NULL) {
			if(copy_to_user(arg.ext.cfg.forbidden_vlans,
						port->forbidden_vlans, SW_VLAN_BMP_NO)) {
				err = -EFAULT;
				break;
			}
		}
		if(arg.ext.cfg.description != NULL) {
			if(copy_to_user(arg.ext.cfg.description, port->desc,
						strlen(port->desc) + 1)) {
				err = -EFAULT;
				break;
			}
		}
		if(copy_to_user(uarg, &arg, sizeof(arg))) {
			err = -EFAULT;
			break;
		}
		err = 0;
		break;
	case SWCFG_GETIFTYPE:
		DEV_GET;
		do {
			if (sw_is_vif(dev)) {
				struct net_switch_vif_priv *priv = netdev_priv(dev);

				arg.ext.switchport = SW_IF_VIF;
				arg.vlan = priv->bogo_port.vlan;
				break;
			}
			port = rcu_dereference(dev->sw_port);
			if (!port) {
				arg.ext.switchport = SW_IF_NONE;
				break;
			}
			arg.ext.switchport = (port->flags & SW_PFL_NOSWITCHPORT) ?
				SW_IF_ROUTED : SW_IF_SWITCHED;
		} while (0);
		if (copy_to_user(uarg, &arg, sizeof(arg))) {
			err = -EFAULT;
			break;
		}
		err = 0;
		break;
	case SWCFG_GETMAC:
		/* the code in dev_new_index() is small and simple enough to
		 * figure out that interface indexes are never <= 0; thus
		 * using a value of 0 to disable filter-by-port is safe */
		if (arg.ifindex)
			PORT_GET;
		err = sw_get_mac(&arg, port);
		break;
	case SWCFG_DELMACDYN:
		/* the code in dev_new_index() is small and simple enough to
		 * figure out that interface indexes are never <= 0; thus
		 * using a value of 0 to disable filter-by-port is safe */
		if (arg.ifindex) 
			PORT_GET;
	
		if (port) {
			err = fdb_cleanup_port(port, SW_FDB_MAC_DYNAMIC);
			break;
		}	
		if (arg.vlan) {
			err = fdb_cleanup_vlan(&sw, arg.vlan, SW_FDB_MAC_DYNAMIC);
			break;
		}
		if (!is_null_mac(arg.ext.mac.addr))
			err = fdb_del(&sw, arg.ext.mac.addr, port, arg.vlan, SW_FDB_MAC_DYNAMIC);
		else 
			err = fdb_cleanup_by_type(&sw, SW_FDB_MAC_DYNAMIC);
		break;
	case SWCFG_GETVDB:
		if (arg.ext.vlan_desc != NULL &&
				!strncpy_from_user(vlan_desc, arg.ext.vlan_desc, SW_MAX_VLAN_NAME)) {
			err = -EFAULT;
			break;
		}
		vlan_desc[SW_MAX_VLAN_NAME] = '\0';
		err = sw_get_vdb(&arg, arg.vlan, arg.ext.vlan_desc == NULL ?
				NULL : vlan_desc);
		break;
	case SWCFG_SETSWPORT:
		PORT_GET;
		err = sw_set_switchport(port, arg.ext.switchport);
		break;
	case SWCFG_GETIFLIST:
		err = sw_getiflist(&arg);
		break;
	case SWCFG_GETVLANIFS:
		err = sw_getvlanif(&arg);
		break;
	case SWCFG_GETMROUTERS:
		err = sw_getmrouters(&arg);
		break;
	}

	if (do_put)
		dev_put(dev);

	return err;
}

#undef DEV_GET
#undef PORT_GET
