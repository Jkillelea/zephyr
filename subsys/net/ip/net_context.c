/** @file
 * @brief Network context API
 *
 * An API for applications to define a network connection.
 */

/*
 * Copyright (c) 2016 Intel Corporation
 * Copyright (c) 2021 Nordic Semiconductor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(net_ctx, CONFIG_NET_CONTEXT_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/random/rand32.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_offload.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/socketcan.h>
#include <zephyr/net/ieee802154.h>

#include "connection.h"
#include "net_private.h"

#include "ipv6.h"
#include "ipv4.h"
#include "udp_internal.h"
#include "tcp_internal.h"
#include "net_stats.h"

#if defined(CONFIG_NET_TCP)
#include "tcp.h"
#endif

#ifndef EPFNOSUPPORT
/* Some old versions of newlib haven't got this defined in errno.h,
 * Just use EPROTONOSUPPORT in this case
 */
#define EPFNOSUPPORT EPROTONOSUPPORT
#endif

#define PKT_WAIT_TIME K_SECONDS(1)

#define NET_MAX_CONTEXT CONFIG_NET_MAX_CONTEXTS

static struct net_context contexts[NET_MAX_CONTEXT];

/* We need to lock the contexts array as these APIs are typically called
 * from applications which are usually run in task context.
 */
static struct k_sem contexts_lock;

#if defined(CONFIG_NET_UDP) || defined(CONFIG_NET_TCP)
static int check_used_port(enum net_ip_protocol proto,
			   uint16_t local_port,
			   const struct sockaddr *local_addr)

{
	int i;

	for (i = 0; i < NET_MAX_CONTEXT; i++) {
		if (!net_context_is_used(&contexts[i])) {
			continue;
		}

		if (!(net_context_get_proto(&contexts[i]) == proto &&
		      net_sin((struct sockaddr *)&
			      contexts[i].local)->sin_port == local_port)) {
			continue;
		}

		if (IS_ENABLED(CONFIG_NET_IPV6) &&
		    local_addr->sa_family == AF_INET6) {
			if (net_sin6_ptr(&contexts[i].local)->sin6_addr == NULL) {
				continue;
			}

			if (net_ipv6_addr_cmp(
				    net_sin6_ptr(&contexts[i].local)->
							     sin6_addr,
				    &((struct sockaddr_in6 *)
				      local_addr)->sin6_addr)) {
				return -EEXIST;
			}
		} else if (IS_ENABLED(CONFIG_NET_IPV4) &&
			   local_addr->sa_family == AF_INET) {
			if (net_sin_ptr(&contexts[i].local)->sin_addr == NULL) {
				continue;
			}

			if (net_ipv4_addr_cmp(
				    net_sin_ptr(&contexts[i].local)->
							      sin_addr,
				    &((struct sockaddr_in *)
				      local_addr)->sin_addr)) {
				return -EEXIST;
			}
		}
	}

	return 0;
}

static uint16_t find_available_port(struct net_context *context,
				    const struct sockaddr *addr)
{
	uint16_t local_port;

	do {
		local_port = sys_rand32_get() | 0x8000;
	} while (check_used_port(net_context_get_proto(context),
				 htons(local_port), addr) == -EEXIST);

	return htons(local_port);
}
#else
#define check_used_port(...) 0
#define find_available_port(...) 0
#endif

bool net_context_port_in_use(enum net_ip_protocol proto,
			   uint16_t local_port,
			   const struct sockaddr *local_addr)
{
	return check_used_port(proto, htons(local_port), local_addr) != 0;
}

#if defined(CONFIG_NET_CONTEXT_CHECK)
static int net_context_check(sa_family_t family, enum net_sock_type type,
			     uint16_t proto, struct net_context **context)
{
	switch (family) {
	case AF_INET:
	case AF_INET6:
		if (family == AF_INET && !IS_ENABLED(CONFIG_NET_IPV4)) {
			NET_DBG("IPv4 disabled");
			return -EPFNOSUPPORT;
		}
		if (family == AF_INET6 && !IS_ENABLED(CONFIG_NET_IPV6)) {
			NET_DBG("IPv6 disabled");
			return -EPFNOSUPPORT;
		}
		if (!IS_ENABLED(CONFIG_NET_UDP)) {
			if (type == SOCK_DGRAM) {
				NET_DBG("DGRAM socket type disabled.");
				return -EPROTOTYPE;
			}
			if (proto == IPPROTO_UDP) {
				NET_DBG("UDP disabled");
				return -EPROTONOSUPPORT;
			}
		}
		if (!IS_ENABLED(CONFIG_NET_TCP)) {
			if (type == SOCK_STREAM) {
				NET_DBG("STREAM socket type disabled.");
				return -EPROTOTYPE;
			}
			if (proto == IPPROTO_TCP) {
				NET_DBG("TCP disabled");
				return -EPROTONOSUPPORT;
			}
		}
		switch (type) {
		case SOCK_DGRAM:
			if (proto != IPPROTO_UDP) {
				NET_DBG("Context type and protocol mismatch,"
					" type %d proto %d", type, proto);
				return -EPROTONOSUPPORT;
			}
			break;
		case SOCK_STREAM:
			if (proto != IPPROTO_TCP) {
				NET_DBG("Context type and protocol mismatch,"
					" type %d proto %d", type, proto);
				return -EPROTONOSUPPORT;
			}
			break;
		case SOCK_RAW:
			break;
		default:
			NET_DBG("Unknown context type.");
			return -EPROTOTYPE;
		}
		break;

	case AF_PACKET:
		if (!IS_ENABLED(CONFIG_NET_SOCKETS_PACKET)) {
			NET_DBG("AF_PACKET disabled");
			return -EPFNOSUPPORT;
		}
		if (type != SOCK_RAW && type != SOCK_DGRAM) {
			NET_DBG("AF_PACKET only supports RAW and DGRAM socket "
				"types.");
			return -EPROTOTYPE;
		}
		break;

	case AF_CAN:
		if (!IS_ENABLED(CONFIG_NET_SOCKETS_CAN)) {
			NET_DBG("AF_CAN disabled");
			return -EPFNOSUPPORT;
		}
		if (type != SOCK_RAW) {
			NET_DBG("AF_CAN only supports RAW socket type.");
			return -EPROTOTYPE;
		}
		if (proto != CAN_RAW) {
			NET_DBG("AF_CAN only supports RAW_CAN protocol.");
			return -EPROTOTYPE;
		}
		break;

	default:
		NET_DBG("Unknown address family %d", family);
		return -EAFNOSUPPORT;
	}

	if (!context) {
		NET_DBG("Invalid context");
		return -EINVAL;
	}

	return 0;
}
#endif /* CONFIG_NET_CONTEXT_CHECK */

int net_context_get(sa_family_t family, enum net_sock_type type, uint16_t proto,
		    struct net_context **context)
{
	int i, ret;

	if (IS_ENABLED(CONFIG_NET_CONTEXT_CHECK)) {
		ret = net_context_check(family, type, proto, context);
		if (ret < 0) {
			return ret;
		}
	}

	k_sem_take(&contexts_lock, K_FOREVER);

	ret = -ENOENT;
	for (i = 0; i < NET_MAX_CONTEXT; i++) {
		if (net_context_is_used(&contexts[i])) {
			continue;
		}

		memset(&contexts[i], 0, sizeof(contexts[i]));
		/* FIXME - Figure out a way to get the correct network interface
		 * as it is not known at this point yet.
		 */
		if (!net_if_is_ip_offloaded(net_if_get_default())
			&& proto == IPPROTO_TCP) {
			if (net_tcp_get(&contexts[i]) < 0) {
				break;
			}
		}

		contexts[i].iface = -1;
		contexts[i].flags = 0U;
		atomic_set(&contexts[i].refcount, 1);

		net_context_set_family(&contexts[i], family);
		net_context_set_type(&contexts[i], type);
		net_context_set_proto(&contexts[i], proto);

#if defined(CONFIG_NET_CONTEXT_RCVTIMEO)
		contexts[i].options.rcvtimeo = K_FOREVER;
#endif
#if defined(CONFIG_NET_CONTEXT_SNDTIMEO)
		contexts[i].options.sndtimeo = K_FOREVER;
#endif

		if (IS_ENABLED(CONFIG_NET_IP)) {
			(void)memset(&contexts[i].remote, 0, sizeof(struct sockaddr));
			(void)memset(&contexts[i].local, 0, sizeof(struct sockaddr_ptr));

			if (IS_ENABLED(CONFIG_NET_IPV6) && family == AF_INET6) {
				struct sockaddr_in6 *addr6 =
					(struct sockaddr_in6 *)&contexts[i].local;
				addr6->sin6_port =
					find_available_port(&contexts[i], (struct sockaddr *)addr6);

				if (!addr6->sin6_port) {
					ret = -EADDRINUSE;
					break;
				}
			}
			if (IS_ENABLED(CONFIG_NET_IPV4) && family == AF_INET) {
				struct sockaddr_in *addr = (struct sockaddr_in *)&contexts[i].local;

				addr->sin_port =
					find_available_port(&contexts[i], (struct sockaddr *)addr);

				if (!addr->sin_port) {
					ret = -EADDRINUSE;
					break;
				}
			}
		}

		if (IS_ENABLED(CONFIG_NET_CONTEXT_SYNC_RECV)) {
			k_sem_init(&contexts[i].recv_data_wait, 1, K_SEM_MAX_LIMIT);
		}

		k_mutex_init(&contexts[i].lock);

		contexts[i].flags |= NET_CONTEXT_IN_USE;
		*context = &contexts[i];

		ret = 0;
		break;
	}

	k_sem_give(&contexts_lock);

	if (ret < 0) {
		return ret;
	}

	/* FIXME - Figure out a way to get the correct network interface
	 * as it is not known at this point yet.
	 */
	if (IS_ENABLED(CONFIG_NET_OFFLOAD) && net_if_is_ip_offloaded(net_if_get_default())) {
		ret = net_offload_get(net_if_get_default(), family, type, proto, context);
		if (ret < 0) {
			(*context)->flags &= ~NET_CONTEXT_IN_USE;
			*context = NULL;
			return ret;
		}
	}

	return 0;
}

int net_context_ref(struct net_context *context)
{
	int old_rc = atomic_inc(&context->refcount);

	return old_rc + 1;
}

int net_context_unref(struct net_context *context)
{
	int old_rc = atomic_dec(&context->refcount);

	if (old_rc != 1) {
		return old_rc - 1;
	}

	k_mutex_lock(&context->lock, K_FOREVER);

	if (context->conn_handler) {
		if (IS_ENABLED(CONFIG_NET_TCP) || IS_ENABLED(CONFIG_NET_UDP) ||
		    IS_ENABLED(CONFIG_NET_SOCKETS_CAN) ||
		    IS_ENABLED(CONFIG_NET_SOCKETS_PACKET)) {
			net_conn_unregister(context->conn_handler);
		}

		context->conn_handler = NULL;
	}

	net_context_set_state(context, NET_CONTEXT_UNCONNECTED);

	context->flags &= ~NET_CONTEXT_IN_USE;

	NET_DBG("Context %p released", context);

	k_mutex_unlock(&context->lock);

	return 0;
}

int net_context_put(struct net_context *context)
{
	int ret = 0;

	NET_ASSERT(context);

	if (!PART_OF_ARRAY(contexts, context)) {
		return -EINVAL;
	}

	k_mutex_lock(&context->lock, K_FOREVER);

	if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
	    net_if_is_ip_offloaded(net_context_get_iface(context))) {
		context->flags &= ~NET_CONTEXT_IN_USE;
		ret = net_offload_put(net_context_get_iface(context), context);
		goto unlock;
	}

	context->connect_cb = NULL;
	context->recv_cb = NULL;
	context->send_cb = NULL;

	/* net_tcp_put() will handle decrementing refcount on stack's behalf */
	net_tcp_put(context);

	/* Decrement refcount on user app's behalf */
	net_context_unref(context);

unlock:
	k_mutex_unlock(&context->lock);

	return ret;
}

/* If local address is not bound, bind it to INADDR_ANY and random port. */
static int bind_default(struct net_context *context)
{
	sa_family_t family = net_context_get_family(context);

	if (IS_ENABLED(CONFIG_NET_IPV6) && family == AF_INET6) {
		struct sockaddr_in6 addr6;

		if (net_sin6_ptr(&context->local)->sin6_addr) {
			return 0;
		}

		addr6.sin6_family = AF_INET6;
		memcpy(&addr6.sin6_addr, net_ipv6_unspecified_address(),
		       sizeof(addr6.sin6_addr));
		addr6.sin6_port =
			find_available_port(context,
					    (struct sockaddr *)&addr6);

		return net_context_bind(context, (struct sockaddr *)&addr6,
					sizeof(addr6));
	}

	if (IS_ENABLED(CONFIG_NET_IPV4) && family == AF_INET) {
		struct sockaddr_in addr4;

		if (net_sin_ptr(&context->local)->sin_addr) {
			return 0;
		}

		addr4.sin_family = AF_INET;
		addr4.sin_addr.s_addr = INADDR_ANY;
		addr4.sin_port =
			find_available_port(context,
					    (struct sockaddr *)&addr4);

		return net_context_bind(context, (struct sockaddr *)&addr4,
					sizeof(addr4));
	}

	if (IS_ENABLED(CONFIG_NET_SOCKETS_PACKET) && family == AF_PACKET) {
		struct sockaddr_ll ll_addr;

		if (net_sll_ptr(&context->local)->sll_addr) {
			return 0;
		}

		ll_addr.sll_family = AF_PACKET;
		ll_addr.sll_protocol = htons(ETH_P_ALL);
		ll_addr.sll_ifindex = net_if_get_by_iface(net_if_get_default());

		return net_context_bind(context, (struct sockaddr *)&ll_addr,
					sizeof(ll_addr));
	}

	if (IS_ENABLED(CONFIG_NET_SOCKETS_CAN) && family == AF_CAN) {
		struct sockaddr_can can_addr;

		if (context->iface >= 0) {
			return 0;
		} else {
#if defined(CONFIG_NET_L2_CANBUS_RAW)
			struct net_if *iface;

			iface = net_if_get_first_by_type(
						&NET_L2_GET_NAME(CANBUS_RAW));
			if (!iface) {
				return -ENOENT;
			}

			can_addr.can_ifindex = net_if_get_by_iface(iface);
			context->iface = can_addr.can_ifindex;
#else
			return -ENOENT;
#endif
		}

		can_addr.can_family = AF_CAN;

		return net_context_bind(context, (struct sockaddr *)&can_addr,
					sizeof(can_addr));
	}

	return -EINVAL;
}

int net_context_bind(struct net_context *context, const struct sockaddr *addr,
		     socklen_t addrlen)
{
	int ret;

	NET_ASSERT(addr);
	NET_ASSERT(PART_OF_ARRAY(contexts, context));

	/* If we already have connection handler, then it effectively
	 * means that it's already bound to an interface/port, and we
	 * don't support rebinding connection to new address/port in
	 * the code below.
	 * TODO: Support rebinding.
	 */
	if (context->conn_handler) {
		return -EISCONN;
	}

	if (IS_ENABLED(CONFIG_NET_IPV6) && addr->sa_family == AF_INET6) {
		struct net_if *iface = NULL;
		struct in6_addr *ptr;
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;

		if (addrlen < sizeof(struct sockaddr_in6)) {
			return -EINVAL;
		}

		if (net_context_is_bound_to_iface(context)) {
			iface = net_context_get_iface(context);
		}

		if (net_ipv6_is_addr_mcast(&addr6->sin6_addr)) {
			struct net_if_mcast_addr *maddr;

			maddr = net_if_ipv6_maddr_lookup(&addr6->sin6_addr,
							 &iface);
			if (!maddr) {
				return -ENOENT;
			}

			ptr = &maddr->address.in6_addr;

		} else if (net_ipv6_is_addr_unspecified(&addr6->sin6_addr)) {
			if (iface == NULL) {
				iface = net_if_ipv6_select_src_iface(
					&net_sin6(&context->remote)->sin6_addr);
			}

			ptr = (struct in6_addr *)net_ipv6_unspecified_address();
		} else {
			struct net_if_addr *ifaddr;

			ifaddr = net_if_ipv6_addr_lookup(
					&addr6->sin6_addr,
					iface == NULL ? &iface : NULL);
			if (!ifaddr) {
				return -ENOENT;
			}

			ptr = &ifaddr->address.in6_addr;
		}

		if (!iface) {
			NET_ERR("Cannot bind to %s",
				net_sprint_ipv6_addr(&addr6->sin6_addr));

			return -EADDRNOTAVAIL;
		}

		if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
		    net_if_is_ip_offloaded(iface)) {
			net_context_set_iface(context, iface);

			return net_offload_bind(iface,
						context,
						addr,
						addrlen);
		}

		k_mutex_lock(&context->lock, K_FOREVER);

		ret = 0;

		net_context_set_iface(context, iface);

		net_sin6_ptr(&context->local)->sin6_family = AF_INET6;
		net_sin6_ptr(&context->local)->sin6_addr = ptr;
		if (addr6->sin6_port) {
			ret = check_used_port(AF_INET6, addr6->sin6_port,
					      addr);
			if (!ret) {
				net_sin6_ptr(&context->local)->sin6_port =
					addr6->sin6_port;
			} else {
				NET_ERR("Port %d is in use!",
					ntohs(addr6->sin6_port));
				goto unlock_ipv6;
			}
		} else {
			addr6->sin6_port =
				net_sin6_ptr(&context->local)->sin6_port;
		}

		NET_DBG("Context %p binding to %s [%s]:%d iface %d (%p)",
			context,
			net_proto2str(AF_INET6,
				      net_context_get_proto(context)),
			net_sprint_ipv6_addr(ptr),
			ntohs(addr6->sin6_port),
			net_if_get_by_iface(iface), iface);

	unlock_ipv6:
		k_mutex_unlock(&context->lock);

		return ret;
	}

	if (IS_ENABLED(CONFIG_NET_IPV4) && addr->sa_family == AF_INET) {
		struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
		struct net_if *iface = NULL;
		struct net_if_addr *ifaddr;
		struct in_addr *ptr;

		if (addrlen < sizeof(struct sockaddr_in)) {
			return -EINVAL;
		}

		if (net_context_is_bound_to_iface(context)) {
			iface = net_context_get_iface(context);
		}

		if (net_ipv4_is_addr_mcast(&addr4->sin_addr)) {
			struct net_if_mcast_addr *maddr;

			maddr = net_if_ipv4_maddr_lookup(&addr4->sin_addr,
							 &iface);
			if (!maddr) {
				return -ENOENT;
			}

			ptr = &maddr->address.in_addr;

		} else if (addr4->sin_addr.s_addr == INADDR_ANY) {
			if (iface == NULL) {
				iface = net_if_ipv4_select_src_iface(
					&net_sin(&context->remote)->sin_addr);
			}

			ptr = (struct in_addr *)net_ipv4_unspecified_address();
		} else {
			ifaddr = net_if_ipv4_addr_lookup(
					&addr4->sin_addr,
					iface == NULL ? &iface : NULL);
			if (!ifaddr) {
				return -ENOENT;
			}

			ptr = &ifaddr->address.in_addr;
		}

		if (!iface) {
			NET_ERR("Cannot bind to %s",
				net_sprint_ipv4_addr(&addr4->sin_addr));

			return -EADDRNOTAVAIL;
		}

		if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
		    net_if_is_ip_offloaded(iface)) {
			net_context_set_iface(context, iface);

			return net_offload_bind(iface,
						context,
						addr,
						addrlen);
		}

		k_mutex_lock(&context->lock, K_FOREVER);

		ret = 0;

		net_context_set_iface(context, iface);

		net_sin_ptr(&context->local)->sin_family = AF_INET;
		net_sin_ptr(&context->local)->sin_addr = ptr;
		if (addr4->sin_port) {
			ret = check_used_port(AF_INET, addr4->sin_port,
					      addr);
			if (!ret) {
				net_sin_ptr(&context->local)->sin_port =
					addr4->sin_port;
			} else {
				NET_ERR("Port %d is in use!",
					ntohs(addr4->sin_port));
				goto unlock_ipv4;
			}
		} else {
			addr4->sin_port =
				net_sin_ptr(&context->local)->sin_port;
		}

		NET_DBG("Context %p binding to %s %s:%d iface %d (%p)",
			context,
			net_proto2str(AF_INET,
				      net_context_get_proto(context)),
			net_sprint_ipv4_addr(ptr),
			ntohs(addr4->sin_port),
			net_if_get_by_iface(iface), iface);

	unlock_ipv4:
		k_mutex_unlock(&context->lock);

		return ret;
	}

	if (IS_ENABLED(CONFIG_NET_SOCKETS_PACKET) &&
	    addr->sa_family == AF_PACKET) {
		struct sockaddr_ll *ll_addr = (struct sockaddr_ll *)addr;
		struct net_if *iface = NULL;

		if (addrlen < sizeof(struct sockaddr_ll)) {
			return -EINVAL;
		}

		if (ll_addr->sll_ifindex < 0) {
			return -EINVAL;
		}

		iface = net_if_get_by_index(ll_addr->sll_ifindex);
		if (!iface) {
			NET_ERR("Cannot bind to interface index %d",
				ll_addr->sll_ifindex);
			return -EADDRNOTAVAIL;
		}

		if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
		    net_if_is_ip_offloaded(iface)) {
			net_context_set_iface(context, iface);

			return net_offload_bind(iface,
						context,
						addr,
						addrlen);
		}

		k_mutex_lock(&context->lock, K_FOREVER);

		net_context_set_iface(context, iface);

		net_sll_ptr(&context->local)->sll_family = AF_PACKET;
		net_sll_ptr(&context->local)->sll_ifindex =
			ll_addr->sll_ifindex;
		net_sll_ptr(&context->local)->sll_protocol =
			ll_addr->sll_protocol;

		net_if_lock(iface);
		net_sll_ptr(&context->local)->sll_addr =
			net_if_get_link_addr(iface)->addr;
		net_sll_ptr(&context->local)->sll_halen =
			net_if_get_link_addr(iface)->len;
		net_if_unlock(iface);

		NET_DBG("Context %p bind to type 0x%04x iface[%d] %p addr %s",
			context, htons(net_context_get_proto(context)),
			ll_addr->sll_ifindex, iface,
			net_sprint_ll_addr(
				net_sll_ptr(&context->local)->sll_addr,
				net_sll_ptr(&context->local)->sll_halen));

		k_mutex_unlock(&context->lock);

		return 0;
	}

	if (IS_ENABLED(CONFIG_NET_SOCKETS_CAN) && addr->sa_family == AF_CAN) {
		struct sockaddr_can *can_addr = (struct sockaddr_can *)addr;
		struct net_if *iface = NULL;

		if (addrlen < sizeof(struct sockaddr_can)) {
			return -EINVAL;
		}

		if (can_addr->can_ifindex < 0) {
			return -EINVAL;
		}

		iface = net_if_get_by_index(can_addr->can_ifindex);
		if (!iface) {
			NET_ERR("Cannot bind to interface index %d",
				can_addr->can_ifindex);
			return -EADDRNOTAVAIL;
		}

		if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
		    net_if_is_ip_offloaded(iface)) {
			net_context_set_iface(context, iface);

			return net_offload_bind(iface,
						context,
						addr,
						addrlen);
		}

		k_mutex_lock(&context->lock, K_FOREVER);

		net_context_set_iface(context, iface);
		net_context_set_family(context, AF_CAN);

		net_can_ptr(&context->local)->can_family = AF_CAN;
		net_can_ptr(&context->local)->can_ifindex =
			can_addr->can_ifindex;

		NET_DBG("Context %p binding to %d iface[%d] %p",
			context, net_context_get_proto(context),
			can_addr->can_ifindex, iface);

		k_mutex_unlock(&context->lock);

		return 0;
	}

	return -EINVAL;
}

static inline struct net_context *find_context(void *conn_handler)
{
	int i;

	for (i = 0; i < NET_MAX_CONTEXT; i++) {
		if (!net_context_is_used(&contexts[i])) {
			continue;
		}

		if (contexts[i].conn_handler == conn_handler) {
			return &contexts[i];
		}
	}

	return NULL;
}

int net_context_listen(struct net_context *context, int backlog)
{
	ARG_UNUSED(backlog);

	NET_ASSERT(PART_OF_ARRAY(contexts, context));

	if (!net_context_is_used(context)) {
		return -EBADF;
	}

	if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
	    net_if_is_ip_offloaded(net_context_get_iface(context))) {
		return net_offload_listen(net_context_get_iface(context),
					  context, backlog);
	}

	k_mutex_lock(&context->lock, K_FOREVER);

	if (net_tcp_listen(context) >= 0) {
		k_mutex_unlock(&context->lock);
		return 0;
	}

	k_mutex_unlock(&context->lock);

	return -EOPNOTSUPP;
}

#if defined(CONFIG_NET_IPV4)
int net_context_create_ipv4_new(struct net_context *context,
				struct net_pkt *pkt,
				const struct in_addr *src,
				const struct in_addr *dst)
{
	if (!src) {
		NET_ASSERT(((
			struct sockaddr_in_ptr *)&context->local)->sin_addr);

		src = ((struct sockaddr_in_ptr *)&context->local)->sin_addr;
	}

	if (net_ipv4_is_addr_unspecified(src)
	    || net_ipv4_is_addr_mcast(src)) {
		src = net_if_ipv4_select_src_addr(net_pkt_iface(pkt),
						  (struct in_addr *)dst);
		/* If src address is still unspecified, do not create pkt */
		if (net_ipv4_is_addr_unspecified(src)) {
			NET_DBG("DROP: src addr is unspecified");
			return -EINVAL;
		}
	}

	net_pkt_set_ipv4_ttl(pkt, net_context_get_ipv4_ttl(context));
#if defined(CONFIG_NET_CONTEXT_DSCP_ECN)
	net_pkt_set_ip_dscp(pkt, net_ipv4_get_dscp(context->options.dscp_ecn));
	net_pkt_set_ip_ecn(pkt, net_ipv4_get_ecn(context->options.dscp_ecn));
	/* Direct priority takes precedence over DSCP */
	if (!IS_ENABLED(CONFIG_NET_CONTEXT_PRIORITY)) {
		net_pkt_set_priority(pkt, net_ipv4_dscp_to_priority(
			net_ipv4_get_dscp(context->options.dscp_ecn)));
	}
#endif

	return net_ipv4_create(pkt, src, dst);
}
#endif /* CONFIG_NET_IPV4 */

#if defined(CONFIG_NET_IPV6)
int net_context_create_ipv6_new(struct net_context *context,
				struct net_pkt *pkt,
				const struct in6_addr *src,
				const struct in6_addr *dst)
{
	if (!src) {
		NET_ASSERT(((
			struct sockaddr_in6_ptr *)&context->local)->sin6_addr);

		src = ((struct sockaddr_in6_ptr *)&context->local)->sin6_addr;
	}

	if (net_ipv6_is_addr_unspecified(src)
	    || net_ipv6_is_addr_mcast(src)) {
		src = net_if_ipv6_select_src_addr(net_pkt_iface(pkt),
						  (struct in6_addr *)dst);
	}

	net_pkt_set_ipv6_hop_limit(pkt,
				   net_context_get_ipv6_hop_limit(context));
#if defined(CONFIG_NET_CONTEXT_DSCP_ECN)
	net_pkt_set_ip_dscp(pkt, net_ipv6_get_dscp(context->options.dscp_ecn));
	net_pkt_set_ip_ecn(pkt, net_ipv6_get_ecn(context->options.dscp_ecn));
	/* Direct priority takes precedence over DSCP */
	if (!IS_ENABLED(CONFIG_NET_CONTEXT_PRIORITY)) {
		net_pkt_set_priority(pkt, net_ipv6_dscp_to_priority(
			net_ipv6_get_dscp(context->options.dscp_ecn)));
	}
#endif

	return net_ipv6_create(pkt, src, dst);
}
#endif /* CONFIG_NET_IPV6 */

int net_context_connect(struct net_context *context,
			const struct sockaddr *addr,
			socklen_t addrlen,
			net_context_connect_cb_t cb,
			k_timeout_t timeout,
			void *user_data)
{
	struct sockaddr *laddr = NULL;
	struct sockaddr local_addr __unused;
	uint16_t lport, rport;
	int ret;

	NET_ASSERT(addr);
	NET_ASSERT(PART_OF_ARRAY(contexts, context));

	k_mutex_lock(&context->lock, K_FOREVER);

	if (net_context_get_state(context) == NET_CONTEXT_CONNECTING) {
		ret = -EALREADY;
		goto unlock;
	}

	if (!net_context_is_used(context)) {
		ret = -EBADF;
		goto unlock;
	}

	if (addr->sa_family != net_context_get_family(context)) {
		NET_ASSERT(addr->sa_family == net_context_get_family(context),
			   "Family mismatch %d should be %d",
			   addr->sa_family,
			   net_context_get_family(context));
		ret = -EINVAL;
		goto unlock;
	}

	if (IS_ENABLED(CONFIG_NET_SOCKETS_PACKET) &&
	    addr->sa_family == AF_PACKET) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}

	if (net_context_get_state(context) == NET_CONTEXT_LISTENING) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}

	if (IS_ENABLED(CONFIG_NET_IPV6) &&
	    net_context_get_family(context) == AF_INET6) {
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)
							&context->remote;

		if (addrlen < sizeof(struct sockaddr_in6)) {
			ret = -EINVAL;
			goto unlock;
		}

		if (net_context_get_proto(context) == IPPROTO_TCP &&
		    net_ipv6_is_addr_mcast(&addr6->sin6_addr)) {
			ret = -EADDRNOTAVAIL;
			goto unlock;
		}

		memcpy(&addr6->sin6_addr, &net_sin6(addr)->sin6_addr,
		       sizeof(struct in6_addr));

		addr6->sin6_port = net_sin6(addr)->sin6_port;
		addr6->sin6_family = AF_INET6;

		if (!net_ipv6_is_addr_unspecified(&addr6->sin6_addr)) {
			context->flags |= NET_CONTEXT_REMOTE_ADDR_SET;
		} else {
			context->flags &= ~NET_CONTEXT_REMOTE_ADDR_SET;
		}

		rport = addr6->sin6_port;

		/* The binding must be done after we have set the remote
		 * address but before checking the local address. Otherwise
		 * the laddr might not be set properly which would then cause
		 * issues when doing net_tcp_connect(). This issue was seen
		 * with socket tests and when connecting to loopback interface.
		 */
		ret = bind_default(context);
		if (ret) {
			goto unlock;
		}

		NET_ASSERT(net_sin6_ptr(&context->local)->sin6_addr != NULL);

		net_sin6_ptr(&context->local)->sin6_family = AF_INET6;
		net_sin6(&local_addr)->sin6_family = AF_INET6;
		net_sin6(&local_addr)->sin6_port = lport =
			net_sin6((struct sockaddr *)&context->local)->sin6_port;
		net_ipaddr_copy(&net_sin6(&local_addr)->sin6_addr,
				net_sin6_ptr(&context->local)->sin6_addr);

		laddr = &local_addr;
	} else if (IS_ENABLED(CONFIG_NET_IPV4) &&
		   net_context_get_family(context) == AF_INET) {
		struct sockaddr_in *addr4 = (struct sockaddr_in *)
							&context->remote;

		if (addrlen < sizeof(struct sockaddr_in)) {
			ret = -EINVAL;
			goto unlock;
		}

		/* FIXME - Add multicast and broadcast address check */

		memcpy(&addr4->sin_addr, &net_sin(addr)->sin_addr,
		       sizeof(struct in_addr));

		addr4->sin_port = net_sin(addr)->sin_port;
		addr4->sin_family = AF_INET;

		if (addr4->sin_addr.s_addr) {
			context->flags |= NET_CONTEXT_REMOTE_ADDR_SET;
		} else {
			context->flags &= ~NET_CONTEXT_REMOTE_ADDR_SET;
		}

		rport = addr4->sin_port;

		ret = bind_default(context);
		if (ret) {
			goto unlock;
		}

		NET_ASSERT(net_sin_ptr(&context->local)->sin_addr != NULL);

		net_sin_ptr(&context->local)->sin_family = AF_INET;
		net_sin(&local_addr)->sin_family = AF_INET;
		net_sin(&local_addr)->sin_port = lport =
			net_sin((struct sockaddr *)&context->local)->sin_port;
		net_ipaddr_copy(&net_sin(&local_addr)->sin_addr,
				net_sin_ptr(&context->local)->sin_addr);

		laddr = &local_addr;
	} else {
		ret = -EINVAL; /* Not IPv4 or IPv6 */
		goto unlock;
	}

	if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
	    net_if_is_ip_offloaded(net_context_get_iface(context))) {
		ret = net_offload_connect(
			net_context_get_iface(context),
			context,
			addr,
			addrlen,
			cb,
			timeout,
			user_data);
		goto unlock;
	}

	if (IS_ENABLED(CONFIG_NET_UDP) &&
	    net_context_get_type(context) == SOCK_DGRAM) {
		if (cb) {
			cb(context, 0, user_data);
		}

		ret = 0;
	} else if (IS_ENABLED(CONFIG_NET_TCP) &&
		   net_context_get_type(context) == SOCK_STREAM) {
		ret = net_tcp_connect(context, addr, laddr, rport, lport,
				      timeout, cb, user_data);
	} else {
		ret = -ENOTSUP;
	}

unlock:
	k_mutex_unlock(&context->lock);

	return ret;
}

int net_context_accept(struct net_context *context,
		       net_tcp_accept_cb_t cb,
		       k_timeout_t timeout,
		       void *user_data)
{
	int ret = 0;

	NET_ASSERT(PART_OF_ARRAY(contexts, context));

	if (!net_context_is_used(context)) {
		return -EBADF;
	}

	k_mutex_lock(&context->lock, K_FOREVER);

	if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
	    net_if_is_ip_offloaded(net_context_get_iface(context))) {
		ret = net_offload_accept(
			net_context_get_iface(context),
			context,
			cb,
			timeout,
			user_data);
		goto unlock;
	}

	if ((net_context_get_state(context) != NET_CONTEXT_LISTENING) &&
	    (net_context_get_type(context) != SOCK_STREAM)) {
		NET_DBG("Invalid socket, state %d type %d",
			net_context_get_state(context),
			net_context_get_type(context));
		ret = -EINVAL;
		goto unlock;
	}

	if (net_context_get_proto(context) == IPPROTO_TCP) {
		ret = net_tcp_accept(context, cb, user_data);
		goto unlock;
	}

unlock:
	k_mutex_unlock(&context->lock);

	return ret;
}


static int get_context_priority(struct net_context *context,
				void *value, size_t *len)
{
#if defined(CONFIG_NET_CONTEXT_PRIORITY)
	*((uint8_t *)value) = context->options.priority;

	if (len) {
		*len = sizeof(uint8_t);
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int get_context_proxy(struct net_context *context,
			     void *value, size_t *len)
{
#if defined(CONFIG_SOCKS)
	struct sockaddr *addr = (struct sockaddr *)value;

	if (!value || !len) {
		return -EINVAL;
	}

	if (*len < context->options.proxy.addrlen) {
		return -EINVAL;
	}

	*len = MIN(context->options.proxy.addrlen, *len);

	memcpy(addr, &context->options.proxy.addr, *len);

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int get_context_txtime(struct net_context *context,
			      void *value, size_t *len)
{
#if defined(CONFIG_NET_CONTEXT_TXTIME)
	*((bool *)value) = context->options.txtime;

	if (len) {
		*len = sizeof(bool);
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int get_context_rcvtimeo(struct net_context *context,
				void *value, size_t *len)
{
#if defined(CONFIG_NET_CONTEXT_RCVTIMEO)
	*((k_timeout_t *)value) = context->options.rcvtimeo;

	if (len) {
		*len = sizeof(k_timeout_t);
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int get_context_sndtimeo(struct net_context *context,
				void *value, size_t *len)
{
#if defined(CONFIG_NET_CONTEXT_SNDTIMEO)
	*((k_timeout_t *)value) = context->options.sndtimeo;

	if (len) {
		*len = sizeof(k_timeout_t);
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int get_context_rcvbuf(struct net_context *context,
			      void *value, size_t *len)
{
#if defined(CONFIG_NET_CONTEXT_RCVBUF)
	*((int *)value) = context->options.rcvbuf;

	if (len) {
		*len = sizeof(int);
	}
	return 0;
#else
	return -ENOTSUP;
#endif
}

static int get_context_sndbuf(struct net_context *context,
				void *value, size_t *len)
{
#if defined(CONFIG_NET_CONTEXT_SNDBUF)
	*((int *)value) = context->options.sndbuf;

	if (len) {
		*len = sizeof(int);
	}
	return 0;
#else
	return -ENOTSUP;
#endif
}

static int get_context_dscp_ecn(struct net_context *context,
				void *value, size_t *len)
{
#if defined(CONFIG_NET_CONTEXT_DSCP_ECN)
	*((int *)value) = context->options.dscp_ecn;

	if (len) {
		*len = sizeof(int);
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}

/* If buf is not NULL, then use it. Otherwise read the data to be written
 * to net_pkt from msghdr.
 */
static int context_write_data(struct net_pkt *pkt, const void *buf,
			      int buf_len, const struct msghdr *msghdr)
{
	int ret = 0;

	if (msghdr) {
		int i;

		for (i = 0; i < msghdr->msg_iovlen; i++) {
			int len = MIN(msghdr->msg_iov[i].iov_len, buf_len);

			ret = net_pkt_write(pkt, msghdr->msg_iov[i].iov_base,
					    len);
			if (ret < 0) {
				break;
			}

			buf_len -= len;
			if (buf_len == 0) {
				break;
			}
		}
	} else {
		ret = net_pkt_write(pkt, buf, buf_len);
	}

	return ret;
}

static int context_setup_udp_packet(struct net_context *context,
				    struct net_pkt *pkt,
				    const void *buf,
				    size_t len,
				    const struct msghdr *msg,
				    const struct sockaddr *dst_addr,
				    socklen_t addrlen)
{
	int ret = -EINVAL;
	uint16_t dst_port = 0U;

	if (IS_ENABLED(CONFIG_NET_IPV6) &&
	    net_context_get_family(context) == AF_INET6) {
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)dst_addr;

		dst_port = addr6->sin6_port;

		ret = net_context_create_ipv6_new(context, pkt,
						  NULL, &addr6->sin6_addr);
	} else if (IS_ENABLED(CONFIG_NET_IPV4) &&
		   net_context_get_family(context) == AF_INET) {
		struct sockaddr_in *addr4 = (struct sockaddr_in *)dst_addr;

		dst_port = addr4->sin_port;

		ret = net_context_create_ipv4_new(context, pkt,
						  NULL, &addr4->sin_addr);
	}

	if (ret < 0) {
		return ret;
	}

	ret = bind_default(context);
	if (ret) {
		return ret;
	}

	ret = net_udp_create(pkt,
			     net_sin((struct sockaddr *)
				     &context->local)->sin_port,
			     dst_port);
	if (ret) {
		return ret;
	}

	ret = context_write_data(pkt, buf, len, msg);
	if (ret) {
		return ret;
	}

	return 0;
}

static void context_finalize_packet(struct net_context *context,
				    struct net_pkt *pkt)
{
	/* This function is meant to be temporary: once all moved to new
	 * API, it will be up to net_send_data() to finalize the packet.
	 */

	net_pkt_cursor_init(pkt);

	if (IS_ENABLED(CONFIG_NET_IPV6) &&
	    net_context_get_family(context) == AF_INET6) {
		net_ipv6_finalize(pkt, net_context_get_proto(context));
	} else if (IS_ENABLED(CONFIG_NET_IPV4) &&
		   net_context_get_family(context) == AF_INET) {
		net_ipv4_finalize(pkt, net_context_get_proto(context));
	}
}

static struct net_pkt *context_alloc_pkt(struct net_context *context,
					 size_t len, k_timeout_t timeout)
{
	struct net_pkt *pkt;

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	if (context->tx_slab) {
		pkt = net_pkt_alloc_from_slab(context->tx_slab(), timeout);
		if (!pkt) {
			return NULL;
		}

		net_pkt_set_iface(pkt, net_context_get_iface(context));
		net_pkt_set_family(pkt, net_context_get_family(context));
		net_pkt_set_context(pkt, context);

		if (net_pkt_alloc_buffer(pkt, len,
					 net_context_get_proto(context),
					 timeout)) {
			net_pkt_unref(pkt);
			return NULL;
		}

		return pkt;
	}
#endif
	pkt = net_pkt_alloc_with_buffer(net_context_get_iface(context), len,
					net_context_get_family(context),
					net_context_get_proto(context),
					timeout);
	if (pkt) {
		net_pkt_set_context(pkt, context);
	}

	return pkt;
}

static void set_pkt_txtime(struct net_pkt *pkt, const struct msghdr *msghdr)
{
	struct cmsghdr *cmsg;

	for (cmsg = CMSG_FIRSTHDR(msghdr); cmsg != NULL;
	     cmsg = CMSG_NXTHDR(msghdr, cmsg)) {
		if (cmsg->cmsg_len == CMSG_LEN(sizeof(uint64_t)) &&
		    cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_TXTIME) {
			uint64_t txtime = *(uint64_t *)CMSG_DATA(cmsg);

			net_pkt_set_txtime(pkt, txtime);
			break;
		}
	}
}

static int context_sendto(struct net_context *context,
			  const void *buf,
			  size_t len,
			  const struct sockaddr *dst_addr,
			  socklen_t addrlen,
			  net_context_send_cb_t cb,
			  k_timeout_t timeout,
			  void *user_data,
			  bool sendto)
{
	const struct msghdr *msghdr = NULL;
	struct net_if *iface;
	struct net_pkt *pkt;
	size_t tmp_len;
	int ret;

	NET_ASSERT(PART_OF_ARRAY(contexts, context));

	if (!net_context_is_used(context)) {
		return -EBADF;
	}

	if (sendto && addrlen == 0 && dst_addr == NULL && buf != NULL) {
		/* User wants to call sendmsg */
		msghdr = buf;
	}

	if (!msghdr && !dst_addr) {
		return -EDESTADDRREQ;
	}

	if (IS_ENABLED(CONFIG_NET_IPV6) &&
	    net_context_get_family(context) == AF_INET6) {
		const struct sockaddr_in6 *addr6 =
			(const struct sockaddr_in6 *)dst_addr;

		if (msghdr) {
			addr6 = msghdr->msg_name;
			addrlen = msghdr->msg_namelen;

			if (!addr6) {
				addr6 = net_sin6(&context->remote);
				addrlen = sizeof(struct sockaddr_in6);
			}

			/* For sendmsg(), the dst_addr is NULL so set it here.
			 */
			dst_addr = (const struct sockaddr *)addr6;
		}

		if (addrlen < sizeof(struct sockaddr_in6)) {
			return -EINVAL;
		}

		if (net_ipv6_is_addr_unspecified(&addr6->sin6_addr)) {
			return -EDESTADDRREQ;
		}

		/* If application has not yet set the destination address
		 * i.e., by not calling connect(), then set the interface
		 * here so that the packet gets sent to the correct network
		 * interface. This issue can be seen if there are multiple
		 * network interfaces and we are trying to send data to
		 * second or later network interface.
		 */
		if (net_ipv6_is_addr_unspecified(
				&net_sin6(&context->remote)->sin6_addr) &&
		    !net_context_is_bound_to_iface(context)) {
			iface = net_if_ipv6_select_src_iface(&addr6->sin6_addr);
			net_context_set_iface(context, iface);
		}

	} else if (IS_ENABLED(CONFIG_NET_IPV4) &&
		   net_context_get_family(context) == AF_INET) {
		const struct sockaddr_in *addr4 =
			(const struct sockaddr_in *)dst_addr;

		if (msghdr) {
			addr4 = msghdr->msg_name;
			addrlen = msghdr->msg_namelen;

			if (!addr4) {
				addr4 = net_sin(&context->remote);
				addrlen = sizeof(struct sockaddr_in);
			}

			/* For sendmsg(), the dst_addr is NULL so set it here.
			 */
			dst_addr = (const struct sockaddr *)addr4;
		}

		if (addrlen < sizeof(struct sockaddr_in)) {
			return -EINVAL;
		}

		if (!addr4->sin_addr.s_addr) {
			return -EDESTADDRREQ;
		}

		/* If application has not yet set the destination address
		 * i.e., by not calling connect(), then set the interface
		 * here so that the packet gets sent to the correct network
		 * interface. This issue can be seen if there are multiple
		 * network interfaces and we are trying to send data to
		 * second or later network interface.
		 */
		if (net_sin(&context->remote)->sin_addr.s_addr == 0U &&
		    !net_context_is_bound_to_iface(context)) {
			iface = net_if_ipv4_select_src_iface(&addr4->sin_addr);
			net_context_set_iface(context, iface);
		}

	} else if (IS_ENABLED(CONFIG_NET_SOCKETS_PACKET) &&
		   net_context_get_family(context) == AF_PACKET) {
		struct sockaddr_ll *ll_addr = (struct sockaddr_ll *)dst_addr;

		if (msghdr) {
			ll_addr = msghdr->msg_name;
			addrlen = msghdr->msg_namelen;

			if (!ll_addr) {
				ll_addr = (struct sockaddr_ll *)
							(&context->remote);
				addrlen = sizeof(struct sockaddr_ll);
			}

			/* For sendmsg(), the dst_addr is NULL so set it here.
			 */
			dst_addr = (const struct sockaddr *)ll_addr;
		}

		if (addrlen < sizeof(struct sockaddr_ll)) {
			return -EINVAL;
		}

		iface = net_context_get_iface(context);
		if (iface == NULL) {
			if (ll_addr->sll_ifindex < 0) {
				return -EDESTADDRREQ;
			}

			iface = net_if_get_by_index(ll_addr->sll_ifindex);
			if (iface == NULL) {
				NET_ERR("Cannot bind to interface index %d",
					ll_addr->sll_ifindex);
				return -EDESTADDRREQ;
			}
		}

		if (net_context_get_type(context) == SOCK_DGRAM) {
			context->flags |= NET_CONTEXT_REMOTE_ADDR_SET;

			/* The user must set the protocol in send call */

			/* For sendmsg() call, we might have set ll_addr to
			 * point to remote addr.
			 */
			if ((void *)&context->remote != (void *)ll_addr) {
				memcpy((struct sockaddr_ll *)&context->remote,
				       ll_addr, sizeof(struct sockaddr_ll));
			}
		}

	} else if (IS_ENABLED(CONFIG_NET_SOCKETS_CAN) &&
		   net_context_get_family(context) == AF_CAN) {
		struct sockaddr_can *can_addr = (struct sockaddr_can *)dst_addr;

		if (msghdr) {
			can_addr = msghdr->msg_name;
			addrlen = msghdr->msg_namelen;

			if (!can_addr) {
				can_addr = (struct sockaddr_can *)
							(&context->remote);
				addrlen = sizeof(struct sockaddr_can);
			}

			/* For sendmsg(), the dst_addr is NULL so set it here.
			 */
			dst_addr = (const struct sockaddr *)can_addr;
		}

		if (addrlen < sizeof(struct sockaddr_can)) {
			return -EINVAL;
		}

		if (can_addr->can_ifindex < 0) {
			/* The index should have been set in bind */
			can_addr->can_ifindex =
				net_can_ptr(&context->local)->can_ifindex;
		}

		if (can_addr->can_ifindex < 0) {
			return -EDESTADDRREQ;
		}

		iface = net_if_get_by_index(can_addr->can_ifindex);
		if (!iface) {
			NET_ERR("Cannot bind to interface index %d",
				can_addr->can_ifindex);
			return -EDESTADDRREQ;
		}
	} else {
		NET_DBG("Invalid protocol family %d",
			net_context_get_family(context));
		return -EINVAL;
	}

	if (msghdr && len == 0) {
		int i;

		for (i = 0; i < msghdr->msg_iovlen; i++) {
			len += msghdr->msg_iov[i].iov_len;
		}
	}

	iface = net_context_get_iface(context);
	if (iface && !net_if_is_up(iface)) {
		return -ENETDOWN;
	}

	pkt = context_alloc_pkt(context, len, PKT_WAIT_TIME);
	if (!pkt) {
		NET_ERR("Failed to allocate net_pkt");
		return -ENOBUFS;
	}

	tmp_len = net_pkt_available_payload_buffer(
				pkt, net_context_get_proto(context));
	if (tmp_len < len) {
		if (net_context_get_type(context) == SOCK_DGRAM) {
			NET_ERR("Available payload buffer (%zu) is not enough for requested DGRAM (%zu)",
				tmp_len, len);
			ret = -ENOMEM;
			goto fail;
		}
		len = tmp_len;
	}

	context->send_cb = cb;
	context->user_data = user_data;

	if (IS_ENABLED(CONFIG_NET_CONTEXT_PRIORITY)) {
		uint8_t priority;

		get_context_priority(context, &priority, NULL);
		net_pkt_set_priority(pkt, priority);
	}

	/* If there is ancillary data in msghdr, then we need to add that
	 * to net_pkt as there is no other way to store it.
	 */
	if (msghdr && msghdr->msg_control && msghdr->msg_controllen) {
		if (IS_ENABLED(CONFIG_NET_CONTEXT_TXTIME)) {
			bool is_txtime;

			get_context_txtime(context, &is_txtime, NULL);
			if (is_txtime) {
				set_pkt_txtime(pkt, msghdr);
			}
		}
	}

	if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
	    net_if_is_ip_offloaded(net_context_get_iface(context))) {
		ret = context_write_data(pkt, buf, len, msghdr);
		if (ret < 0) {
			goto fail;
		}

		net_pkt_cursor_init(pkt);

		if (sendto) {
			ret = net_offload_sendto(net_context_get_iface(context),
						 pkt, dst_addr, addrlen, cb,
						 timeout, user_data);
		} else {
			ret = net_offload_send(net_context_get_iface(context),
					       pkt, cb, timeout, user_data);
		}
	} else if (IS_ENABLED(CONFIG_NET_UDP) &&
	    net_context_get_proto(context) == IPPROTO_UDP) {
		ret = context_setup_udp_packet(context, pkt, buf, len, msghdr,
					       dst_addr, addrlen);
		if (ret < 0) {
			goto fail;
		}

		context_finalize_packet(context, pkt);

		ret = net_send_data(pkt);
	} else if (IS_ENABLED(CONFIG_NET_TCP) &&
		   net_context_get_proto(context) == IPPROTO_TCP) {

		ret = context_write_data(pkt, buf, len, msghdr);
		if (ret < 0) {
			goto fail;
		}

		net_pkt_cursor_init(pkt);
		ret = net_tcp_queue_data(context, pkt);
		if (ret < 0) {
			goto fail;
		}

		ret = net_tcp_send_data(context, cb, user_data);
	} else if (IS_ENABLED(CONFIG_NET_SOCKETS_PACKET) &&
		   net_context_get_family(context) == AF_PACKET) {
		ret = context_write_data(pkt, buf, len, msghdr);
		if (ret < 0) {
			goto fail;
		}

		net_pkt_cursor_init(pkt);

		if (net_context_get_proto(context) == IPPROTO_RAW) {
			char type = (NET_IPV6_HDR(pkt)->vtc & 0xf0);

			/* Set the family to pkt if detected */
			switch (type) {
			case 0x60:
				net_pkt_set_family(pkt, AF_INET6);
				break;
			case 0x40:
				net_pkt_set_family(pkt, AF_INET);
				break;
			default:
				/* Not IP traffic, let it go forward as it is */
				break;
			}

			/* Pass to L2: */
			ret = net_send_data(pkt);
		} else {
			net_if_queue_tx(net_pkt_iface(pkt), pkt);
		}
	} else if (IS_ENABLED(CONFIG_NET_SOCKETS_CAN) &&
		   net_context_get_family(context) == AF_CAN &&
		   net_context_get_proto(context) == CAN_RAW) {
		ret = context_write_data(pkt, buf, len, msghdr);
		if (ret < 0) {
			goto fail;
		}

		net_pkt_cursor_init(pkt);

		ret = net_send_data(pkt);
	} else {
		NET_DBG("Unknown protocol while sending packet: %d",
		net_context_get_proto(context));
		ret = -EPROTONOSUPPORT;
	}

	if (ret < 0) {
		goto fail;
	}

	return len;
fail:
	net_pkt_unref(pkt);

	return ret;
}

int net_context_send(struct net_context *context,
		     const void *buf,
		     size_t len,
		     net_context_send_cb_t cb,
		     k_timeout_t timeout,
		     void *user_data)
{
	socklen_t addrlen;
	int ret = 0;

	k_mutex_lock(&context->lock, K_FOREVER);

	if (!(context->flags & NET_CONTEXT_REMOTE_ADDR_SET) ||
	    !net_sin(&context->remote)->sin_port) {
		ret = -EDESTADDRREQ;
		goto unlock;
	}

	if (IS_ENABLED(CONFIG_NET_IPV6) &&
	    net_context_get_family(context) == AF_INET6) {
		addrlen = sizeof(struct sockaddr_in6);
	} else if (IS_ENABLED(CONFIG_NET_IPV4) &&
		   net_context_get_family(context) == AF_INET) {
		addrlen = sizeof(struct sockaddr_in);
	} else if (IS_ENABLED(CONFIG_NET_SOCKETS_PACKET) &&
		   net_context_get_family(context) == AF_PACKET) {
		ret = -EOPNOTSUPP;
		goto unlock;
	} else if (IS_ENABLED(CONFIG_NET_SOCKETS_CAN) &&
		   net_context_get_family(context) == AF_CAN) {
		addrlen = sizeof(struct sockaddr_can);
	} else {
		addrlen = 0;
	}

	ret = context_sendto(context, buf, len, &context->remote,
			     addrlen, cb, timeout, user_data, false);
unlock:
	k_mutex_unlock(&context->lock);

	return ret;
}

int net_context_sendmsg(struct net_context *context,
			const struct msghdr *msghdr,
			int flags,
			net_context_send_cb_t cb,
			k_timeout_t timeout,
			void *user_data)
{
	int ret;

	k_mutex_lock(&context->lock, K_FOREVER);

	ret = context_sendto(context, msghdr, 0, NULL, 0,
			     cb, timeout, user_data, true);

	k_mutex_unlock(&context->lock);

	return ret;
}

int net_context_sendto(struct net_context *context,
		       const void *buf,
		       size_t len,
		       const struct sockaddr *dst_addr,
		       socklen_t addrlen,
		       net_context_send_cb_t cb,
		       k_timeout_t timeout,
		       void *user_data)
{
	int ret;

	k_mutex_lock(&context->lock, K_FOREVER);

	ret = context_sendto(context, buf, len, dst_addr, addrlen,
			     cb, timeout, user_data, true);

	k_mutex_unlock(&context->lock);

	return ret;
}

enum net_verdict net_context_packet_received(struct net_conn *conn,
					     struct net_pkt *pkt,
					     union net_ip_header *ip_hdr,
					     union net_proto_header *proto_hdr,
					     void *user_data)
{
	struct net_context *context = find_context(conn);
	enum net_verdict verdict = NET_DROP;

	NET_ASSERT(context);
	NET_ASSERT(net_pkt_iface(pkt));

	k_mutex_lock(&context->lock, K_FOREVER);

	net_context_set_iface(context, net_pkt_iface(pkt));
	net_pkt_set_context(pkt, context);

	/* If there is no callback registered, then we can only drop
	 * the packet.
	 */
	if (!context->recv_cb) {
		goto unlock;
	}

	if (net_context_get_proto(context) == IPPROTO_TCP) {
		net_stats_update_tcp_recv(net_pkt_iface(pkt),
					  net_pkt_remaining_data(pkt));
	}

#if defined(CONFIG_NET_CONTEXT_SYNC_RECV)
	k_sem_give(&context->recv_data_wait);
#endif /* CONFIG_NET_CONTEXT_SYNC_RECV */

	k_mutex_unlock(&context->lock);

	context->recv_cb(context, pkt, ip_hdr, proto_hdr, 0, user_data);

	verdict = NET_OK;

	return verdict;

unlock:
	k_mutex_unlock(&context->lock);

	return verdict;
}

#if defined(CONFIG_NET_UDP)
static int recv_udp(struct net_context *context,
		    net_context_recv_cb_t cb,
		    k_timeout_t timeout,
		    void *user_data)
{
	struct sockaddr local_addr = {
		.sa_family = net_context_get_family(context),
	};
	struct sockaddr *laddr = NULL;
	uint16_t lport = 0U;
	int ret;

	ARG_UNUSED(timeout);

	if (context->conn_handler) {
		net_conn_unregister(context->conn_handler);
		context->conn_handler = NULL;
	}

	ret = bind_default(context);
	if (ret) {
		return ret;
	}

	if (IS_ENABLED(CONFIG_NET_IPV6) &&
	    net_context_get_family(context) == AF_INET6) {
		if (net_sin6_ptr(&context->local)->sin6_addr) {
			net_ipaddr_copy(&net_sin6(&local_addr)->sin6_addr,
				     net_sin6_ptr(&context->local)->sin6_addr);

			laddr = &local_addr;
		}

		net_sin6(&local_addr)->sin6_port =
			net_sin6((struct sockaddr *)&context->local)->sin6_port;
		lport = net_sin6((struct sockaddr *)&context->local)->sin6_port;
	} else if (IS_ENABLED(CONFIG_NET_IPV4) &&
		   net_context_get_family(context) == AF_INET) {
		if (net_sin_ptr(&context->local)->sin_addr) {
			net_ipaddr_copy(&net_sin(&local_addr)->sin_addr,
				      net_sin_ptr(&context->local)->sin_addr);

			laddr = &local_addr;
		}

		lport = net_sin((struct sockaddr *)&context->local)->sin_port;
	}

	context->recv_cb = cb;

	ret = net_conn_register(net_context_get_proto(context),
				net_context_get_family(context),
				context->flags & NET_CONTEXT_REMOTE_ADDR_SET ?
							&context->remote : NULL,
				laddr,
				ntohs(net_sin(&context->remote)->sin_port),
				ntohs(lport),
				context,
				net_context_packet_received,
				user_data,
				&context->conn_handler);

	return ret;
}
#else
#define recv_udp(...) 0
#endif /* CONFIG_NET_UDP */

static enum net_verdict net_context_raw_packet_received(
					struct net_conn *conn,
					struct net_pkt *pkt,
					union net_ip_header *ip_hdr,
					union net_proto_header *proto_hdr,
					void *user_data)
{
	struct net_context *context = find_context(conn);

	NET_ASSERT(context);
	NET_ASSERT(net_pkt_iface(pkt));

	/* If there is no callback registered, then we can only drop
	 * the packet.
	 */

	if (!context->recv_cb) {
		return NET_DROP;
	}

	net_context_set_iface(context, net_pkt_iface(pkt));
	net_pkt_set_context(pkt, context);

	context->recv_cb(context, pkt, ip_hdr, proto_hdr, 0, user_data);

#if defined(CONFIG_NET_CONTEXT_SYNC_RECV)
	k_sem_give(&context->recv_data_wait);
#endif /* CONFIG_NET_CONTEXT_SYNC_RECV */

	return NET_OK;
}

static int recv_raw(struct net_context *context,
		    net_context_recv_cb_t cb,
		    k_timeout_t timeout,
		    struct sockaddr *local_addr,
		    void *user_data)
{
	int ret;

	ARG_UNUSED(timeout);

	context->recv_cb = cb;

	if (context->conn_handler) {
		net_conn_unregister(context->conn_handler);
		context->conn_handler = NULL;
	}

	ret = bind_default(context);
	if (ret) {
		return ret;
	}

	ret = net_conn_register(net_context_get_proto(context),
				net_context_get_family(context),
				NULL, local_addr, 0, 0,
				context,
				net_context_raw_packet_received,
				user_data,
				&context->conn_handler);

	return ret;
}

int net_context_recv(struct net_context *context,
		     net_context_recv_cb_t cb,
		     k_timeout_t timeout,
		     void *user_data)
{
	int ret;
	NET_ASSERT(context);

	if (!net_context_is_used(context)) {
		return -EBADF;
	}

	k_mutex_lock(&context->lock, K_FOREVER);

	if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
	    net_if_is_ip_offloaded(net_context_get_iface(context))) {
		ret = net_offload_recv(
			net_context_get_iface(context),
			context, cb, timeout, user_data);
		goto unlock;
	}

	if (IS_ENABLED(CONFIG_NET_UDP) &&
	    net_context_get_proto(context) == IPPROTO_UDP) {
		ret = recv_udp(context, cb, timeout, user_data);
	} else if (IS_ENABLED(CONFIG_NET_TCP) &&
		   net_context_get_proto(context) == IPPROTO_TCP) {
		ret = net_tcp_recv(context, cb, user_data);
	} else {
		if (IS_ENABLED(CONFIG_NET_SOCKETS_PACKET) &&
		    net_context_get_family(context) == AF_PACKET) {
			struct sockaddr_ll addr;

			addr.sll_family = AF_PACKET;
			addr.sll_ifindex =
				net_sll_ptr(&context->local)->sll_ifindex;
			addr.sll_protocol =
				net_sll_ptr(&context->local)->sll_protocol;
			addr.sll_halen =
				net_sll_ptr(&context->local)->sll_halen;

			memcpy(addr.sll_addr,
			       net_sll_ptr(&context->local)->sll_addr,
			       MIN(addr.sll_halen, sizeof(addr.sll_addr)));

			ret = recv_raw(context, cb, timeout,
				       (struct sockaddr *)&addr, user_data);
		} else if (IS_ENABLED(CONFIG_NET_SOCKETS_CAN) &&
			   net_context_get_family(context) == AF_CAN) {
			struct sockaddr_can local_addr = {
				.can_family = AF_CAN,
			};

			ret = recv_raw(context, cb, timeout,
				       (struct sockaddr *)&local_addr,
				       user_data);
			if (ret == -EALREADY) {
				/* This is perfectly normal for CAN sockets.
				 * The SocketCAN will dispatch the packet to
				 * correct net_context listener.
				 */
				ret = 0;
			}
		} else {
			ret = -EPROTOTYPE;
		}
	}

	if (ret < 0) {
		goto unlock;
	}

#if defined(CONFIG_NET_CONTEXT_SYNC_RECV)
	if (!K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		int ret;

		/* Make sure we have the lock, then the
		 * net_context_packet_received() callback will release the
		 * semaphore when data has been received.
		 */
		k_sem_reset(&context->recv_data_wait);

		k_mutex_unlock(&context->lock);

		ret = k_sem_take(&context->recv_data_wait, timeout);

		k_mutex_lock(&context->lock, K_FOREVER);

		if (ret == -EAGAIN) {
			ret = -ETIMEDOUT;
			goto unlock;
		}
	}
#endif /* CONFIG_NET_CONTEXT_SYNC_RECV */

unlock:
	k_mutex_unlock(&context->lock);

	return ret;
}

int net_context_update_recv_wnd(struct net_context *context,
				int32_t delta)
{
	int ret;

	if (IS_ENABLED(CONFIG_NET_OFFLOAD) &&
		net_if_is_ip_offloaded(net_context_get_iface(context))) {
		return 0;
	}

	k_mutex_lock(&context->lock, K_FOREVER);

	ret = net_tcp_update_recv_wnd(context, delta);

	k_mutex_unlock(&context->lock);

	return ret;
}

static int set_context_priority(struct net_context *context,
				const void *value, size_t len)
{
#if defined(CONFIG_NET_CONTEXT_PRIORITY)
	if (len > sizeof(uint8_t)) {
		return -EINVAL;
	}

	context->options.priority = *((uint8_t *)value);

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int set_context_txtime(struct net_context *context,
			      const void *value, size_t len)
{
#if defined(CONFIG_NET_CONTEXT_TXTIME)
	if (len > sizeof(bool)) {
		return -EINVAL;
	}

	context->options.txtime = *((bool *)value);

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int set_context_proxy(struct net_context *context,
			     const void *value, size_t len)
{
#if defined(CONFIG_SOCKS)
	struct sockaddr *addr = (struct sockaddr *)value;

	if (len > NET_SOCKADDR_MAX_SIZE) {
		return -EINVAL;
	}

	if (addr->sa_family != net_context_get_family(context)) {
		return -EINVAL;
	}

	context->options.proxy.addrlen = len;
	memcpy(&context->options.proxy.addr, addr, len);

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int set_context_rcvtimeo(struct net_context *context,
				const void *value, size_t len)
{
#if defined(CONFIG_NET_CONTEXT_RCVTIMEO)
	if (len != sizeof(k_timeout_t)) {
		return -EINVAL;
	}

	context->options.rcvtimeo = *((k_timeout_t *)value);

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int set_context_sndtimeo(struct net_context *context,
				const void *value, size_t len)
{
#if defined(CONFIG_NET_CONTEXT_SNDTIMEO)
	if (len != sizeof(k_timeout_t)) {
		return -EINVAL;
	}

	context->options.sndtimeo = *((k_timeout_t *)value);

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int set_context_rcvbuf(struct net_context *context,
				const void *value, size_t len)
{
#if defined(CONFIG_NET_CONTEXT_RCVBUF)
	int rcvbuf_value = *((int *)value);

	if (len != sizeof(int)) {
		return -EINVAL;
	}

	if ((rcvbuf_value < 0) || (rcvbuf_value > UINT16_MAX)) {
		return -EINVAL;
	}

	context->options.rcvbuf = (uint16_t) rcvbuf_value;

	return 0;
#else
	return -ENOTSUP;
#endif
}

static int set_context_sndbuf(struct net_context *context,
				const void *value, size_t len)
{
#if defined(CONFIG_NET_CONTEXT_SNDBUF)
	int sndbuf_value = *((int *)value);

	if (len != sizeof(int)) {
		return -EINVAL;
	}

	if ((sndbuf_value < 0) || (sndbuf_value > UINT16_MAX)) {
		return -EINVAL;
	}

	context->options.sndbuf = (uint16_t) sndbuf_value;
	return 0;
#else
	return -ENOTSUP;
#endif
}

static int set_context_dscp_ecn(struct net_context *context,
				const void *value, size_t len)
{
#if defined(CONFIG_NET_CONTEXT_DSCP_ECN)
	int dscp_ecn = *((int *)value);

	if (len != sizeof(int)) {
		return -EINVAL;
	}

	if ((dscp_ecn < 0) || (dscp_ecn > UINT8_MAX)) {
		return -EINVAL;
	}

	context->options.dscp_ecn = (uint8_t)dscp_ecn;

	return 0;
#else
	return -ENOTSUP;
#endif
}

int net_context_set_option(struct net_context *context,
			   enum net_context_option option,
			   const void *value, size_t len)
{
	int ret = 0;

	NET_ASSERT(context);

	if (!PART_OF_ARRAY(contexts, context)) {
		return -EINVAL;
	}

	k_mutex_lock(&context->lock, K_FOREVER);

	switch (option) {
	case NET_OPT_PRIORITY:
		ret = set_context_priority(context, value, len);
		break;
	case NET_OPT_TXTIME:
		ret = set_context_txtime(context, value, len);
		break;
	case NET_OPT_SOCKS5:
		ret = set_context_proxy(context, value, len);
		break;
	case NET_OPT_RCVTIMEO:
		ret = set_context_rcvtimeo(context, value, len);
		break;
	case NET_OPT_SNDTIMEO:
		ret = set_context_sndtimeo(context, value, len);
		break;
	case NET_OPT_RCVBUF:
		ret = set_context_rcvbuf(context, value, len);
		break;
	case NET_OPT_SNDBUF:
		ret = set_context_sndbuf(context, value, len);
		break;
	case NET_OPT_DSCP_ECN:
		ret = set_context_dscp_ecn(context, value, len);
		break;
	}

	k_mutex_unlock(&context->lock);

	return ret;
}

int net_context_get_option(struct net_context *context,
			    enum net_context_option option,
			    void *value, size_t *len)
{
	int ret = 0;

	NET_ASSERT(context);

	if (!PART_OF_ARRAY(contexts, context)) {
		return -EINVAL;
	}

	k_mutex_lock(&context->lock, K_FOREVER);

	switch (option) {
	case NET_OPT_PRIORITY:
		ret = get_context_priority(context, value, len);
		break;
	case NET_OPT_TXTIME:
		ret = get_context_txtime(context, value, len);
		break;
	case NET_OPT_SOCKS5:
		ret = get_context_proxy(context, value, len);
		break;
	case NET_OPT_RCVTIMEO:
		ret = get_context_rcvtimeo(context, value, len);
		break;
	case NET_OPT_SNDTIMEO:
		ret = get_context_sndtimeo(context, value, len);
		break;
	case NET_OPT_RCVBUF:
		ret = get_context_rcvbuf(context, value, len);
		break;
	case NET_OPT_SNDBUF:
		ret = get_context_sndbuf(context, value, len);
		break;
	case NET_OPT_DSCP_ECN:
		ret = get_context_dscp_ecn(context, value, len);
		break;
	}

	k_mutex_unlock(&context->lock);

	return ret;
}

void net_context_foreach(net_context_cb_t cb, void *user_data)
{
	int i;

	k_sem_take(&contexts_lock, K_FOREVER);

	for (i = 0; i < NET_MAX_CONTEXT; i++) {
		if (!net_context_is_used(&contexts[i])) {
			continue;
		}

		k_mutex_lock(&contexts[i].lock, K_FOREVER);

		cb(&contexts[i], user_data);

		k_mutex_unlock(&contexts[i].lock);
	}

	k_sem_give(&contexts_lock);
}

const char *net_context_state(struct net_context *context)
{
	switch (net_context_get_state(context)) {
	case NET_CONTEXT_IDLE:
		return "IDLE";
	case NET_CONTEXT_CONNECTING:
		return "CONNECTING";
	case NET_CONTEXT_CONNECTED:
		return "CONNECTED";
	case NET_CONTEXT_LISTENING:
		return "LISTENING";
	}

	return NULL;
}

void net_context_init(void)
{
	k_sem_init(&contexts_lock, 1, K_SEM_MAX_LIMIT);
}
