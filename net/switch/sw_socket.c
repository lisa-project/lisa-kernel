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

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <net/protocol.h>
#include <net/sock.h>

#include "sw_private.h"

/* This implements the PF_SWITCH protocol family that is used by user space
 * daemons (such as cdpd) to send and receive protocol-specific raw packets.
 *
 * We prefer this over PF_PACKET and libpcap/libnet because we have to
 * detect and filter these packets anyway in the switching engine. By using
 * our custom socket implementation, we can "send" these packets to user
 * space instead of just dropping them.
 *
 * This implementation is strongly inspired from PF_PACKET implementation,
 * in net/packet/af_packet.c.
 */

DEFINE_MUTEX(sw_ioctl_mutex);

/* FIXME: maybe we should move this to a header file */
#define ETH_HDLC_CDP 0x2000
#define ETH_HDLC_VTP 0x2004


/* FIXME: shouldn't this be moved to a header file? (or we consider it private and leave it here)?
 * Custom structure that wraps the kernel "sock" struct. We use it to be
 * able to add implementation-specific data to the socket structure.
 */
struct switch_sock {
	/* struct sock has to be the first member of switch_sock to keep
	 * consistency with the rest of the kernel */
	struct sock				sk;

	/* implementation-specific fields follow */
	int						proto;			/* socket protocol number */
	struct list_head		port_chain;		/* link to port list of sockets */
	struct net_switch_port	*port;			/* required for sendmsg() */
};

/* Enqueue a sk_buff to a socket receive queue.
 */
static int sw_socket_enqueue(struct sk_buff *skb, struct net_device *dev, struct switch_sock *sw_sock) {
	struct sock *sk = &sw_sock->sk;

	if (skb->pkt_type == PACKET_LOOPBACK)
		goto skb_unhandled;

	skb->dev = dev;

	if (atomic_read(&sk->sk_rmem_alloc) + skb->truesize >=
			(unsigned)sk->sk_rcvbuf)
		goto skb_unhandled;

	if (dev->hard_header) {
		if (sk->sk_type != SOCK_DGRAM)
			skb_push(skb, skb->data - skb->mac.raw);
		else if (skb->pkt_type == PACKET_OUTGOING)
			skb_pull(skb, skb->nh.raw - skb->data);
	}

	/* clone the skb if others we're sharing it with others  */
	if (skb_shared(skb)) {
		struct sk_buff *nskb = skb_clone(skb, GFP_ATOMIC);

		if (nskb == NULL)
			goto skb_unhandled;

		skb = nskb;
	}

	skb_set_owner_r(skb, sk);
	skb->dev = NULL;
	dst_release(skb->dst);
	skb->dst = NULL;

	nf_reset(skb);

	/* queue the skb in the socket recieve queue */
	spin_lock(&sk->sk_receive_queue.lock);
	dbg("enqueuing skb=%p to sk_receive_queue=%p\n", skb, &sk->sk_receive_queue);
	__skb_queue_tail(&sk->sk_receive_queue, skb);
	spin_unlock(&sk->sk_receive_queue.lock);

	/* notify blocked reader that data is available */
	sk->sk_data_ready(sk, skb->len);
	return 1;

skb_unhandled:
	kfree_skb(skb);
	return 0;
}

static unsigned char cdp_vtp_dst[6] = {0x01, 0x00, 0x0c, 0xcc, 0xcc, 0xcc};
static unsigned char filter_paranoia[5] = {0xaa, 0x03, 0x00, 0x00, 0x0c};
/*                                          S    N C F \______  ______/
 *                                          S    e o i        \/
 *                                          A    t n e   Organization
 *                                          P    W t l    Identifier
 *                                               a r d     (CISCO)
 *                                               r o
 *                                               e l
 */

/* Socket filter: all packets are filtered through this function.
 * If we recognize a protocol we're interested in, we enqueue the
 * socket buffer to the appropriate switch socket and return non
 * zero value, so the packet doesn't get in the forwarding algorithm.
 */
int sw_socket_filter(struct sk_buff *skb, struct net_switch_port *port) {
	int handled = 0;
	struct switch_sock *sw_sk; 

	/* First identify SNAP frames. See specs/ether_frame_formats.html for
	 * details */
	if(skb->len >= port->dev->mtu || skb->data[0] != 0xaa)
		goto out;
	
#ifdef SW_SOCK_FILTER_PARANOIA
	/* Also check SSAP, Control Field, and Organization ID */
	if(memcmp(filter_paranoia, skb->data + 1, sizeof(filter_paranoia)))
		goto out;
#endif
	
	/* Both CDP and VTP frames are sent to a specific multicast address */
	if(memcmp(cdp_vtp_dst, skb->mac.raw, sizeof(cdp_vtp_dst)))
		goto out;

	/* Check for HDLC protocol type (offset 6 within LLC field) */
	switch(ntohs(*(short *)(skb->data + 6))) {
	case ETH_HDLC_CDP:
		dbg("Identified CDP frame on %s\n", port->dev->name);
		list_for_each_entry_rcu(sw_sk, &port->sock_cdp, port_chain) {
			/* Be nice and increase the usage count on this skb */
			atomic_inc(&skb->users);
			handled |= sw_socket_enqueue(skb, port->dev, sw_sk);
		}
		break;
	case ETH_HDLC_VTP:
		dbg("Identified VTP frame on %s\n", port->dev->name);
		list_for_each_entry_rcu(sw_sk, &port->sock_vtp, port_chain) {
			/* Be nice and increase the usage count on this skb */
			atomic_inc(&skb->users);
			handled |= sw_socket_enqueue(skb, port->dev, sw_sk);
			/* FIXME VTP frames must be forwarded when we are transparent */
		}
		break;
	}
out:
	return handled;
}

static inline struct switch_sock *sw_sk(struct sock *sk) {
	return (struct switch_sock *)sk;
}

/* Almost copy-paste from af_packet.c */
static void sw_sock_destruct(struct sock *sk) {
	dbg("sw_sock_destruct, sk=%p\n", sk);
	BUG_TRAP(!atomic_read(&sk->sk_rmem_alloc));
	BUG_TRAP(!atomic_read(&sk->sk_wmem_alloc));

	if (!sock_flag(sk, SOCK_DEAD)) {
		dbg("Attempt to release alive switch socket: %p\n", sk);
		return;
	}
}

static struct proto switch_proto = {
	.name = "SWITCH",
	.owner = THIS_MODULE,
	.obj_size = sizeof(struct switch_sock)
};

static const struct proto_ops sw_sock_ops;

static int sw_sock_create(struct socket *sock, int protocol) {
	struct sock *sk;
	struct switch_sock *sws;

	dbg("sw_sock_create(sock=%p), proto=%d\n", sock, protocol);
	if(!capable(CAP_NET_RAW))
		return -EPERM;
	if(sock->type != SOCK_RAW)
		return -ESOCKTNOSUPPORT;
	if(protocol)
		return -ESOCKTNOSUPPORT;

	sock->state = SS_UNCONNECTED;

	sk = sk_alloc(PF_SWITCH, GFP_KERNEL, &switch_proto, 1);
	if(sk == NULL)
		return -ENOBUFS;

	sock->ops = &sw_sock_ops;
	sock_init_data(sock, sk);

	sws = sw_sk(sk);
	sk->sk_family = PF_SWITCH;
	sws->proto = 0;

	sk->sk_destruct = sw_sock_destruct;
	dbg("sk_sock_create, created sk %p\n", sk);

	return 0;
}

static int bind_switch_port(struct switch_sock *sws, struct net_switch_port *port, int proto) {
	struct list_head *lh;

	switch(proto) {
	case ETH_P_CDP:
		lh = &port->sock_cdp;
		break;
	/* FIXME: here we should handle other protocols as well */
	default:
		return -EINVAL;
	}

	sws->proto = proto;
	sws->port = port;

	/* now the socket is ready and we can publish it to the port */
	list_add_tail_rcu(&sws->port_chain, lh);
	return 0;
}

static void unbind_switch_port(struct switch_sock *sws) {
	if(!sws->proto)
		return;
	list_del_rcu(&sws->port_chain);
	synchronize_rcu();
	sws->proto = 0;
}

static int sw_sock_release(struct socket *sock) {
	struct sock *sk = sock->sk;
	struct switch_sock *sws = sw_sk(sk);
	
	dbg("sw_sock_release(sock=%p) sk=%p\n", sock, sock->sk);

	unbind_switch_port(sws);

	/* set socket to be dead for the destructor to be called */
	sock_orphan(sk);
	sock->sk = NULL;

	/* purge tx, rx queue */
	skb_queue_purge(&sk->sk_write_queue);
	skb_queue_purge(&sk->sk_error_queue);
	skb_queue_purge(&sk->sk_receive_queue);

	/* decrement usage count */
	sock_put(sk);

	return 0;
}

static int sw_sock_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len) {
	struct net_device *dev = NULL;
	struct switch_sock *sws = sw_sk(sock->sk);
	struct sockaddr_sw *sw_addr = (struct sockaddr_sw *)uaddr;
	int err;

	dbg("sw_sock_bind, sk=%p\n", sws);

	if(addr_len < sizeof(struct sockaddr_sw))
		return -EINVAL;
	if(sw_addr->ssw_family != AF_SWITCH)
		return -EINVAL;

	dev = dev_get_by_name(sw_addr->ssw_if_name);
	if(dev == NULL)
		return -ENODEV;

	/* prevent devices from being added or removed from the switch */
	mutex_lock(&sw_ioctl_mutex);

	if(dev->sw_port == NULL) {
		err = -ENODEV;
	} else {
		unbind_switch_port(sws);
		err = bind_switch_port(sws, dev->sw_port, sw_addr->ssw_proto);
	}

	mutex_unlock(&sw_ioctl_mutex);
	dev_put(dev); /* dev_get_by_name() calls dev_hold() */
	return err;
}

/* First implementation of Linux Multilayer Switch used a hook in
 * sock_ioctl(), which is the socket-specific implementation
 * of the ioctl() generic file operation. It is referenced in
 * the socket_file_ops structure at the beginnig of socket.c.
 *
 * In sock_ioctl() we added a new branch to the switch statement:
 * our own hook to call our particular ioctl() handler. But the
 * default branch of the switch statement calls sock->ops->ioctl(),
 * which points to sw_sock_ioctl().
 *
 * Now that we have our own protocol implementation, we prefer
 * using our own ioctl() handler than hijacking the main socket
 * ioctl() handler.
 */
static int sw_sock_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg) {
	int err;
	void __user *argp = (void __user *)arg;

	mutex_lock(&sw_ioctl_mutex);
	err = sw_deviceless_ioctl(cmd, argp);
	mutex_unlock(&sw_ioctl_mutex);
	return err;
}

static int sw_sock_sendmsg(struct kiocb *iocb, struct socket *sock,
		struct msghdr *msg, size_t len) {
	struct sock *sk = sock->sk;
	struct switch_sock *sws = sw_sk(sk);
	struct sk_buff *skb;
	struct net_device *dev = NULL;
	unsigned short proto = 0;
	int err = 0;

	dbg("sw_sock_sendmsg, sk=%p\n", sk);

	if (msg->msg_name == NULL) {
		/* check if we are connected */
		if (!sws->proto)
			return -ENOTCONN;
		
		dev = sws->port->dev;
		dev_hold(dev);
	} else {
		struct sockaddr_sw *sw_addr = (struct sockaddr_sw *)msg->msg_name;

		/* verify that the protocol family is AF_SWITCH */
		if (sw_addr != NULL && sw_addr->ssw_family != AF_SWITCH)
			return -EINVAL;

		/* verify the address */
		if (msg->msg_namelen < sizeof(struct sockaddr_sw))
			return -EINVAL;
		if (msg->msg_namelen == sizeof(struct sockaddr_sw))
			proto = sw_addr->ssw_proto;

		dev = dev_get_by_name(sw_addr->ssw_if_name);
		if (dev == NULL)
			return -ENODEV;
	}
	
	dbg("sw_sock_sendmsg, proto=%x, if_name=%s\n", proto, dev->name);

	/* Since this is a raw protocol, the user is responsible for doing 
	 the fragmentation. Message size cannot exceed device mtu. */
	err = -EMSGSIZE;
	if (len > dev->mtu + dev->hard_header_len) {
		dbg("sw_sock_sendmsg, len(%d) exceeds mtu (%d)+ hard_header_len(%d)\n",
				len, dev->mtu, dev->hard_header_len);
		goto out_unlock;
	}

	err = -ENOBUFS;
	skb = sock_wmalloc(sk, len + LL_RESERVED_SPACE(dev), 0, GFP_KERNEL);
	if (skb == NULL) {
		dbg("sw_sock_sendmsg, sock_wmalloc failed\n");
		goto out_unlock;
	}
	dbg("sw_sock_sendmsg, skb allocated at %p\n", skb);

	/* Fill the socket buffer  */
	/* Save space for drivers that write hard header at Tx time (implement
	   the hard_header method)*/
	skb_reserve(skb, LL_RESERVED_SPACE(dev));
	skb->nh.raw = skb->data;

	/* Align data correctly */
	if (dev->hard_header) {
		skb->data -= dev->hard_header_len;
		skb->tail -= dev->hard_header_len;
		if (len < dev->hard_header_len)
			skb->nh.raw = skb->data;
	}

	/* Copy data from msg */
	err = memcpy_fromiovec(skb_put(skb,len), msg->msg_iov, len);
	skb->protocol = proto;
	skb->dev = dev;
	skb->priority = sk->sk_priority;
	if (err) {
		dbg("sw_sock_sendmsg, memcpy_fromiovec failed\n");
		goto out_free;
	}

	err = -ENETDOWN;
	if (!(dev->flags & IFF_UP)) {
		dbg("sw_sock_sendmsg, interface is down\n");
		goto out_free;
	}

	/* send skb */
	dbg("sw_sock_sendmsg, sending skb (len=%d)\n", len);
	dev_queue_xmit(skb);
	dev_put(dev);
	return len;

	/* Error recovery */
out_free:
	kfree_skb(skb);
out_unlock:
	if (dev)
		dev_put(dev);
	return err;
}

static int sw_sock_recvmsg(struct kiocb *iocb, struct socket *sock,
		struct msghdr *msg, size_t len, int flags) {
	struct sock *sk = sock->sk;
	int copied, err;
	struct sk_buff *skb;

	dbg("sw_sock_recvmsg, sk=%p\n", sock->sk);

	err = -EINVAL;
	if(flags & ~(MSG_DONTWAIT))
		goto out;
	
	/* Call the generic datagram receiver. This handles all sorts
	 * of horrible races and re-entrancy so we can forget about it
	 * in the protocol layers.
	 *
	 * Now it will return ENETDOWN, if device have just gone down,
	 * but then it will block.
	 */

	skb = skb_recv_datagram(sk, flags, flags & MSG_DONTWAIT, &err);

	/* An error occurred so return it. Because skb_recv_datagram() 
	 * handles the blocking we don't see and worry about blocking
	 * retries.
	 */
	if(skb == NULL) {
		dbg("skb null after returning from skb_recv_datagram\n");
		goto out;
	}

	/* We don't return any address. If we change our mind, then
	 * msg->msg_namelen should be the address length and
	 * msg->msg_name should point to the address
	 */
	msg->msg_namelen = 0;
	
	/* You lose any data beyond the buffer you gave. If it worries a
	 * user program they can ask the device for its MTU anyway.
	 */
	copied = skb->len;
	if(copied > len) {
		copied = len;
		msg->msg_flags |= MSG_TRUNC;
	}

	err = skb_copy_datagram_iovec(skb, 0, msg->msg_iov, copied);
	if (err)
		goto out_free;

	sock_recv_timestamp(msg, sk, skb);

	/* Free or return the buffer as appropriate. Again this
	 * hides all the races and re-entrancy issues from us.
	 */
	err = (flags & MSG_TRUNC) ? skb->len : copied;

out_free:
	skb_free_datagram(sk, skb);
out:
	return err;
}

static const struct proto_ops sw_sock_ops = {
	.family =		PF_SWITCH,
	.owner =		THIS_MODULE,
	.release = 		sw_sock_release,
	.bind =			sw_sock_bind,
	.connect =		sock_no_connect,
	.socketpair =	sock_no_socketpair,
	.accept =		sock_no_accept,
	.getname =		sock_no_getname,
	.poll =			datagram_poll,
	.ioctl =		sw_sock_ioctl,
	.listen =		sock_no_listen,
	.shutdown =		sock_no_shutdown,
	.setsockopt =	sock_no_setsockopt,
	.getsockopt =	sock_no_getsockopt,
	.sendmsg =		sw_sock_sendmsg,
	.recvmsg =		sw_sock_recvmsg,
	.mmap =			sock_no_mmap,
	.sendpage =		sock_no_sendpage,
};

static struct net_proto_family switch_family_ops = {
	.family =		PF_SWITCH,
	.create =		sw_sock_create,
	.owner =		THIS_MODULE,
};

void sw_sock_exit(void) {
	sock_unregister(PF_SWITCH);
	proto_unregister(&switch_proto);
	dbg("Sucessfully unregistered PF_SWITCH protocol family\n");
}

int sw_sock_init(void) {
	int err = proto_register(&switch_proto, 0);

	if(err)
		goto out;
	
	sock_register(&switch_family_ops);
	dbg("Sucessfully registered PF_SWITCH protocol family\n");
out:
	return err;
}

MODULE_ALIAS_NETPROTO(PF_SWITCH);
