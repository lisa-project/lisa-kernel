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
#include <linux/ip.h>
#include <linux/igmp.h>

static inline short sw_eth_hproto(const struct sk_buff *skb, const struct skb_extra *skb_e)
{
	return ntohs(*(__be16 *)(skb_mac_header(skb) + (skb_e->has_vlan_tag ? 14 : 12)));
}

static inline struct iphdr *sw_ip_hdr(const struct sk_buff *skb, const struct skb_extra *skb_e)
{
	return (struct iphdr *)(skb_mac_header(skb) + 14 + (skb_e->has_vlan_tag ? 2 : 0));
}

static inline struct igmphdr *sw_igmp_hdr(const struct sk_buff *skb, const struct skb_extra *skb_e)
{
	struct iphdr *iph = sw_ip_hdr(skb, skb_e);
	return (struct igmphdr *)((unsigned char *)iph + iph->ihl * 4);
}

static __inline__ void add_vlan_tag(struct sk_buff *skb, int vlan)
{
	int nhead = (ETH_HLEN + VLAN_TAG_BYTES) - (skb->data - skb->head);
	
	dbg("%s: head=%p data=%p mac.raw=%p nh.raw=%p h.raw=%p csum=%x "
			"csum_start=%x csum_offset=%x ip_summed=%d\n",
			__func__, skb->head, skb->data, skb_mac_header(skb),
			skb_network_header(skb), skb_transport_header(skb),
			skb->csum, skb->csum_start, skb->csum_offset,
			(int)skb->ip_summed);
	/* If we don't have enough headroom, we make some :D */
	/* FIXME daca nu avem destul headroom, poate avem destul tailroom
	   si atunci e mai eficient sa mutam datele decat sa copiem tot
	   pachetul si sa mutam headerul */
	if (nhead > 0) {
		pskb_expand_head(skb, nhead, 0, GFP_ATOMIC); 
		/* FIXME maybe this can break ip_summed, csum and Xen (although
		 * these fields are not touched in pskb_expand_head) */
		dbg("add_vlan_tag: pskb_expand_head necessary\n");
		dbg("add_vlan_tag(after expand): skb=0x%p skb headroom: %d (head=0x%p data=0x%p)\n",
			skb, skb->data - skb->head, skb->head, skb->data);
	}
	memmove(skb_mac_header(skb)-VLAN_TAG_BYTES, skb_mac_header(skb), 2 * ETH_ALEN);
	skb->mac_header -= VLAN_TAG_BYTES;
	/* adding to or subtracting from skb->mac_header should work the same
	   with both offset and pointer skb storage implementations */
	skb_push(skb, VLAN_TAG_BYTES);
	skb_reset_network_header(skb);
	/* FIXME: old comment says "skb->h.raw doesn't need to be set here,
	   because it's properly set later in dev_queue_xmit(), but now we
	   cannot find any change on skb->transport_header in dev_queue_xmit()
	   unless skb->ip_summed == CHECKSUM_PARTIAL (where the transport
	   header is explicitly set) */
	*(short *)skb->data = htons((short)vlan);
	*(short *)(skb_mac_header(skb) + ETH_HLEN - 2) = htons(ETH_P_8021Q);
#ifdef CONFIG_XEN
	/* This is pretty obscure :) First of all, unless we have xen,
	 * ip_summed is either CHECKSUM_NONE or CHECKSUM_COMPLETE
	 * (but never CHECKSUM_PARTIAL - the value here is the one we
	 * get from the rx stack and CHECKSUM_PARTIAL has no meaning
	 * for rx).
	 *
	 * If we do have xen and get CHECKSUM_PARTIAL, then the packet
	 * came from a xennet backend and checksum must be carried out
	 * here in dom0 for optimization. Even if we don't touch
	 * ip_summed and hand it with CHECKSUM_PARTIAL to the tx stack,
	 * things go wrong becauze Ether Type is no longer ip (0x8000)
	 * but 802.1q (0x8100). NICs that support hardware ip sum seem
	 * to get confused and don't do the checksum. So do it here in
	 * software to avoid trouble.
	 */
	if (skb->ip_summed == CHECKSUM_PARTIAL && skb->protocol == htons(ETH_P_IP)) {
		dbg("%s: applying xen ip checksum fix\n", __func__);
		skb->csum_start -= ETH_HLEN;
		/* skb->h.raw = skb->head + skb->csum_start; */
		skb_set_transport_header(skb, skb->csum_start - skb_headroom(skb));
		skb_checksum_help(skb);
		/* skb_checksum_help() sets skb->ip_summed to CHECKSUM_NONE */
		skb->csum = 0;
	}
#endif
	dbg("%s: head=%p data=%p mac.raw=%p nh.raw=%p h.raw=%p csum=%x\n",
			__func__, skb->head, skb->data, skb_mac_header(skb),
			skb_network_header(skb), skb_transport_header(skb),
			skb->csum);
}

static __inline__ void strip_vlan_tag(struct sk_buff *skb)
{
	memmove(skb_mac_header(skb)+VLAN_TAG_BYTES, skb_mac_header(skb), 2 * ETH_ALEN);
	skb->mac_header += VLAN_TAG_BYTES;
	skb_pull(skb, VLAN_TAG_BYTES);
	skb->protocol = *(short *)(skb_mac_header(skb) + ETH_HLEN - 2);
}

static __inline__ void __strip_vlan_tag(struct sk_buff *skb, int vlan)
{
	strip_vlan_tag(skb);
}

/* FIXME: pkt_type may be PACKET_MULTICAST */
static __inline__ void sw_skb_xmit(struct sk_buff *skb, struct net_device *dev,
		unsigned char pkt_type)
{
	/* FIXME packets larger that 1500 may break things */
#ifdef DEBUG
	if (skb->len > dev->mtu) {
		dbg("%s: mtu exceeded on %s len=%d mtu=%d\n",
				__func__, dev->name, skb->len, dev->mtu);
		// goto destroy;
	}
#endif

	if (dev->sw_port) {
		/* This is a physical port (not a bogus one i.e. vif) */
		skb->dev = dev;

		/* Prevent packets from being checksummed again on egress.
		 * A nice explanation of the ip_summed field can be found
		 * in skbuff.h (around line 50). For Xen integration, comply
		 * with their hw/csum optimization.
		 */
#ifdef CONFIG_XEN
		/* CHECKSUM_COMPLETE changes to CHECKSUM_NONE
		 * This prevents checksumming for real devices ingres
		 * packets (which always have CHECKSUM_COMPLETE - see
		 * checksum constans explanation for rx in skbuff.h)
		 * but keeps CHECKSUM_PARTIAL untouched (this is the
		 * case for xen vif ingress packets).
		 */
		skb_forward_csum(skb);
#else
		skb->ip_summed = CHECKSUM_NONE;
#endif
		skb_push(skb, ETH_HLEN);
		dbg("%s: sending out skb; ip_summed=%d\n", __func__, (int)skb->ip_summed);
		dev_queue_xmit(skb);
		return;
	}
	/* This is a vif, so we need to call sw_vif_rx() instead. The
	 * ip_summed field is properly handled by sw_vif_rx() */
	sw_vif_rx(skb, pkt_type, dev);
	return;
	
/* destroy:	*/
	dev_kfree_skb(skb);
}

/* if the packet data is used by someone else
   we make a copy before altering it 
 */
static void sw_skb_unshare(struct sk_buff **skb)
{
	struct sk_buff *skb2;

	if (atomic_read(&skb_shinfo(*skb)->dataref)) {
		dbg("%s: expanding skb=%p\n", __func__, *skb);
		skb2 = skb_copy_expand(*skb, ETH_HLEN+VLAN_TAG_BYTES, 0, GFP_ATOMIC);
		BUG_ON(!skb2);

		/* Fix ip_summed stuff (also see "BUG ALERT" at bottom of
		 * skb_copy_expand() comment)
		 */
		skb2->csum = (*skb)->csum;
		skb2->ip_summed = (*skb)->ip_summed;

		dev_kfree_skb(*skb);
		*skb = skb2;
	}
}

/* Forward frame from in port to out port,
   adding/removing vlan tag if necessary.
 */
static void __sw_forward(struct net_switch_port *in, struct net_switch_port *out,
	struct sk_buff *skb, struct skb_extra *skb_e)
{
	dbg("%s: forwarding frame to %s; ip_summed=%d\n", __func__,
			out->dev->name, (int)skb->ip_summed);
	if (out->flags & SW_PFL_TRUNK && !(in->flags & SW_PFL_TRUNK)) {
		/* must add vlan tag (vlan = in->vlan) */
		sw_skb_unshare(&skb);
		add_vlan_tag(skb, in->vlan);
	}
	else if (!(out->flags & SW_PFL_TRUNK) && in->flags & SW_PFL_TRUNK) {
		/* must remove vlan tag */
		sw_skb_unshare(&skb);
		strip_vlan_tag(skb);
	}
	sw_skb_xmit(skb, out->dev, PACKET_HOST);
}

struct sw_flood_context {
	struct net_switch_port *in;
	int vlan;
	int pkt_type;
	struct sk_buff *skb;
	struct list_head *lh1;
	struct list_head *lh2;
	struct net_device *(*netdev)(struct list_head *);
	int (*valid)(struct list_head *, int,
			struct sw_flood_context *);
	void *priv;
#ifdef DEBUG
	int copied, cloned, unshared;
#endif
};

#ifdef DEBUG
#define __sw_flood_inc_cloned ctx->cloned++
#define __sw_flood_inc_copied ctx->copied++
#define __sw_flood_inc_unshared ctx->unshared++
#else
#define __sw_flood_inc_cloned
#define __sw_flood_inc_copied
#define __sw_flood_inc_unshared
#endif

static int __sw_flood(struct sw_flood_context *ctx)
{
	struct list_head *entry, *prev=NULL, *oldprev;
	struct sk_buff *skb, *skb2;
	int needs_tag_change = 1, ret = 0;
	void (*vlan_op)(struct sk_buff *, int);

	vlan_op = ctx->in->flags & SW_PFL_TRUNK ? __strip_vlan_tag : add_vlan_tag;
	skb = ctx->skb;

	__list_for_each_rcu(entry, ctx->lh1) {
		if (!ctx->valid(entry, 1, ctx))
			continue;
		if (prev) {
			__sw_flood_inc_cloned;
			skb2 = skb_clone(skb, GFP_ATOMIC);
			sw_skb_xmit(skb, ctx->netdev(prev), ctx->pkt_type);
			ret++;
			skb = skb2;
		}
		prev = entry;
	}
	oldprev = prev;
	__list_for_each_rcu(entry, ctx->lh2) {
		if (!ctx->valid(entry, 2, ctx))
			continue;
		if (oldprev && prev == oldprev) {
			/* 1 or more elements in lh1 && and we're at the first element
			   in lh2; make a copy of the skb, then send the last skb from
			   lh1 */
			__sw_flood_inc_copied;
			skb2 = skb_copy_expand(skb, ETH_HLEN+VLAN_TAG_BYTES, 0, GFP_ATOMIC);
			vlan_op(skb2, ctx->vlan);
			needs_tag_change = 0;
			sw_skb_xmit(skb, ctx->netdev(prev), ctx->pkt_type);
			ret++;
			skb = skb2;
			prev = entry;
			continue;
		}
		if (prev) {
			if (needs_tag_change) {
				/* 0 elements in lh1, and we're at the 2nd element in lh2;
				   make sure skb is an exclusive copy and apply the tag
				   change to it before it gets cloned and sent */
				__sw_flood_inc_unshared;
				sw_skb_unshare(&skb);
				vlan_op(skb, ctx->vlan);
				needs_tag_change = 0;
			}
			__sw_flood_inc_cloned;
			skb2 = skb_clone(skb, GFP_ATOMIC);
			sw_skb_xmit(skb, ctx->netdev(prev), ctx->pkt_type);
			ret++;
			skb = skb2;
		}
		prev = entry;
	}
	if (prev) {
		if (needs_tag_change && prev != oldprev) {
			/* lh2 is not empty, so the remaining element is from lh2,
			   but the tag change was not applied */
			__sw_flood_inc_unshared;
			sw_skb_unshare(&skb);
			vlan_op(skb, ctx->vlan);
		}
		sw_skb_xmit(skb, ctx->netdev(prev), ctx->pkt_type);
		ret++;
	}
	else {
		dbg("%s: freeing skb\n", __func__);
		dev_kfree_skb(skb);
	}
	return ret;
}

#undef __sw_flood_inc_cloned
#undef __sw_flood_inc_copied
#undef __sw_flood_inc_unshared

static struct net_device *sw_fdb_entry_to_netdev(struct list_head *l)
{
	struct net_switch_fdb_entry *entry;

	entry = list_entry(l, struct net_switch_fdb_entry, lh);
	return entry->port->dev;
}

static int sw_group_fdb_entry_valid(struct list_head *curr, int stage,
		struct sw_flood_context *ctx)
{
	struct net_switch_fdb_entry *entry;
	int first_trunkness;
	unsigned char *mc_vmac;

	entry = list_entry(curr, struct net_switch_fdb_entry, lh);
	first_trunkness = ctx->in->flags & SW_PFL_TRUNK;
	mc_vmac = (unsigned char *)ctx->priv;

	if (entry->port == ctx->in)
		return 0;
	if (stage == 1 && (entry->port->flags & SW_PFL_TRUNK) != first_trunkness)
		return 0;
	if (stage == 2 && (entry->port->flags & SW_PFL_TRUNK) == first_trunkness)
		return 0;
	if (entry->vlan != ctx->vlan)
		return 0;
	if (memcmp(entry->mac, mc_vmac, ETH_ALEN))
		return 0;
	if (!(entry->type & SW_FDB_IGMP))
		return 0;
	/* In sw_flood we use the vdb, so we know for sure that 'vlan' is
	   allowed on the output port. But here we walk the fdb, so we need
	   to additionally check if 'vlan' is allowed on the output port. */
	if ((first_trunkness && sw_port_forbidden_vlan(entry->port, ctx->vlan)) ||
			(!first_trunkness && entry->port->vlan != ctx->vlan))
		return 0;
	return 1;
}

static int sw_flood_group(struct net_switch *sw, struct net_switch_port *in,
		struct sk_buff *skb, struct skb_extra *skb_e)
{
	struct net_switch_bucket *bucket = &sw->fdb[sw_mac_hash(skb_mac_header(skb))];
	unsigned char mc_vmac[ETH_ALEN] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
	struct sw_flood_context ctx;
	int ret;

	/* Build the multicast group virtual MAC */
	*(__be32 *)(&mc_vmac[2]) = sw_ip_hdr(skb, skb_e)->daddr;
	/* Lookup in FDB to select the bucket */
	bucket = &sw->fdb[sw_mac_hash(mc_vmac)];

	memset(&ctx, 0, sizeof(ctx));
	ctx.in = in;
	ctx.vlan = skb_e->vlan;
	ctx.pkt_type = PACKET_MULTICAST;
	ctx.skb = skb;
	ctx.lh1 = &bucket->entries;
	ctx.lh2 = &bucket->entries;
	ctx.netdev = sw_fdb_entry_to_netdev;
	ctx.valid = sw_group_fdb_entry_valid;
	ctx.priv = &mc_vmac[0];

	ret = __sw_flood(&ctx);

	dbg("%s: cloned=%d copied=%d unshared=%d\n", __func__, ctx.cloned, ctx.copied,
			ctx.unshared);
	return ret;
}

static struct net_device *sw_vdb_link_to_netdev(struct list_head *l)
{
	struct net_switch_vdb_link *entry;

	entry = list_entry(l, struct net_switch_vdb_link, lh);
	return entry->port->dev;
}

static int sw_flood_mrouters_valid(struct list_head *curr, int stage,
		struct sw_flood_context *ctx)
{
	struct net_switch_vdb_link *link;

	link = list_entry(curr, struct net_switch_vdb_link, lh);
	if (link->port == ctx->in)
		return 0;
	return sw_is_mrouter(link->port->mrouters, ctx->vlan);
}

static int sw_flood_mrouters(struct net_switch *sw, struct net_switch_port *in,
		struct sk_buff *skb, int vlan)
{
	struct sw_flood_context ctx;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.in = in;
	ctx.vlan = vlan;
	ctx.pkt_type = PACKET_MULTICAST;
	ctx.skb = skb;
	ctx.netdev = sw_vdb_link_to_netdev;
	ctx.valid = sw_flood_mrouters_valid;

	/* if source port is in trunk mode we first send the
	   socket buffer to all trunk ports in that vlan then
	   strip vlan tag and send to all non-trunk ports in that vlan
	 */
	if (in->flags & SW_PFL_TRUNK) {
		ctx.lh1 = &sw->vdb[vlan]->trunk_ports;
		ctx.lh2 = &sw->vdb[vlan]->non_trunk_ports;
	}
	else {
	/* otherwise we send the frame to all non-trunk ports in that vlan
	   then add a vlan tag to it and send it to all trunk ports in that vlan.
	 */
		ctx.lh1 = &sw->vdb[vlan]->non_trunk_ports;
		ctx.lh2 = &sw->vdb[vlan]->trunk_ports;
	}

	ret = __sw_flood(&ctx);

	dbg("%s: cloned=%d copied=%d unshared=%d\n", __func__, ctx.cloned, ctx.copied,
			ctx.unshared);
	return ret;
}

static int sw_flood_valid(struct list_head *curr, int stage,
		struct sw_flood_context *ctx)
{
	struct net_switch_vdb_link *link;

	link = list_entry(curr, struct net_switch_vdb_link, lh);
	return (link->port != ctx->in);
}

/* Flood frame to all necessary ports */
static int sw_flood(struct net_switch *sw, struct net_switch_port *in,
		struct sk_buff *skb, int vlan) {
	struct sw_flood_context ctx;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.in = in;
	ctx.vlan = vlan;
	ctx.pkt_type = PACKET_BROADCAST;
	ctx.skb = skb;
	ctx.netdev = sw_vdb_link_to_netdev;
	ctx.valid = sw_flood_valid;

	/* if source port is in trunk mode we first send the 
	   socket buffer to all trunk ports in that vlan then
	   strip vlan tag and send to all non-trunk ports in that vlan 
	 */
	if (in->flags & SW_PFL_TRUNK) {
		ctx.lh1 = &sw->vdb[vlan]->trunk_ports;
		ctx.lh2 = &sw->vdb[vlan]->non_trunk_ports;
	}
	else {
	/* otherwise we send the frame to all non-trunk ports in that vlan 
	   then add a vlan tag to it and send it to all trunk ports in that vlan.
	 */
		ctx.lh1 = &sw->vdb[vlan]->non_trunk_ports;
		ctx.lh2 = &sw->vdb[vlan]->trunk_ports;
	}

	ret = __sw_flood(&ctx);

	dbg("%s: cloned=%d copied=%d unshared=%d\n", __func__, ctx.cloned, ctx.copied,
			ctx.unshared);
	return ret;
}	

static int sw_vif_forward(struct sk_buff *skb, struct skb_extra *skb_e)
{
	struct net_switch *sw = (skb->dev->sw_port)? skb->dev->sw_port->sw:
		((struct net_switch_vif_priv *)netdev_priv(skb->dev))->bogo_port.sw;
	unsigned char *vif_mac = sw->vif_mac;
	struct net_device *dev;
	int vlan;

	if(memcmp(skb_mac_header(skb), vif_mac, ETH_ALEN - 2))
		return 0;
	vlan = (vif_mac[ETH_ALEN - 2] ^ skb_mac_header(skb)[ETH_ALEN - 2]) * 0x100 +
		(vif_mac[ETH_ALEN - 1] ^ skb_mac_header(skb)[ETH_ALEN - 1]);
	if(vlan == skb_e->vlan && (dev = sw_vif_find(sw, vlan))) {
		if(skb_e->has_vlan_tag) {
			sw_skb_unshare(&skb);
			strip_vlan_tag(skb);
		}
		sw_vif_rx(skb, PACKET_HOST, dev);
		return 1;
	}
	return 0;
}

static int sw_multicast(struct net_switch *sw, struct net_switch_port *in,
		struct sk_buff *skb, struct skb_extra *skb_e)
{
	unsigned char mc_vmac[ETH_ALEN] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
	struct net_switch_port *port;

	dbg("%s: vlan=%d eth=%x (%x) ip=%x (%x) igmp=%x (%x)\n", __func__,
			skb_e->vlan,
			sw_eth_hproto(skb, skb_e), ETH_P_IP,
			sw_ip_hdr(skb, skb_e)->protocol, IPPROTO_IGMP,
			sw_igmp_hdr(skb, skb_e)->type, IGMPV2_HOST_MEMBERSHIP_REPORT);
	dbg("%s: skb->data=%p mac_header=%p network_header=%p transport_header=%p\n",
			__func__, skb->data,
			skb_mac_header(skb), skb_network_header(skb), skb_transport_header(skb));

	/* if IGMP snooping is disabled we need to flood the frame */
	if (!sw->igmp_snooping || !sw->vdb[skb_e->vlan]->igmp_snooping)
		return sw_flood(sw, in, skb, skb_e->vlan);

	/* IGMP membership report: create group and forward to mrouters */
	if (sw_eth_hproto(skb, skb_e) == ETH_P_IP && sw_ip_hdr(skb, skb_e)->protocol == IPPROTO_IGMP &&
			sw_igmp_hdr(skb, skb_e)->type == IGMPV2_HOST_MEMBERSHIP_REPORT) {
		if (!sw_is_mrouter(in->mrouters, skb_e->vlan)) {
			list_for_each_entry(port, &sw->ports, lh) {
				if (sw_is_mrouter(port->mrouters, skb_e->vlan))
					goto fdb_igmp_learn;
			}
			dbg("%s: port is not mrouter and no other mrouter found\n", __func__);
			dev_kfree_skb(skb);
			return 0;
		}
fdb_igmp_learn:
		/* Add the multicast group virtual MAC in the FDB */
		dbg("%s: learning group address\n", __func__);
		*(__be32 *)(&mc_vmac[2]) = sw_igmp_hdr(skb, skb_e)->group;
		fdb_learn(mc_vmac, in, skb_e->vlan, SW_FDB_IGMP_DYNAMIC);
		return sw_flood_mrouters(sw, in, skb, skb_e->vlan);
	}

	/* Normal multicast traffic */
	if (!sw_is_mrouter(in->mrouters, skb_e->vlan)) {
		list_for_each_entry(port, &sw->ports, lh) {
			if (sw_is_mrouter(port->mrouters, skb_e->vlan))
				return sw_flood_mrouters(sw, in, skb, skb_e->vlan);
		}
		return sw_flood(sw, in, skb, skb_e->vlan);
	}
	/* forward to multicast group members from the FDB */
	return sw_flood_group(sw, in, skb, skb_e);
}

/* Forwarding decision
   Returns the number of ports the packet was forwarded to.
 */
int sw_forward(struct net_switch_port *in,
		struct sk_buff *skb, struct skb_extra *skb_e)
{
	struct net_switch *sw = in->sw;
	struct net_switch_bucket *bucket = &sw->fdb[sw_mac_hash(skb_mac_header(skb))];
	struct net_switch_fdb_entry *out;
	int ret = 1;

	dbg("sw_forward: usage count %d\n", atomic_read(&skb_shinfo(skb)->dataref) != 1);
	if (sw_vif_forward(skb, skb_e))
		return ret;
	rcu_read_lock();
	if (is_mcast_mac(skb_mac_header(skb))) {
		ret = sw_multicast(sw, in, skb, skb_e);
		rcu_read_unlock();
		return ret;
	}
	if (fdb_lookup(bucket, skb_mac_header(skb), skb_e->vlan, &out)) {
		/* fdb entry found */
		rcu_read_unlock();
		if (in == out->port) {
			/* in_port == out_port */
			dbg("forward: Dropping frame, dport %s == sport %s\n",
					out->port->dev->name, in->dev->name);
			goto free_skb; 
		}
		if (!(out->port->flags & SW_PFL_TRUNK) && 
				skb_e->vlan != out->port->vlan) {
			dbg("forward: Dropping frame, dport %s vlan_id %d != skb_e.vlan_id %d\n",
					out->port->dev->name, out->port->vlan, skb_e->vlan);
			goto free_skb;
		}
		if ((out->port->flags & SW_PFL_TRUNK) &&
				(out->port->forbidden_vlans[skb_e->vlan / 8] & (1 << (skb_e->vlan % 8)))) {
			dbg("forward: Dropping frame, skb_e.vlan_id %d not in allowed vlans of dport %s\n",
					skb_e->vlan, out->port->dev->name);
			goto free_skb;
		}
		dbg("forward: Forwarding frame from %s to %s\n", in->dev->name,
				out->port->dev->name);
		__sw_forward(in, out->port, skb, skb_e);
	} else {
		rcu_read_unlock();
		dbg("forward: Flooding frame from %s to all necessary ports\n",
				in->dev->name);
		/*
		   The fact that skb_e->vlan exists in the vdb is based
		   _only_ on the checks performed in sw_handle_frame()
		 */
		ret = sw_flood(sw, in, skb, skb_e->vlan);
	}	
	return ret; 

free_skb:	
	dev_kfree_skb(skb);
	return 0;
}
