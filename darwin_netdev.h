/* Darwin backend for osmo_netdev, see darwin_netdev.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
struct osmo_sockaddr;
int osmo_darwin_netdev_addr(const char *dev_name, const struct osmo_sockaddr *addr, uint8_t prefixlen, bool add);
int osmo_darwin_netdev_set_mtu(const char *dev_name, uint32_t mtu);
int osmo_darwin_netdev_ifupdown(const char *dev_name, bool up);
int osmo_darwin_netdev_add_route(const char *dev_name, const struct osmo_sockaddr *dst, uint8_t dst_prefixlen,
				 const struct osmo_sockaddr *gw);
