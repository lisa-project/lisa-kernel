#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include "lac_private.h"
#include "lac_debug.h"

/* TODO: local receive statistics*/
/* called with rcu_read_lock */
void start_lacp(struct net_lac_port *port, struct sk_buff *skb)
{
	La_Byte		*pdu;
	Lac_packet	*pkt;
	La_Short	*pw;
	Lac_info	actor;
	Lac_info	partner;
	
	pdu = (La_Byte *)skb_mac_header(skb);
#if 0	
	if(DEB_GET_LAC_RX_PDU(lac_debug_info))
	{
		lac_deb_print_rx_pdu(pdu);
	}
#endif
	pw = (La_Short *)&pdu[Lacpdu_etype_offset];
	if	(ntohs(*pw) != Slow_Protocols_Ethertype)
		goto out;

	
	/* Verify LACPDU subtype and version_number.*/
	if(pdu[Lacpdu_subtype_offset] != LACP_Subtype)
	{
		//lac_port_stats[port_no-1].unknownPduRx++;
		goto out;
	}

	if(pdu[Lacpdu_version_number_offset] != LACP_Version)
	{
		//lac_port_stats[port_no-1].illegalPduRx++;
		goto out;
	}

	/* Check for port loop back. */
	if(memcmp((char *)&pdu[Lacpdu_actor_sys_id_offset], (char *)la_system.sysId, sizeof(System_id))==0)
	{
		if(DEB_LAC_RX_PDU_ON)
			Lac_printi("Loop back on port %d.\n", port->port_no);
		goto out;
	}

	pw = (La_Short *)&pdu[Lacpdu_actor_sys_pri_offset];
	actor.system_priority = ntohs(*pw);

	memcpy((char *)&actor.system_id, (char*)&pdu[Lacpdu_actor_sys_id_offset], sizeof(System_id));
	
	pw = (La_Short *)&pdu[Lacpdu_actor_key_offset];
	actor.key = ntohs(*pw);

	pw = (La_Short *)&pdu[Lacpdu_actor_port_pri_offset];
	actor.port_priority =  ntohs(*pw);

	pw = (La_Short *)&pdu[Lacpdu_actor_port_offset];
	actor.port_no = ntohs(*pw);

	actor.state.state = pdu[Lacpdu_actor_state_offset];

	pw = (La_Short *)&pdu[Lacpdu_partner_sys_pri_offset];
	partner.system_priority = ntohs(*pw);

	memcpy((char *)&partner.system_id, &pdu[Lacpdu_partner_sys_id_offset], sizeof(System_id));

	pw = (La_Short *)&pdu[Lacpdu_partner_key_offset];
	partner.key = ntohs(*pw);

	pw = (La_Short *)&pdu[Lacpdu_partner_port_pri_offset];
	partner.port_priority = ntohs(*pw);

	pw = (La_Short *)&pdu[Lacpdu_partner_port_offset];
	partner.port_no = ntohs(*pw);

	partner.state.state  = pdu[Lacpdu_partner_state_offset];

	pw = (La_Short *)&pdu[Lacpdu_collector_max_del_offset];
	port->collector_max_delay = ntohs(*pw);

	pkt = kmalloc(sizeof(Lac_packet), GFP_ATOMIC);
	
	memcpy(&pkt->actor, &actor, sizeof(Lac_info));
	memcpy(&pkt->partner, &partner, sizeof(Lac_info));

	/* send to machines */
	lac_rx(port, pkt);

	kfree(pkt);
out:
	kfree_skb(skb);
	return;
}

static void lac_pass_frame_up(Lac_aggr *agg, struct sk_buff *skb)
{
	//Lac_warn("lac_pass_frame_up\n");
	agg->statistics.rx_packets++;
	agg->statistics.rx_bytes += skb->len;

	skb->dev = agg->dev;
	netif_receive_skb(skb);
}

void lac_port_pass_frame_up(Lac_port *p, struct sk_buff *skb)
{
	skb->dev = p->dev;
	netif_receive_skb(skb);
}

void lac_frame_collector(struct net_lac_port *port, struct sk_buff *skb)
{
	const unsigned char *dest = eth_hdr(skb)->h_dest;
	Lac_aggr *agg;

	if(!port)
	{
		kfree_skb(skb);
		return;
	}
	
	agg = port->agg;
	/* port is aggregated */
	if(agg)
	{
		Lac_warn("agg %d address 0x%02x-%02x-%02x-%02x-%02x-%02x\n", 
				agg->aggr_no, 
				agg->dev->dev_addr[0],
				agg->dev->dev_addr[1],
				agg->dev->dev_addr[2],
				agg->dev->dev_addr[3],
				agg->dev->dev_addr[4],
				agg->dev->dev_addr[5]);
		Lac_warn("dest address (from packet) 0x%02x-%02x-%02x-%02x-%02x-%02x\n", dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);

		
		//if (!memcmp(agg->dev->dev_addr, dest, ETH_ALEN) || dest[0]&1) 
		//ca fiind eu in promiscuitate, LE VREAU pe toate
		//{
			lac_pass_frame_up(agg, skb);
			return;
		//}	
	}
	else if(port->actor.state.bmask.aggregation == False)
	{	
		/* individual port */
		Lac_warn("port is individual\n");
		if (!memcmp(port->dev->dev_addr, dest, ETH_ALEN) || dest[0]&1)
		{
			lac_port_pass_frame_up(port, skb);
			return;
		}

	}

	kfree_skb(skb);
	return;
}

static int aggregator_parser(struct net_lac_port *port, struct sk_buff *skb)
{
	const unsigned char *dest = eth_hdr(skb)->h_dest;

	if (port->actor.state.bmask.aggregation == False)
	{
		return 0;
	}
	
	if (!memcmp(port->dev->dev_addr, dest, ETH_ALEN))
		skb->pkt_type = PACKET_HOST;

	lac_frame_collector(port, skb);
	return 1;
}

/**
 * Handle newly receive packet (right from the wire). 
 * Called via lac_handle_frame_hook.
 * note: already called with rcu_read_lock (preempt_disabled) 
 *
 * In LAC this is Control Parser: on receive distinguise LACPDUs from other frames, 
 * passing LACPDUs to appropriate entity and all other to aggregator. 
 */
int lac_handle_frame(struct net_lac_port *port, struct sk_buff **pskb) 
{
	struct sk_buff *skb1 = *pskb;
	struct sk_buff *skb;
	const unsigned char *dest; /* adresa mac destinatie */
	if (!skb1)
	{
		return 1;
	}
	
	skb = skb_clone(skb1, GFP_ATOMIC);
	if(!skb)
	{
		return 1;
	}
	kfree_skb(skb1);

	if (!is_valid_ether_addr(eth_hdr(skb)->h_source))
		goto err;
	
	dest = eth_hdr(skb)->h_dest;
	/* verify multicat address 
	   verify LACPDU slow protocol type
	   verify subtype for the LACP
	*/	
	if (!memcmp(dest, Slow_Protocols_Multicast, ETH_ALEN) && 
		eth_hdr(skb)->h_proto == htons((La_Short)Slow_Protocols_Ethertype))
	{
		if(*(skb_mac_header(skb) + Lacpdu_subtype_offset) == LACP_Subtype )
		{
			start_lacp(port, skb);
			return 1;
		}
	}
	else
	{
		return aggregator_parser(port, skb);
	}

err:
	kfree_skb(skb);
	return 1;
}
