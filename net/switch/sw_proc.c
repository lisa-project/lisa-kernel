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

#include <linux/proc_fs.h>

#include "sw_private.h"
#include "sw_proc.h"

#define SCR_RIGHT_MAX 70
#define INITIAL_OFFSET 22

static struct proc_dir_entry *switch_dir, *iface_file,
							 *mac_file, *vlan_file, *vif_file;


static int read_vlan_bitmap(char *page, struct net_switch_port *port, int initial_offset) {
	int i, vlan, min, max, mask, count;
	int enabled=0;
	int offset = initial_offset;
	
	min = max = vlan = count = 0;
	
	for (i=0; i<SW_VLAN_BMP_NO; i++) {
		for (mask=0x01; mask < 0x100; mask<<=1, vlan++) {
			if ((port->forbidden_vlans[i] & mask) && enabled) {
				if (offset > initial_offset) offset+=sprintf(page+count+offset, ",");
				if (offset > SCR_RIGHT_MAX) {
					count+=offset;
					count+=sprintf(page+count, "\n");
					for (offset=0; offset<initial_offset; )
						offset+=sprintf(page+count+offset," ");
				}
				if (max - min > 1) 
					offset += sprintf(page+count+offset, "%d-%d", min, max);
				else if (max - min == 1) 
					offset += sprintf(page+count+offset, "%d,%d", min, max);
				else 
					offset += sprintf(page+count+offset, "%d", min);
				enabled = 0;
			}
			else if (! (port->forbidden_vlans[i] & mask)) {
				if (!enabled) {
					min = max = vlan;
					enabled = 1;
				}
				else {
					max = vlan;
				}
			}
		}
	}

	if (! (port->forbidden_vlans[SW_VLAN_BMP_NO-1] & 0x80)) {
		if (offset > initial_offset) offset+=sprintf(page+count+offset, ",");
		if (offset > SCR_RIGHT_MAX) {
			count+=offset;
			count+=sprintf(page+count, "\n");
			for (offset=0; offset<initial_offset;)
				offset+=sprintf(page+count+offset," ");
		}
		if (max - min > 1) 
			offset += sprintf(page+count+offset, "%d-%d", min, max);
		else if (max - min == 1)
			offset += sprintf(page+count+offset, "%d,%d", min, max);
		else 
			offset += sprintf(page+count+offset, "%d", min);
	}

	count+=offset;

	return count;
}

static int proc_read_ifaces(char *page, char **start,
		off_t off, int count,
		int *eof, void *data) {
		
	struct net_switch_port *port;
	int len = 0;
	
	len += sprintf(page, "Port  Trunk  Enabled  VLAN\n"
		"----  -----  -------  ----\n");

	rcu_read_lock();
	list_for_each_entry(port, &sw.ports, lh) {
		len+= sprintf(page+len, "%4s  %5d  %7d  ",
			port->dev->name, (port->flags & SW_PFL_TRUNK)?1:0, 
			!(port->flags & SW_PFL_DISABLED));
		if (port->flags & SW_PFL_TRUNK) {
			len += read_vlan_bitmap(page+len-INITIAL_OFFSET, port, INITIAL_OFFSET);
			len -= INITIAL_OFFSET;
			len += sprintf(page+len, "\n");
		}
		else 
			len += sprintf(page+len, "%-4d\n", port->vlan);
	}
	rcu_read_unlock();
	return len;
}

static int proc_read_mac(char *page, char **start,
		off_t off, int count,
		int *eof, void *data) {
		
	struct net_switch_fdb_entry *entry;	
	int len = 0;
	int i;

	len += sprintf(page, "Destination Address  Address Type  VLAN  Destination Port\n"
		"-------------------  ------------  ----  ----------------\n");
	rcu_read_lock();
	for (i=0; i<SW_HASH_SIZE; i++) {
		list_for_each_entry_rcu(entry, &sw.fdb[i].entries, lh) {
			len+=sprintf(page+len, "%02x%02x.%02x%02x.%02x%02x       "
				"%12s  %4d  %s\n",
				entry->mac[0], entry->mac[1], entry->mac[2],
				entry->mac[3], entry->mac[4], entry->mac[5],
				(entry->is_static)? "Static": "Dynamic",
				entry->vlan,
				entry->port->dev->name
				);
		}
	}
	rcu_read_unlock();
	
	return len;
}

static int proc_read_vlan(char *page, char **start,
		off_t off, int count,
		int *eof, void *data) {
		
	int len, vlan;
	struct net_switch_vdb_link *link;
	
	len = sprintf(page, "VLAN Name                             Status    Ports\n");
	len += sprintf(page+len, "---- -------------------------------- --------- "
		"-------------------------------\n");
	
	rcu_read_lock();

	for (vlan = SW_MIN_VLAN; vlan <= SW_MAX_VLAN; vlan++) {
		if (! sw.vdb[vlan]) continue;
		len += sprintf(page+len, "%-4d %-32s active    ", vlan, sw.vdb[vlan]->name);
		/* FIXME: functie de listat porturile paginat */
		list_for_each_entry(link, &sw.vdb[vlan]->trunk_ports, lh) {
			len += sprintf(page+len,"%s ", link->port->dev->name);
		}
		list_for_each_entry(link, &sw.vdb[vlan]->non_trunk_ports, lh) {
			if (!link->port->dev->sw_port) continue;
			len += sprintf(page+len,"%s ", link->port->dev->name);
		}
		len += sprintf(page+len, "\n");
	}
	
	rcu_read_unlock();
	
	return len;	
}	

static int proc_read_vif(char *page, char **start,
		off_t off, int count,
		int *eof, void *data) {
	int len = 0, vlan;

	for (vlan=SW_MIN_VLAN; vlan<=SW_MAX_VLAN; vlan++) {
		if (sw_vif_find(&sw, vlan))
			len += sprintf(page+len, "vlan%d\n", vlan);
	}
	return len;
}	

int init_switch_proc(void) {

	/* Create our own directory under /proc/net */
	switch_dir = proc_net_mkdir(&init_net, SW_PROCFS_DIR, init_net.proc_net);
	if (!switch_dir)
		return -ENOMEM;

	switch_dir->owner = THIS_MODULE;

	/*
	   Create the ifaces file, which lists information
	   about the interfaces added to switch
	 */
	iface_file = create_proc_read_entry(SW_PROCFS_IFACES, 0644,
			switch_dir, proc_read_ifaces, NULL);
	if (!iface_file) {
		proc_net_remove(&init_net, SW_PROCFS_DIR);
		return -ENOMEM;
	}
	iface_file->owner = THIS_MODULE;

	mac_file = create_proc_read_entry(SW_PROCFS_MAC, 0644,
			switch_dir, proc_read_mac, NULL);
	if (!mac_file) {
		remove_proc_entry(SW_PROCFS_IFACES, switch_dir);
		proc_net_remove(&init_net, SW_PROCFS_DIR);
		return -ENOMEM;
	}
	mac_file->owner = THIS_MODULE;
	
	vlan_file = create_proc_read_entry(SW_PROCFS_VLAN, 0644,
			switch_dir, proc_read_vlan, NULL);
	if (!vlan_file) {
		remove_proc_entry(SW_PROCFS_IFACES, switch_dir);
		remove_proc_entry(SW_PROCFS_MAC, switch_dir);
		proc_net_remove(&init_net, SW_PROCFS_DIR);
		return -ENOMEM;
	}
	vlan_file->owner = THIS_MODULE;

	vif_file = create_proc_read_entry(SW_PROCFS_VIF, 0644,
			switch_dir, proc_read_vif, NULL);
	if (!vif_file) {
		remove_proc_entry(SW_PROCFS_IFACES, switch_dir);
		remove_proc_entry(SW_PROCFS_MAC, switch_dir);
		remove_proc_entry(SW_PROCFS_VLAN, switch_dir);
		proc_net_remove(&init_net, SW_PROCFS_DIR);
	}

	dbg("sw_proc initialized\n");
	return 0;
}

void cleanup_switch_proc(void) {
	remove_proc_entry(SW_PROCFS_IFACES, switch_dir);
	remove_proc_entry(SW_PROCFS_MAC, switch_dir);
	remove_proc_entry(SW_PROCFS_VLAN, switch_dir);
	remove_proc_entry(SW_PROCFS_VIF, switch_dir);
	proc_net_remove(&init_net, SW_PROCFS_DIR);
}
