/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(net_test, CONFIG_DNS_RESOLVER_LOG_LEVEL);

#include <zephyr/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>

#include <zephyr/ztest.h>

#include <zephyr/net/ethernet.h>
#include <zephyr/net/dummy.h>
#include <zephyr/net_buf.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket_service.h>

#define NET_LOG_ENABLED 1
#include "net_private.h"

#if defined(CONFIG_DNS_RESOLVER_LOG_LEVEL_DBG)
#define DBG(fmt, ...) printk(fmt, ##__VA_ARGS__)
#else
#define DBG(fmt, ...)
#endif

#define NAME4 "4.zephyr.test"
#define NAME6 "6.zephyr.test"
#define NAME_IPV4 "192.0.2.1"
#define NAME_IPV6 "2001:db8::1"

#define DNS_NAME_IPV4 "192.0.2.4"
#define DNS2_NAME_IPV4 "192.0.2.5"
#define DNS_NAME_IPV6 "2001:db8::4"

#define DNS_TIMEOUT 500 /* ms */

#if defined(CONFIG_NET_IPV6) && (CONFIG_NET_IF_MAX_IPV6_COUNT < 2)
/* Interface 1 addresses */
static struct net_in6_addr my_addr1 = { { { 0x20, 0x01, 0x0d, 0xb8, 1, 0, 0, 0,
					    0, 0, 0, 0, 0, 0, 0, 0x1 } } };
#endif

#if defined(CONFIG_NET_IPV4) && (CONFIG_NET_IF_MAX_IPV6_COUNT < 2)
/* Interface 1 addresses */
static struct net_in_addr my_addr2 = { { { 192, 0, 2, 1 } } };
#endif

#if CONFIG_NET_IF_MAX_IPV6_COUNT < 2

static struct net_if *iface1;

#endif

/* this must be higher that the DNS_TIMEOUT */
#define WAIT_TIME K_MSEC((DNS_TIMEOUT + 300) * 3)

struct net_if_test {
	uint8_t idx;
	uint8_t mac_addr[sizeof(struct net_eth_addr)];
};

#if CONFIG_NET_IF_MAX_IPV6_COUNT < 2

static uint8_t *net_iface_get_mac(const struct device *dev)
{
	struct net_if_test *data = dev->data;

	if (data->mac_addr[2] == 0x00) {
		/* 00-00-5E-00-53-xx Documentation RFC 7042 */
		data->mac_addr[0] = 0x00;
		data->mac_addr[1] = 0x00;
		data->mac_addr[2] = 0x5E;
		data->mac_addr[3] = 0x00;
		data->mac_addr[4] = 0x53;
		data->mac_addr[5] = sys_rand8_get();
	}

	return data->mac_addr;
}

static void net_iface_init(struct net_if *iface)
{
	uint8_t *mac = net_iface_get_mac(net_if_get_device(iface));

	net_if_set_link_addr(iface, mac, sizeof(struct net_eth_addr),
			     NET_LINK_ETHERNET);
}

static int sender_iface(const struct device *dev, struct net_pkt *pkt)
{
	if (!pkt->frags) {
		DBG("No data to send!\n");
		return -ENODATA;
	}

	return 0;
}

struct net_if_test net_iface1_data;

static struct dummy_api net_iface_api = {
	.iface_api.init = net_iface_init,
	.send = sender_iface,
};

#define _ETH_L2_LAYER DUMMY_L2
#define _ETH_L2_CTX_TYPE NET_L2_GET_CTX_TYPE(DUMMY_L2)

NET_DEVICE_INIT_INSTANCE(net_iface1_test,
			 "iface1",
			 iface1,
			 NULL,
			 NULL,
			 &net_iface1_data,
			 NULL,
			 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
			 &net_iface_api,
			 _ETH_L2_LAYER,
			 _ETH_L2_CTX_TYPE,
			 127);

#endif /* CONFIG_NET_IF_MAX_IPV6_COUNT < 2 */

static void *test_init(void)
{
#if CONFIG_NET_IF_MAX_IPV6_COUNT >= 2
	return NULL;
#else
	struct net_if_addr *ifaddr;

	iface1 = net_if_get_by_index(0);
	zassert_is_null(iface1, "iface1");

	iface1 = net_if_get_by_index(1);

	((struct net_if_test *) net_if_get_device(iface1)->data)->idx =
		net_if_get_by_iface(iface1);

#if defined(CONFIG_NET_IPV6)
	ifaddr = net_if_ipv6_addr_add(iface1, &my_addr1,
				      NET_ADDR_MANUAL, 0);
	if (!ifaddr) {
		DBG("Cannot add IPv6 address %s\n",
		       net_sprint_ipv6_addr(&my_addr1));
		zassert_not_null(ifaddr, "addr1");

		return NULL;
	}

	/* For testing purposes we need to set the addresses preferred */
	ifaddr->addr_state = NET_ADDR_PREFERRED;
#endif

#if defined(CONFIG_NET_IPV4)
	ifaddr = net_if_ipv4_addr_add(iface1, &my_addr2,
				      NET_ADDR_MANUAL, 0);
	if (!ifaddr) {
		DBG("Cannot add IPv4 address %s\n",
		       net_sprint_ipv4_addr(&my_addr2));
		zassert_not_null(ifaddr, "addr2");

		return NULL;
	}

	ifaddr->addr_state = NET_ADDR_PREFERRED;
#endif

	net_if_up(iface1);

	return NULL;
#endif
}

ZTEST(dns_dispatcher, test_dns_dispatcher)
{
	struct dns_resolve_context *ctx;
	int ret, sock1, sock2 = -1;

#if CONFIG_NET_IF_MAX_IPV6_COUNT >= 2
	ztest_test_skip();
#endif

#if IS_ENABLED(CONFIG_MDNS_RESOLVER)
	ztest_test_skip();
#endif

	ctx = dns_resolve_get_default();

	dns_resolve_close(ctx);

	ret = dns_resolve_init_default(ctx);
	zassert_equal(ret, 0, "Cannot initialize DNS resolver (%d)", ret);

	sock1 = ctx->servers[0].sock;

	for (int i = 0; i < ctx->servers[0].dispatcher.fds_len; i++) {
		if (ctx->servers[0].dispatcher.fds[i].fd == sock1) {
			sock2 = i;
			break;
		}
	}

	zassert_not_equal(sock2, -1, "Cannot find socket");

	k_sleep(K_MSEC(10));

	dns_resolve_close(ctx);

	zassert_equal(ctx->servers[0].dispatcher.fds[sock2].fd, -1, "Socket not closed");
	zassert_equal(ctx->servers[0].dispatcher.sock, -1, "Dispatcher still registered");
}

#if CONFIG_NET_IF_MAX_IPV6_COUNT < 2

/* Single-interface host: an unscoped (ifindex 0) resolver must pair with a
 * scoped responder on that only interface (rule 3 in dns_dispatcher_can_pair()).
 */

#define TEST_DNS_PAIR_PORT 5353

extern void dns_dispatcher_svc_handler(struct net_socket_service_event *pev);

NET_SOCKET_SERVICE_SYNC_DEFINE_STATIC(pair_svc, dns_dispatcher_svc_handler, 2);

static struct dns_socket_dispatcher pair_resp;
static struct dns_socket_dispatcher pair_resv;
static struct zsock_pollfd pair_resp_fd;
static struct zsock_pollfd pair_resv_fd;

static int pair_mock_cb(struct dns_socket_dispatcher *ctx, int sock,
			struct net_sockaddr *addr, size_t addrlen,
			struct net_buf *buf, size_t len)
{
	ARG_UNUSED(ctx); ARG_UNUSED(sock); ARG_UNUSED(addr);
	ARG_UNUSED(addrlen); ARG_UNUSED(buf); ARG_UNUSED(len);
	return 0;
}

static int pair_create_socket(void)
{
	struct net_ifreq ifreq = { 0 };
	const struct device *dev = net_if_get_device(iface1);
	int reuse = 1;
	int sock;

	sock = zsock_socket(NET_AF_INET6, NET_SOCK_DGRAM, NET_IPPROTO_UDP);
	zassert_true(sock >= 0, "socket failed");

	if (IS_ENABLED(CONFIG_NET_CONTEXT_REUSEPORT)) {
		(void)zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEPORT,
				       &reuse, sizeof(reuse));
	}

	strncpy(ifreq.ifr_name, dev->name, sizeof(ifreq.ifr_name) - 1);
	(void)zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_BINDTODEVICE,
			       &ifreq, sizeof(ifreq));
	return sock;
}

static void pair_setup(struct dns_socket_dispatcher *disp, enum dns_socket_type type,
		       int ifindex, int sock, struct zsock_pollfd *pfd)
{
	struct net_sockaddr_in6 *local = net_sin6(&disp->local_addr);

	memset(disp, 0, sizeof(*disp));
	disp->type = type;
	disp->ifindex = ifindex;
	disp->sock = sock;
	disp->cb = pair_mock_cb;
	disp->fds = pfd;
	disp->fds_len = 1;
	disp->svc = &pair_svc;
	pfd->fd = sock;
	pfd->events = ZSOCK_POLLIN;

	local->sin6_family = NET_AF_INET6;
	local->sin6_port = net_htons(TEST_DNS_PAIR_PORT);
}

static void pair_teardown(struct dns_socket_dispatcher *disp, struct zsock_pollfd *pfd)
{
	int fd = disp->sock;

	if (disp->pair != NULL) {
		struct dns_socket_dispatcher *p = disp->pair;

		if (p->sock >= 0) {
			(void)zsock_close(p->sock);
		}
		p->sock = -1;
		p->pair = NULL;
		if (p->fds != NULL && p->fds_len > 0) {
			p->fds[0].fd = -1;
		}
		disp->pair = NULL;
	}

	if (fd >= 0) {
		(void)dns_dispatcher_unregister(disp);
		(void)zsock_close(fd);
	}
	memset(disp, 0, sizeof(*disp));
	disp->sock = -1;
	pfd->fd = -1;
}

ZTEST(dns_dispatcher, test_dispatcher_unscoped_pairs_single_iface)
{
	int iface_idx = net_if_get_by_iface(iface1);
	int ret;

	pair_setup(&pair_resp, DNS_SOCKET_RESPONDER, iface_idx,
		   pair_create_socket(), &pair_resp_fd);
	ret = dns_dispatcher_register(&pair_resp);
	zassert_ok(ret, "scoped responder register failed (%d)", ret);

	pair_setup(&pair_resv, DNS_SOCKET_RESOLVER, 0,
		   pair_create_socket(), &pair_resv_fd);
	ret = dns_dispatcher_register(&pair_resv);
	zassert_ok(ret, "unscoped resolver register failed (%d)", ret);
	zassert_equal(pair_resp.pair, &pair_resv,
		      "unscoped resolver must pair with scoped responder on single-iface host");

	pair_teardown(&pair_resp, &pair_resp_fd);
	pair_resv_fd.fd = -1;
	memset(&pair_resv, 0, sizeof(pair_resv));
	pair_resv.sock = -1;
}

#endif /* CONFIG_NET_IF_MAX_IPV6_COUNT < 2 */

ZTEST_SUITE(dns_dispatcher, NULL, test_init, NULL, NULL, NULL);
