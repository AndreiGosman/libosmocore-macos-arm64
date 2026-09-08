/*! \file darwin_netdev.c
 * Darwin backend for the osmo_netdev operations that libosmocore does
 * with netlink (libmnl) on Linux: address add/remove, MTU, up/down and
 * routes. Uses the BSD interface ioctls and a PF_ROUTE socket, which
 * need root (or the right entitlement) exactly as netlink does.
 *
 * Part of libosmocore-macos-arm64. Same licence as libosmocore, GPL-2.0+.
 */

#ifdef __APPLE__

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#include <netinet6/nd6.h>

#include <osmocom/core/socket.h>

#include "darwin_netdev.h"

static int ctl_socket(int family)
{
	int fd = socket(family, SOCK_DGRAM, 0);
	return fd < 0 ? -errno : fd;
}

static void mask_from_prefix4(struct sockaddr_in *sin, uint8_t prefixlen)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = prefixlen ? htonl(0xffffffffu << (32 - prefixlen)) : 0;
}

static void mask_from_prefix6(struct sockaddr_in6 *sin6, uint8_t prefixlen)
{
	unsigned int i;
	memset(sin6, 0, sizeof(*sin6));
	sin6->sin6_len = sizeof(*sin6);
	sin6->sin6_family = AF_INET6;
	for (i = 0; i < 16 && prefixlen > 0; i++, prefixlen = prefixlen >= 8 ? prefixlen - 8 : 0)
		sin6->sin6_addr.s6_addr[i] = prefixlen >= 8 ? 0xff : (uint8_t)(0xff << (8 - prefixlen));
}

/* SIOCGIFFLAGS: the interface flags, or a negative errno */
static int iface_flags(const char *dev_name, short *flags)
{
	struct ifreq ifr;
	int fd, rc;

	fd = ctl_socket(AF_INET);
	if (fd < 0)
		return fd;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, dev_name, sizeof(ifr.ifr_name));
	rc = ioctl(fd, SIOCGIFFLAGS, &ifr) < 0 ? -errno : 0;
	close(fd);
	if (rc == 0)
		*flags = ifr.ifr_flags;
	return rc;
}

/* Add or delete an address on the interface named dev_name.
 *
 * A utun is a point to point interface, and the BSD SIOCAIFADDR refuses an
 * address on a point to point interface without a destination address
 * (EDESTADDRREQ). Set the destination to the address itself, which is what
 * "ifconfig utunN inet A A netmask M" does and what VPN clients do on utun.
 *
 * On Linux an address with a prefix length also brings the route to that
 * prefix; on Darwin a point to point address only brings the host route to
 * the destination. Add the prefix route through the interface after the
 * address, so that the interface behaves as callers written for Linux
 * expect (osmo-ggsn routes the whole pool of an APN into its tun). */
int osmo_darwin_netdev_addr(const char *dev_name, const struct osmo_sockaddr *addr, uint8_t prefixlen, bool add)
{
	int fd, rc;

	switch (addr->u.sa.sa_family) {
	case AF_INET: {
		struct ifaliasreq ifra;
		short flags = 0;
		bool p2p;

		rc = iface_flags(dev_name, &flags);
		if (rc < 0)
			return rc;
		p2p = (flags & IFF_POINTOPOINT) != 0;

		memset(&ifra, 0, sizeof(ifra));
		strlcpy(ifra.ifra_name, dev_name, sizeof(ifra.ifra_name));
		memcpy(&ifra.ifra_addr, &addr->u.sin, sizeof(struct sockaddr_in));
		((struct sockaddr_in *)&ifra.ifra_addr)->sin_len = sizeof(struct sockaddr_in);
		mask_from_prefix4((struct sockaddr_in *)&ifra.ifra_mask, prefixlen);
		/* the slot is ifra_broadaddr; the kernel reads it as the
		 * destination on a point to point interface (Darwin's net/if.h
		 * has no ifra_dstaddr alias for it, unlike FreeBSD) */
		if (p2p)
			memcpy(&ifra.ifra_broadaddr, &ifra.ifra_addr, sizeof(struct sockaddr_in));
		fd = ctl_socket(AF_INET);
		if (fd < 0)
			return fd;
		rc = ioctl(fd, add ? SIOCAIFADDR : SIOCDIFADDR, &ifra);
		if (rc == 0 && add && p2p && prefixlen < 32) {
			struct osmo_sockaddr net;
			int rc2;

			memset(&net, 0, sizeof(net));
			net.u.sin.sin_family = AF_INET;
			net.u.sin.sin_addr.s_addr = addr->u.sin.sin_addr.s_addr &
				((struct sockaddr_in *)&ifra.ifra_mask)->sin_addr.s_addr;
			rc2 = osmo_darwin_netdev_add_route(dev_name, &net, prefixlen, NULL);
			if (rc2 < 0 && rc2 != -EEXIST) {
				close(fd);
				return rc2;
			}
		}
		break;
	}
	case AF_INET6: {
		struct in6_aliasreq ifra6;
		memset(&ifra6, 0, sizeof(ifra6));
		strlcpy(ifra6.ifra_name, dev_name, sizeof(ifra6.ifra_name));
		memcpy(&ifra6.ifra_addr, &addr->u.sin6, sizeof(struct sockaddr_in6));
		ifra6.ifra_addr.sin6_len = sizeof(struct sockaddr_in6);
		mask_from_prefix6(&ifra6.ifra_prefixmask, prefixlen);
		ifra6.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;
		ifra6.ifra_lifetime.ia6t_pltime = ND6_INFINITE_LIFETIME;
		fd = ctl_socket(AF_INET6);
		if (fd < 0)
			return fd;
		rc = ioctl(fd, add ? SIOCAIFADDR_IN6 : SIOCDIFADDR_IN6, &ifra6);
		break;
	}
	default:
		return -EAFNOSUPPORT;
	}
	rc = rc < 0 ? -errno : 0;
	close(fd);
	return rc;
}

int osmo_darwin_netdev_set_mtu(const char *dev_name, uint32_t mtu)
{
	struct ifreq ifr;
	int fd, rc;

	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, dev_name, sizeof(ifr.ifr_name));
	ifr.ifr_mtu = (int)mtu;
	fd = ctl_socket(AF_INET);
	if (fd < 0)
		return fd;
	rc = ioctl(fd, SIOCSIFMTU, &ifr) < 0 ? -errno : 0;
	close(fd);
	return rc;
}

int osmo_darwin_netdev_ifupdown(const char *dev_name, bool up)
{
	struct ifreq ifr;
	int fd, rc;

	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, dev_name, sizeof(ifr.ifr_name));
	fd = ctl_socket(AF_INET);
	if (fd < 0)
		return fd;
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	if (up)
		ifr.ifr_flags |= IFF_UP;
	else
		ifr.ifr_flags &= ~IFF_UP;
	rc = ioctl(fd, SIOCSIFFLAGS, &ifr) < 0 ? -errno : 0;
	close(fd);
	return rc;
}

/* RTM_ADD on a routing socket: destination, optional gateway, netmask,
 * and the interface as an AF_LINK gateway when no gateway is given. */
int osmo_darwin_netdev_add_route(const char *dev_name, const struct osmo_sockaddr *dst, uint8_t dst_prefixlen,
				 const struct osmo_sockaddr *gw)
{
	uint8_t buf[sizeof(struct rt_msghdr) + 4 * sizeof(struct sockaddr_storage)];
	struct rt_msghdr *rtm = (struct rt_msghdr *)buf;
	uint8_t *p = buf + sizeof(*rtm);
	static int seq;
	socklen_t len;
	int fd, rc;

	memset(buf, 0, sizeof(buf));
	rtm->rtm_version = RTM_VERSION;
	rtm->rtm_type = RTM_ADD;
	rtm->rtm_flags = RTF_UP | RTF_STATIC;
	rtm->rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
	rtm->rtm_seq = ++seq;
	rtm->rtm_pid = getpid();
	rtm->rtm_index = if_nametoindex(dev_name);
	if (rtm->rtm_index == 0)
		return -ENODEV;

	/* destination */
	len = osmo_sockaddr_size(dst);
	memcpy(p, &dst->u.sa, len);
	((struct sockaddr *)p)->sa_len = len;
	p += ((len + 3) & ~3);

	/* gateway: a host, or the interface itself */
	if (gw) {
		rtm->rtm_flags |= RTF_GATEWAY;
		len = osmo_sockaddr_size(gw);
		memcpy(p, &gw->u.sa, len);
		((struct sockaddr *)p)->sa_len = len;
	} else {
		struct sockaddr_dl *sdl = (struct sockaddr_dl *)p;
		sdl->sdl_len = sizeof(*sdl);
		sdl->sdl_family = AF_LINK;
		sdl->sdl_index = rtm->rtm_index;
		len = sizeof(*sdl);
	}
	p += ((len + 3) & ~3);

	/* netmask */
	if (dst->u.sa.sa_family == AF_INET6) {
		mask_from_prefix6((struct sockaddr_in6 *)p, dst_prefixlen);
		len = sizeof(struct sockaddr_in6);
	} else {
		mask_from_prefix4((struct sockaddr_in *)p, dst_prefixlen);
		len = sizeof(struct sockaddr_in);
		if (dst_prefixlen == 32)
			rtm->rtm_flags |= RTF_HOST;
	}
	p += ((len + 3) & ~3);
	rtm->rtm_msglen = p - buf;

	fd = socket(PF_ROUTE, SOCK_RAW, 0);
	if (fd < 0)
		return -errno;
	rc = write(fd, buf, rtm->rtm_msglen) < 0 ? -errno : 0;
	close(fd);
	return rc;
}

#endif /* __APPLE__ */
