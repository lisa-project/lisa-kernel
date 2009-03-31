#ifndef _LAC_DEBUG_H
#define _LAC_DEBUG_H

#ifdef DEBUG
#define __dbg_static
#else
#define dbg(par...)
#define __dbg_static static
#endif

#define Lac_printd(text,par...) printk(KERN_DEBUG text, ##par)
#define Lac_printi(text,par...) printk(KERN_INFO text, ##par)

/* depends on print flag */
#define Lac_print(text,par...) if (DEB_GET_LAC_PRINT(lac_debug_info)) printk(KERN_INFO text, ##par)
#define Lac_warn(text,par...) if (DEB_GET_LAC_WARN(lac_debug_info)) printk(KERN_INFO text, ##par)

#define DEB_LAC_PRINT	0x01
#define DEB_LAC_RX_PDU	0x02
#define DEB_LAC_TX_PDU	0x04
#define DEB_LAC_STATE	0x08
#define DEB_LAC_WARN	0x10

#define DEB_GET_LAC_RX_PDU(lac_debug_info)	(lac_debug_info.db_mask & DEB_LAC_RX_PDU)
#define DEB_GET_LAC_TX_PDU(lac_debug_info)	(lac_debug_info.db_mask & DEB_LAC_TX_PDU)
#define DEB_GET_LAC_PRINT(lac_debug_info)	(lac_debug_info.db_mask & DEB_LAC_PRINT)
#define DEB_GET_LAC_WARN(lac_debug_info)	(lac_debug_info.db_mask & DEB_LAC_WARN)
#define DEB_GET_LAC_STATE(lac_debug_info)	(lac_debug_info.db_mask & DEB_LAC_STATE)

#define DEB_LAC_STATE_ON		DEB_GET_LAC_STATE(lac_debug_info)
#define DEB_LAC_RX_PDU_ON		DEB_GET_LAC_RX_PDU(lac_debug_info)
#define DEB_LAC_TX_PDU_ON		DEB_GET_LAC_TX_PDU(lac_debug_info)
#define DEB_LAC_WARN_ON			DEB_GET_LAC_WARN(lac_debug_info)

#define LA_PORT_DEB_ON(port_no)			(lac_debug_info.port_mask &  (1 << (port_no-1)))
#define LA_SET_PORT_DEB(port_no)		(lac_debug_info.port_mask |= (1 << (port_no-1)))

typedef struct lac_debug
{
	unsigned int port_mask;
	unsigned int db_mask;	/* set/clr bits for debugging*/
} Lac_debug_info;

extern void debug_flag_change(unsigned int flag_mask);
extern void debug_flag_set(unsigned int flag_mask);
extern void debug_flag_clr(unsigned int flag_mask);
extern void lac_deb_print_rx_pdu(La_Byte *pdu);
extern void lac_deb_print_rx_lacpdu(Lac_packet *lacpdu);
extern void lac_deb_print_state(Lac_port *port, char *state);
extern void dump_packet(const struct sk_buff *skb);

extern Lac_debug_info lac_debug_info;

#endif
