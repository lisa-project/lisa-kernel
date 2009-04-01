#include <linux/list.h>
#include <linux/if.h>

#include "../lac/lac_private.h"
#include "sw_private.h"

//ADI New File 


/* Linux Link Aggregation */
#if defined(CONFIG_LAC) || defined (CONFIG_LAC_MODULE)
int (*lac_handle_frame_hook)(struct net_lac_port *p, struct sk_buff **pskb);

__inline__ int handle_lac(struct sk_buff **pskb)
{
	struct net_lac_port *port;

	if ((*pskb)->pkt_type == PACKET_LOOPBACK )
		return 0;

	port = rcu_dereference((*pskb)->dev->lac_port);
	if (port == NULL)//daca a venit pe un agregator sau pe unul care nu e in sistem, intorci zero
	{
		return 0;
	}

	if (lac_handle_frame_hook != NULL)
	{
		return lac_handle_frame_hook(port, pskb);
	}

	return 0;
}
#else
#define handle_lac(skb)	(0)
#endif

#if defined(CONFIG_LAC) || defined(CONFIG_LAC_MODULE)
EXPORT_SYMBOL(lac_handle_frame_hook);
#endif


