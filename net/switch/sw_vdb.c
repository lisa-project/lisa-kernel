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

/* Add a new vlan to the vlan database */
int sw_vdb_add_vlan(struct net_switch *sw, int vlan, char *name) {
	struct net_switch_vdb_entry *entry;
	struct net_switch_port *port;
	struct net_device *dev;

    if(sw_invalid_vlan(vlan))
        return -EINVAL;
    if(sw->vdb[vlan])
        return -EEXIST;
    if(!(entry = kmalloc(sizeof(struct net_switch_vdb_entry),
					GFP_ATOMIC))) {
        dbg("Out of memory while trying to add vlan %d\n", vlan);
        return -ENOMEM;
    }
	if(!(entry->name = kmalloc(SW_MAX_VLAN_NAME + 1, GFP_ATOMIC))) {
		kfree(entry);
		return -ENOMEM;
	}
	strncpy(entry->name, name, SW_MAX_VLAN_NAME);
    entry->name[SW_MAX_VLAN_NAME] = '\0';
    INIT_LIST_HEAD(&entry->trunk_ports);
    INIT_LIST_HEAD(&entry->non_trunk_ports);
	rcu_assign_pointer(sw->vdb[vlan], entry);

	/* Add all switched ports to this vlan, if it is configured
	 * on them.
	 */
	list_for_each_entry(port, &sw->ports, lh) {
		if(port->flags & SW_PFL_NOSWITCHPORT)
			continue;
		if(port->flags & SW_PFL_TRUNK) {
			if(sw_port_forbidden_vlan(port, vlan))
				continue;
		} else {
			if(port->vlan != vlan)
				continue;
			sw_enable_port(port);
		}
		sw_vdb_add_port(vlan, port);
	}

	if((dev = sw_vif_find(sw, vlan))) {
		struct net_switch_vif_priv *priv = netdev_priv(dev);
		sw_vdb_add_port(vlan, &priv->bogo_port);
	}

	return 0;
}

int sw_vdb_add_vlan_default(struct net_switch *sw, int vlan) {
	char buf[9];
	
    if(sw_invalid_vlan(vlan))
        return -EINVAL;
	/* TODO If we're vtp client, ignore this request and return */
	sprintf(buf, "VLAN%04d", vlan);
	return sw_vdb_add_vlan(sw, vlan, buf);
}

/* Remove a vlan from the vlan database */
int sw_vdb_del_vlan(struct net_switch *sw, int vlan) {
	struct net_switch_vdb_entry *entry;
	struct net_switch_vdb_link *link, *tmp;

	if(sw_invalid_vlan(vlan))
		return -EINVAL;
	if(!(entry = sw->vdb[vlan]))
		return -ENOENT;

	/* Disable all access mode ports that have this vlan set
	 * as the access vlan */
#if NET_SWITCH_NOVLANFORIF == 2
	list_for_each_entry(link, &entry->non_trunk_ports, lh) {
		if(link->port->flags & SW_PFL_NOSWITCHPORT)
			continue;
		sw_disable_port(link->port);
	}
#endif

	/* Unlink the entry from the vlan database, then wait
	 * until all sw_handle_frame() instances see the change.
	 * Now all sw_handle_frame() instances will drop the
	 * packets on this vlan during the sanity checks. Thus
	 * nobody learns macs on this vlan, so we can safely remove
	 * all entrues from the fdb.
	 */
	rcu_assign_pointer(sw->vdb[vlan], NULL);
	synchronize_sched();
	fdb_cleanup_vlan(sw, vlan, SW_FDB_ANY);

	/* Subsequent invocations of the forwarding code will "see" this
	 * VLAN as deleted and will not run on this VLAN. So we can safely
	 * free the link structures without further synchronization.
	 */
	list_for_each_entry_safe(link, tmp, &entry->trunk_ports, lh) {
		kmem_cache_free(sw->vdb_cache, link);
	}
	list_for_each_entry_safe(link, tmp, &entry->non_trunk_ports, lh) {
		kmem_cache_free(sw->vdb_cache, link);
	}
	kfree(entry->name);
	kfree(entry);

	return 0;
}

/* Rename a vlan */
int sw_vdb_set_vlan_name(struct net_switch *sw, int vlan, char *name) {
	struct net_switch_vdb_entry *entry;
	char *entry_name, *old_name;

    if(sw_invalid_vlan(vlan))
        return -EINVAL;
    if(!(entry = sw->vdb[vlan]))
        return -ENOENT;
	if(sw_is_default_vlan(vlan))
		return -EPERM;
    if(!(entry_name = kmalloc(SW_MAX_VLAN_NAME + 1, GFP_ATOMIC))) {
        dbg("Out of memory while trying to change vlan name%d\n", vlan);
        return -ENOMEM;
    }
	strncpy(entry_name, name, SW_MAX_VLAN_NAME);
    entry_name[SW_MAX_VLAN_NAME] = '\0';
	old_name = entry->name;
	rcu_assign_pointer(entry->name, entry_name);
	synchronize_sched();
	kfree(old_name);

	return 0;
}

/* Initialize the vlan database */
void sw_vdb_init(struct net_switch *sw) {
	memset(&sw->vdb, 0, sizeof(sw->vdb));
	sw->vdb_cache = kmem_cache_create("sw_vdb_cache",
			sizeof(struct net_switch_vdb_link),
			0, SLAB_HWCACHE_ALIGN, NULL, NULL);
	if(!sw->vdb_cache)
		return;
    sw_vdb_add_vlan(sw, 1, "default");
    sw_vdb_add_vlan(sw, 1002, "fddi-default");
    sw_vdb_add_vlan(sw, 1003, "trcrf-default");
    sw_vdb_add_vlan(sw, 1004, "fddinet-default");
    sw_vdb_add_vlan(sw, 1005, "trbrf-default");
}

/* Destroy the vlan database */
void sw_vdb_exit(struct net_switch *sw) {
	int vlan;

	for(vlan = SW_MIN_VLAN; vlan <= SW_MAX_VLAN; vlan++)
		sw_vdb_del_vlan(sw, vlan);
	kmem_cache_destroy(sw->vdb_cache);
}

/* Add a port to a vlan */
int sw_vdb_add_port(int vlan, struct net_switch_port *port) {
	struct net_switch *sw;
	struct net_switch_vdb_link *link;

    if(sw_invalid_vlan(vlan))
        return -EINVAL;
		
	if (!port) 
		return -EINVAL;	
		
	sw = port->sw; 
	
	if(!sw->vdb[vlan])
		return -ENOENT;
	/* The same port cannot be added twice to the same vlan because the only
	   way to add a port to a vlan is by changing the port's configuration.
	   Changing port configuration is mutually exclusive.
	 */
	link = kmem_cache_alloc(sw->vdb_cache, GFP_ATOMIC);
	if(!link) {
		dbg("Out of memory while adding port to vlan\n");
		return -ENOMEM;
	}
	link->port = port;
	smp_wmb();
	if(port->flags & SW_PFL_TRUNK) {
		list_add_tail_rcu(&link->lh, &sw->vdb[vlan]->trunk_ports);
	} else {
		list_add_tail_rcu(&link->lh, &sw->vdb[vlan]->non_trunk_ports);
	}
	dbg("vdb: Added port %s to vlan %d\n", port->dev->name, vlan);

	return 0;
}

/* Remove a port from a vlan */
int sw_vdb_del_port(int vlan, struct net_switch_port *port) {
	struct net_switch_vdb_link *link;
	struct list_head *lh;

    if(sw_invalid_vlan(vlan))
        return -EINVAL;
		
	if (!port) 
		return -EINVAL;	
		
	if(!port->sw->vdb[vlan])
		return -ENOENT;
		
	lh = (port->flags & SW_PFL_TRUNK) ?
		&port->sw->vdb[vlan]->trunk_ports :
		&port->sw->vdb[vlan]->non_trunk_ports;
	list_for_each_entry(link, lh, lh) {
		if(link->port == port) {
			list_del_rcu(&link->lh);
			synchronize_sched();
			kmem_cache_free(port->sw->vdb_cache, link);
			dbg("vdb: Removed port %s from vlan %d\n", port->dev->name, vlan);
			return 0;
		}
	}
	
	return -ENOENT;
}
