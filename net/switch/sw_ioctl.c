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
	for(n = 0; n < 512; n++, bmp++) {
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
	struct net_switch_vif_priv *priv; 
	int i;

	for (i=0; i<SW_VIF_HASH_SIZE; i++)
		list_for_each_entry(priv, &sw.vif[i], lh) {
			if (dev == priv->bogo_port.dev)
				return -EINVAL;
		}
			
	if(rcu_dereference(dev->sw_port) != NULL) {
		/* dev->sw_port shouldn't be changed elsewhere, so
		   we don't necessarily need rcu_dereference here
		 */
		return -EBUSY;
	}
	if((port = kmalloc(sizeof(struct net_switch_port), GFP_KERNEL)) == NULL)
		return -ENOMEM;
	memset(port, 0, sizeof(struct net_switch_port));
	if((port->forbidden_vlans = kmalloc(SW_VLAN_BMP_NO, GFP_KERNEL)) == NULL) {
		kfree(port);
		return -ENOMEM;
	}
	port->dev = dev;
	port->sw = &sw;
    port->vlan = 1; /* By default all ports are in vlan 1 */
	port->desc[0] = '\0';
	port->speed = SW_SPEED_AUTO;
	port->duplex = SW_DUPLEX_AUTO;
	port->flags = SW_PFL_DISABLED;
	INIT_LIST_HEAD(&port->sock_cdp);
	INIT_LIST_HEAD(&port->sock_vtp);
	/* FIXME configure physical characteristics of device (i.e. speed) */
#ifdef NET_SWITCH_TRUNKDEFAULTVLANS
	memset(port->forbidden_vlans, 0xff, 512);
	__sw_allow_default_vlans(port->forbidden_vlans);
#else
	memset(port->forbidden_vlans, 0, 512);
	sw_forbid_vlan(port->forbidden_vlans, 0);
	sw_forbid_vlan(port->forbidden_vlans, 4095);
#endif
    sw_vdb_add_port(1, port);
	list_add_tail_rcu(&port->lh, &sw.ports);
	rcu_assign_pointer(dev->sw_port, port);
	dev_hold(dev);
	dev_set_promiscuity(dev, 1);
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

	if((port = rcu_dereference(dev->sw_port)) == NULL)
		return -EINVAL;

	/* First disable promiscuous mode, so that there be less chances to
	   still receive packets on this port
	 */
	dev_set_promiscuity(dev, -1);
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
	for(n = 0; n < 512; n++, bmp++) {
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
		fdb_cleanup_port(port, SW_FDB_DYN);
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
		fdb_cleanup_port(port, SW_FDB_DYN);
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
		fdb_cleanup_port(port, SW_FDB_DYN);
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
		for(n = 0; n < 512; n++, old++, new++) {
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
	memcpy(port->forbidden_vlans, forbidden_vlans, 512);
	return 0;
}

/* Update a port's bitmap of forbidden vlans by allowing vlans from a
   given bitmap of forbidden vlans. If necessary, make the appropriate
   changes to the vlan database.
 */
static int sw_add_port_forbidden_vlans(struct net_switch_port *port,
		unsigned char *forbidden_vlans) {
	unsigned char bmp[512];
	unsigned char *p = bmp;
	unsigned char *new = forbidden_vlans;
	unsigned char *old = port->forbidden_vlans;
	int n;

	if (!port)
		return -EINVAL;
	if (port->flags & SW_PFL_NOSWITCHPORT)
		return -EACCES;

	for(n = 0; n < 512; n++, old++, new++, p++)
		*p = *old & *new;
	return sw_set_port_forbidden_vlans(port, bmp);
}

/* Update a port's bitmap of forbidden vlans by disallowing those vlans
   that are allowed by a given bitmap (of forbidden vlans). If necessary,
   make the appropriate changes to the vlan database.
 */
static int sw_del_port_forbidden_vlans(struct net_switch_port *port,
		unsigned char *forbidden_vlans) {
	unsigned char bmp[512];
	unsigned char *p = bmp;
	unsigned char *new = forbidden_vlans;
	unsigned char *old = port->forbidden_vlans;
	int n;

	if (!port)
		return -EINVAL;
	if (port->flags & SW_PFL_NOSWITCHPORT)
		return -EACCES;

	for(n = 0; n < 512; n++, old++, new++, p++)
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

static int sw_get_mac_loop(int hash_pos, struct net_switch_ioctl_arg *arg,
		struct net_switch_port *port, int len) {
	struct net_switch_fdb_entry *entry;
	struct net_switch_mac mac;
	struct net_switch_mac_arg *marg = &arg->ext.marg;
	int cmp_mac = !is_null_mac(marg->addr);
	int vlan = arg->vlan;

	list_for_each_entry_rcu(entry, &sw.fdb[hash_pos].entries, lh) {
		if (cmp_mac && memcmp(marg->addr, entry->mac, ETH_ALEN))
			continue;
		if (vlan && vlan != entry->vlan)
			continue;
		if (port && port != entry->port)
			continue;
		if (marg->addr_type != SW_FDB_ANY && marg->addr_type != entry->is_static)
			continue;
		if (len + sizeof(struct net_switch_mac) > marg->buf_size) {
			dbg("sw_get_mac_loop: insufficient buffer space\n");
			len = -ENOMEM;
			break;
		}
		memcpy(mac.addr, entry->mac, ETH_ALEN);
		mac.addr_type = entry->is_static;
		mac.vlan = entry->vlan;
		strncpy(mac.port, entry->port->dev->name, IFNAMSIZ);
		mac.port[IFNAMSIZ-1] = '\0';
		rcu_read_unlock();
		if (copy_to_user(marg->buf+len, &mac, sizeof(struct net_switch_mac))) {
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

static int sw_get_mac(struct net_switch_ioctl_arg *arg, struct net_switch_port *port) {
	int i, ret = 0;

	rcu_read_lock();
	if (!is_null_mac(arg->ext.marg.addr)) 
		ret = sw_get_mac_loop(sw_mac_hash(arg->ext.marg.addr), arg, port, ret);
	else 
		for (i=0; i<SW_HASH_SIZE; i++) {
			ret = sw_get_mac_loop(i, arg, port, ret);
			if (ret < 0)
				break;
		}
	rcu_read_unlock();

	return ret;
}

int sw_get_vdb(struct net_switch_ioctl_arg *arg) {
	char *buf = arg->ext.varg.buf;
	int entries = 0;
	char *buf_range = buf + arg->ext.varg.buf_size;
	struct net_switch_usr_vdb entry;
	int vlan;

	for(vlan = SW_MIN_VLAN; vlan <= SW_MAX_VLAN; vlan++) {
		rcu_read_lock();
		if(sw.vdb[vlan] == NULL) {
			rcu_read_unlock();
			continue;
		}
		entry.vlan = vlan;
		strcpy(entry.name, sw.vdb[vlan]->name);
		rcu_read_unlock();
		if(buf + sizeof(struct net_switch_usr_vdb) > buf_range)
			return -ENOMEM;
		if(copy_to_user(buf, &entry, sizeof(struct net_switch_usr_vdb)))
			return -EFAULT;
		buf += sizeof(struct net_switch_usr_vdb);
		entries++;
	}

	arg->ext.varg.vdb_entries = entries;
	return 0;
}

#define DEV_GET if(1) {\
	if(!strncpy_from_user(if_name, arg.if_name, IFNAMSIZ)) {\
		err = -EFAULT;\
		break;\
	}\
	if_name[IFNAMSIZ - 1] = '\0';\
	if((dev = dev_get_by_name(if_name)) == NULL) {\
		err = -ENODEV;\
		break;\
	}\
	do_put = 1;\
}

#define PORT_GET if(1) {\
	DEV_GET;\
	port = rcu_dereference(dev->sw_port);\
	if(!port) {\
		err = -EINVAL;\
		break;\
	}\
}

/* Handle "deviceless" ioctls. These ioctls are not specific to a certain
   device; they control the switching engine as a whole.
 */
int sw_deviceless_ioctl(unsigned int cmd, void __user *uarg) {
	struct net_device *dev = NULL;
	struct net_switch_port *port = NULL;
	struct net_switch_ioctl_arg arg;
	unsigned char bitmap[SW_VLAN_BMP_NO];
	char if_name[IFNAMSIZ];
	int err = -EINVAL;
	int do_put = 0;
	unsigned long age_time;
	char vlan_desc[SW_MAX_VLAN_NAME+1];

	if(!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (copy_from_user(&arg, uarg, sizeof(struct net_switch_ioctl_arg)))
		return -EFAULT;

	if(cmd != SIOCSWCFG)
		return -EINVAL;

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
		fdb_cleanup_port(port, SW_FDB_DYN);
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
		if(copy_to_user(uarg, &arg, sizeof(struct net_switch_ioctl_arg))) {
			err = -EFAULT;
			break;
		}
		break;
	case SWCFG_MACSTATIC:
		PORT_GET;
		err = fdb_learn(arg.ext.mac, port, arg.vlan, SW_FDB_STATIC,
				is_mcast_mac(arg.ext.mac));
		break;
	case SWCFG_DELMACSTATIC:
		PORT_GET;
		if (is_null_mac(arg.ext.mac)) {
			err = -EINVAL;
			break;
		}
		err = fdb_del(&sw, arg.ext.mac, port, arg.vlan, SW_FDB_STATIC) ? 0 : -ENOENT;
		break;
	case SWCFG_ADDVIF:
		err = sw_vif_addif(&sw, arg.vlan);
		if (!err)
			err = sw_vif_enable(&sw, arg.vlan);
		break;
	case SWCFG_DELVIF:
		err = sw_vif_delif(&sw, arg.vlan);
		if (!err)
			err = sw_vif_disable(&sw, arg.vlan);
		break;
	case SWCFG_DISABLEPORT:
		PORT_GET;
		sw_set_port_flag(port, SW_PFL_ADMDISABLED);
		sw_disable_port(port);
		err = 0;
		break;
	case SWCFG_ENABLEPORT:
		PORT_GET;
		sw_res_port_flag(port, SW_PFL_ADMDISABLED);
		sw_enable_port(port);
		err = 0;
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
	case SWCFG_SETSPEED:
		PORT_GET;
		port->speed = arg.ext.speed;
		err = 0;
		break;
	case SWCFG_SETDUPLEX:
		PORT_GET;
		port->duplex = arg.ext.duplex;
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
		arg.ext.cfg.speed = port->speed;
		arg.ext.cfg.duplex = port->duplex;
		if(copy_to_user(uarg, &arg, sizeof(struct net_switch_ioctl_arg))) {
			err = -EFAULT;
			break;
		}
		err = 0;
		break;
	case SWCFG_GETMAC:
		if (arg.if_name)
			PORT_GET;
		if (arg.ext.marg.buf_size < sizeof(struct net_switch_mac)) {
			err = -EINVAL;
			break;
		}	
		err = sw_get_mac(&arg, port);
		break;
	case SWCFG_DELMACDYN:
		if (arg.if_name) 
			PORT_GET;
	
		if (port) {
			err = fdb_cleanup_port(port, SW_FDB_DYN);
			break;
		}	
		if (arg.vlan) {
			err = fdb_cleanup_vlan(&sw, arg.vlan, SW_FDB_DYN);
			break;
		}
		if (!is_null_mac(arg.ext.mac))
			err = fdb_del(&sw, arg.ext.mac, port, arg.vlan, SW_FDB_DYN);
		else 
			err = fdb_cleanup_by_type(&sw, SW_FDB_DYN);
		break;
	case SWCFG_ENABLEVIF:
		err = sw_vif_enable(&sw, arg.vlan);
		break;
	case SWCFG_DISABLEVIF:
		err = sw_vif_disable(&sw, arg.vlan);
		break;
	case SWCFG_GETVDB:
		err = sw_get_vdb(&arg);
		if (copy_to_user(uarg, &arg, sizeof(struct net_switch_ioctl_arg)))
			err = -EFAULT;
		break;
	case SWCFG_SETSWPORT:
		PORT_GET;
		err = sw_set_switchport(port, arg.ext.switchport);
		break;
	}

	if (do_put)
		dev_put(dev);

	return err;
}

#undef DEV_GET
#undef PORT_GET
