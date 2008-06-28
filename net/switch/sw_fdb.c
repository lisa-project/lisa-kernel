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

void sw_fdb_init(struct net_switch *sw) {
	int i;

	for (i=0; i<SW_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&sw->fdb[i].entries);
		spin_lock_init(&sw->fdb[i].lock);
	}
	sw->fdb_cache = kmem_cache_create("sw_fdb_cache",
		sizeof(struct net_switch_fdb_entry),
		0, SLAB_HWCACHE_ALIGN, NULL, NULL);
		
	dbg("Initialized hash of %d buckets\n", SW_HASH_SIZE);	
}

static __inline__ unsigned long sw_age_time(struct net_switch_fdb_entry *entry) {
	return atomic_read(&entry->port->sw->fdb_age_time);
}

void sw_free_entry_rcu(struct rcu_head *rcu) {
	struct net_switch_fdb_entry *entry = container_of(rcu, struct net_switch_fdb_entry, rcu);
	dbg("About to free fdb entry at 0x%p for port %s (age timer expired)\n",
			entry, entry->port->dev->name);
	kmem_cache_free(entry->port->sw->fdb_cache, entry);
}

void sw_age_timer_expired(unsigned long arg) {
	struct net_switch_fdb_entry *entry;

	entry = (struct net_switch_fdb_entry *)arg;
	if (time_before_eq(entry->stamp + sw_age_time(entry), jiffies)) {
		spin_lock(&entry->bucket->lock);
		del_timer(&entry->age_timer);
		list_del_rcu(&entry->lh);
		spin_unlock(&entry->bucket->lock);
		call_rcu(&entry->rcu, sw_free_entry_rcu);
	}
	else {
		dbg("Reactivating timer for fdb entry at 0x%p for port %s\n",
				entry, entry->port->dev->name);
		mod_timer(&entry->age_timer, entry->stamp + sw_age_time(entry));
	}
}

static void sw_timer_add(struct timer_list *timer,
		void (* callback)(unsigned long),
		unsigned long data,
		unsigned long expires) {
	init_timer(timer);
	timer->function = callback;
	timer->data = data;
	timer->expires = expires;
	add_timer(timer);
}

/* Walk the fdb and delete all entries referencing a given port.
   
   This is (should be) always called from user space, so locking is
   done _with_ softirqs disabled (spin_lock_bh()).

   Writing is done in a transactional manner: first check if we need
   to change a bucket, and then lock the bucket only if we need to
   change it. While holding the lock search for the entry again to
   avoid races.
 */
int fdb_cleanup_port(struct net_switch_port *port, int addr_type) {
    struct net_switch *sw = port->sw;
    struct net_switch_fdb_entry *entry, *tmp;
	struct list_head *entry_lh, *tmp_lh;
	LIST_HEAD(del_list);
    int i, ret = 0;
	
	for (i = 0; i < SW_HASH_SIZE; i++) {
		list_for_each_entry_rcu(entry, &sw->fdb[i].entries, lh) {
			if(entry->port == port && (addr_type == SW_FDB_ANY || entry->is_static == addr_type))
                break;
		}
        if(&entry->lh == &sw->fdb[i].entries)
            continue;
        /* We found entries; lock for write and delete them */
        spin_lock_bh(&sw->fdb[i].lock);
		list_for_each_safe_rcu(entry_lh, tmp_lh, &sw->fdb[i].entries) {
			entry = list_entry(entry_lh, struct net_switch_fdb_entry, lh);
			if(entry->port == port && (addr_type == SW_FDB_ANY || entry->is_static == addr_type)) {
				if (!entry->is_static)
					del_timer(&entry->age_timer);
                list_del_rcu(&entry->lh);
				list_add(&entry->lh, &del_list);
            }
		}
        spin_unlock_bh(&sw->fdb[i].lock);
	}
	synchronize_sched();
	list_for_each_entry_safe(entry, tmp, &del_list, lh) {
		dbg("About to free fdb entry at 0x%p for port %s\n",
				entry, entry->port->dev->name);
		kmem_cache_free(sw->fdb_cache, entry);
		ret++;
	}
	return ret;
}

/* Walk the fdb and delete all entries by addr_type. */
int fdb_cleanup_by_type(struct net_switch *sw, int addr_type) {
    struct net_switch_fdb_entry *entry, *tmp;
	struct list_head *entry_lh, *tmp_lh;
	LIST_HEAD(del_list);
    int i, ret = 0;
	
	for (i = 0; i < SW_HASH_SIZE; i++) {
        spin_lock_bh(&sw->fdb[i].lock);
		list_for_each_safe_rcu(entry_lh, tmp_lh, &sw->fdb[i].entries) {
			entry = list_entry(entry_lh, struct net_switch_fdb_entry, lh);
			if(addr_type == SW_FDB_ANY || entry->is_static == addr_type) {
				if (!entry->is_static)
					del_timer(&entry->age_timer);
                list_del_rcu(&entry->lh);
				list_add(&entry->lh, &del_list);
            }
		}
        spin_unlock_bh(&sw->fdb[i].lock);
	}
	synchronize_sched();
	list_for_each_entry_safe(entry, tmp, &del_list, lh) {
		dbg("About to free fdb entry at 0x%p for port %s\n",
				entry, entry->port->dev->name);
		kmem_cache_free(sw->fdb_cache, entry);
		ret++;
	}
	return ret;
}

/* Walk the fdb and delete all entries referencing a given vlan.
   
   This is (should be) always called from user space, so locking is
   done _with_ softirqs disabled (spin_lock_bh()).

   Writing is done in a transactional manner: first check if we need
   to change a bucket, and then lock the bucket only if we need to
   change it. While holding the lock search for the entry again to
   avoid races.
 */
int fdb_cleanup_vlan(struct net_switch *sw, int vlan, int addr_type) {
    struct net_switch_fdb_entry *entry, *tmp;
	struct list_head *entry_lh, *tmp_lh;
	LIST_HEAD(del_list);
    int i, ret = 0;
	
	for (i = 0; i < SW_HASH_SIZE; i++) {
		list_for_each_entry_rcu(entry, &sw->fdb[i].entries, lh) {
			if(entry->vlan == vlan && (addr_type == SW_FDB_ANY || entry->is_static == addr_type))
                break;
		}
        if(&entry->lh == &sw->fdb[i].entries)
            continue;
        /* We found entries; lock for write and delete them */
        spin_lock_bh(&sw->fdb[i].lock);
		list_for_each_safe_rcu(entry_lh, tmp_lh, &sw->fdb[i].entries) {
			entry = list_entry(entry_lh, struct net_switch_fdb_entry, lh);
			if(entry->vlan == vlan && (addr_type == SW_FDB_ANY || entry->is_static == addr_type)) {
				if (!entry->is_static)
					del_timer(&entry->age_timer);
                list_del_rcu(&entry->lh);
				list_add(&entry->lh, &del_list);
            }
		}
        spin_unlock_bh(&sw->fdb[i].lock);
	}
	synchronize_sched();
	list_for_each_entry_safe(entry, tmp, &del_list, lh) {
		dbg("About to free fdb entry at 0x%p for port %s\n",
				entry, entry->port->dev->name);
		kmem_cache_free(sw->fdb_cache, entry);
		ret++;
	}
	return ret;
}

/* Delete all entries having a specific address.
   
   This is (should be) always called from user space, so locking is
   done _with_ softirqs disabled (spin_lock_bh()).

   Writing is done in a transactional manner: first check if we need
   to change a bucket, and then lock the bucket only if we need to
   change it. While holding the lock search for the entry again to
   avoid races.
 */
int fdb_del(struct net_switch *sw, unsigned char *mac,
		struct net_switch_port *port, int vlan, int addr_type) {
	struct net_switch_fdb_entry *entry, *tmp;
	struct list_head *entry_lh, *tmp_lh;
	LIST_HEAD(del_list);
	struct net_switch_bucket *bucket = &sw->fdb[sw_mac_hash(mac)];
	int ret = 0;

	list_for_each_entry_rcu(entry, &bucket->entries, lh) {
		if((!vlan || entry->vlan == vlan) && (!port || entry->port == port) &&
				(addr_type = SW_FDB_ANY || entry->is_static == addr_type) &&
				!memcmp(entry->mac, mac, ETH_ALEN))
			break;
	}
	if(&entry->lh == &bucket->entries)
		return ret;
	/* We found entries; lock for write and delete them */
	spin_lock_bh(&bucket->lock);
	list_for_each_safe_rcu(entry_lh, tmp_lh, &bucket->entries) {
		entry = list_entry(entry_lh, struct net_switch_fdb_entry, lh);
		if((!vlan || entry->vlan == vlan) && (!port || entry->port == port) &&
				(addr_type = SW_FDB_ANY || entry->is_static == addr_type) &&
				!memcmp(entry->mac, mac, ETH_ALEN)) {
			if (!entry->is_static)
				del_timer(&entry->age_timer);
			list_del_rcu(&entry->lh);
			list_add(&entry->lh, &del_list);
		}
	}
	spin_unlock_bh(&bucket->lock);
	synchronize_sched();
	list_for_each_entry_safe(entry, tmp, &del_list, lh) {
		dbg("About to free fdb entry at 0x%p for port %s\n",
				entry, entry->port->dev->name);
		kmem_cache_free(sw->fdb_cache, entry);
		ret++;
	}
	return ret;
}

static void __fdb_change_to_static(struct net_switch_fdb_entry *entry) {
	local_bh_disable();
	del_timer(&entry->age_timer);
	entry->is_static = SW_FDB_STATIC;
	local_bh_enable();
}

int fdb_lookup(struct net_switch_bucket *bucket, unsigned char *mac,
		int vlan, struct net_switch_fdb_entry **pentry) {
	struct net_switch_fdb_entry *entry;	

	list_for_each_entry_rcu(entry, &bucket->entries, lh) {
		if (!memcmp(entry->mac, mac, ETH_ALEN) && entry->vlan == vlan) {
			*pentry = entry;
			return 1;
		}
	}
	return 0;
}

static inline int __fdb_learn(struct net_switch_bucket *bucket,
		unsigned char *mac, struct net_switch_port *port, int vlan,
		struct net_switch_fdb_entry **pentry) {
	struct net_switch_fdb_entry *entry;

	list_for_each_entry_rcu(entry, &bucket->entries, lh) {
		if(entry->vlan == vlan && (port == NULL || entry->port == port) &&
				!memcmp(entry->mac, mac, ETH_ALEN)) {
			*pentry = entry;
			return 1;
		}
	}
	return 0;
}

/* This is called from both softirq and user context, so we do locking with
   softirqs disabled
 */
int fdb_learn(unsigned char *mac, struct net_switch_port *port,
		int vlan, int is_static, int is_mcast) {
	struct net_switch_bucket *bucket = &port->sw->fdb[sw_mac_hash(mac)];
	struct net_switch_fdb_entry *entry;

	if(is_l2_mac(mac))
		return -EINVAL;

	if(__fdb_learn(bucket, mac, is_mcast ? port : NULL, vlan, &entry)) {
		/* we found a matching entry */
		if (entry->is_static)
			return -EBUSY; /* don't modify a static fdb entry */
		entry->port = port; /* FIXME this should be atomic ??? */
		entry->stamp = jiffies;
		if (is_static && !entry->is_static) 
			__fdb_change_to_static(entry);
		return 0;
	}

	/* No matching entry. This time lock bucket, but search for the entry
	   again, because someone might have added it in the meantime.
	 */
	spin_lock_bh(&bucket->lock);
	if(__fdb_learn(bucket, mac, is_mcast ? port : NULL, vlan, &entry)) {
		/* we found a matching entry */
		if (entry->is_static)
			return -EBUSY;
		entry->port = port;
		entry->stamp = jiffies;
		if (is_static && !entry->is_static)
			__fdb_change_to_static(entry);
		spin_unlock_bh(&bucket->lock);
		return 0;
	}

	/*
		we try to alloc an entry from the cache.
		If that fails we return.
	*/
	entry = kmem_cache_alloc(port->sw->fdb_cache, GFP_ATOMIC);
	if (!entry) {
		spin_unlock_bh(&bucket->lock);
		dbg("cache out of memory");
		return -ENOMEM;
	}

	memcpy(entry->mac, mac, ETH_ALEN);
	entry->vlan = vlan;
	entry->port = port;
	entry->stamp = jiffies;
	entry->bucket = bucket;
	entry->is_static = is_static;
	if (!entry->is_static)  
		sw_timer_add(&entry->age_timer, sw_age_timer_expired,
				(unsigned long)entry, entry->stamp + sw_age_time(entry));
	/* FIXME smp_wmb() here ?? */
	list_add_tail_rcu(&entry->lh, &bucket->entries);
	spin_unlock_bh(&bucket->lock);
	return 0;
}

#ifdef DEBUG
EXPORT_SYMBOL(fdb_cleanup_port);
EXPORT_SYMBOL(fdb_lookup);
EXPORT_SYMBOL(fdb_learn);
#endif

void sw_fdb_exit(struct net_switch *sw) {
	/* Entries are freed by __sw_delif(), which is called for
       all interfaces before this
     */
	kmem_cache_destroy(sw->fdb_cache);
}
