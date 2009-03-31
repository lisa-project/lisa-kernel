#include "lac_protocol.h"
#include "lac_private.h"
#include "lac_debug.h"

Lac_debug_info lac_debug_info;

void debug_flag_change(unsigned int flag_mask)
{
	lac_debug_info.db_mask ^= flag_mask;
}

void debug_flag_set(unsigned int flag_mask)
{
	lac_debug_info.db_mask |= (flag_mask);
}

void debug_flag_clr(unsigned int flag_mask)
{
	lac_debug_info.db_mask &= ~(flag_mask); 
}

void lac_deb_print_rx_pdu(La_Byte *pdu)
{
	La_Byte *ptr = pdu;
	La_Short word;
	
	word = ntohs(*(La_Short *)&ptr[Lacpdu_etype_offset]);
	Lac_printi("Slow protocol type: 0x%4x\n", word);

	word = ntohs(*(La_Short *)&ptr[Lacpdu_actor_sys_pri_offset]);
	Lac_printi("pdu_actor_sy_pri : %d\n", word);

	Lac_printi("pdu_actor_sys_id : 0x%02x-%02x-%02x-%02x-%02x-%02x\n",
        ptr[Lacpdu_actor_sys_id_offset + 0],
		ptr[Lacpdu_actor_sys_id_offset + 1],
		ptr[Lacpdu_actor_sys_id_offset + 2],
		ptr[Lacpdu_actor_sys_id_offset + 3],
		ptr[Lacpdu_actor_sys_id_offset + 4],
		ptr[Lacpdu_actor_sys_id_offset + 5]);
   
	word = ntohs(*(La_Short *)&ptr[Lacpdu_actor_key_offset]);
	Lac_printi("pdu_actor_key : %d\n", word);

	word = ntohs(*(La_Short *)&ptr[Lacpdu_actor_port_pri_offset]);
	Lac_printi("pdu_actor_pri : %d\n", word);

	word = ntohs(*(La_Short *)&ptr[Lacpdu_actor_port_offset]);
	Lac_printi("pdu_actor_port : %d\n", word);

	Lac_printi("pdu_actor_state: 0x%2x\n", ptr[Lacpdu_actor_state_offset]);
}

void lac_deb_print_rx_lacpdu(Lac_packet *pkt)
{
	//Lac_printi("Slow protocol type: 0x%4x\n", pkt->subtype);
	//Lac_printi("lacpdu_actor_sys_pri : %d\n", pkt->actor_sys_pri);
	Lac_printi("lacpdu_actor_sys_id : 0x%02x-%02x-%02x-%02x-%02x-%02x\n",
		pkt->actor.system_id[0],
		pkt->actor.system_id[1],
		pkt->actor.system_id[2],
		pkt->actor.system_id[3],
		pkt->actor.system_id[4],
		pkt->actor.system_id[5]);

	Lac_printi("lacpdu_actor_key : %d\n", pkt->actor.key);

	Lac_printi("lacpdu_actor_pri : %d\n", pkt->actor.port_priority);

	Lac_printi("lacpdu_actor_port : %d\n", pkt->actor.port_no);

	Lac_printi("lacpdu_actor_state: 0x%2x\n", pkt->actor.state.state);
}

void lac_deb_print_state(Lac_port *port, char *state)
{
	if(LA_PORT_DEB_ON(port->port_no))
		Lac_printi("STATE:%s, port:%d, rx_state: %d, mux_state:%d, selected:%d, agg: 0x%p\n", state, port->port_no, port->rxm, port->mux_state, port->selected, port->agg);
}

void dump_packet(const struct sk_buff *skb) {
	int i;
	
	printk(KERN_INFO "dev=%s: proto=0x%hx mac_len=%d "
			"head=0x%p data=0x%p tail=0x%p end=0x%p mac=0x%p\n",
			skb->dev->name, ntohs(skb->protocol), skb->mac_len,
			skb->head, skb->data, skb->tail, skb->end, skb->mac.raw);
	printk("MAC dump: ");
	if(skb->mac.raw)
		for(i = 0; i < skb->mac_len; i++)
			printk("0x%x ", skb->mac.raw[i]);
	printk("\nDATA dump: ");
	if(skb->data)
		for(i = 0; i < 4; i++)
			printk("0x%x ", skb->data[i]);
	printk("\n");
}
