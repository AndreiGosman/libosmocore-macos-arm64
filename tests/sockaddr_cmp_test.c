/*
 * Regression test for patch 011: osmo_sockaddr_cmp() must treat an address
 * the application built (sin_len == 0) and the same address as returned by
 * recvfrom() (sin_len filled in by the kernel) as equal.
 *
 * Build and run after `install.sh`:
 *   cc tests/sockaddr_cmp_test.c -o /tmp/sockaddr_cmp_test \
 *      $(pkg-config --cflags --libs libosmocore) && /tmp/sockaddr_cmp_test
 * Exit status 0 means pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <osmocom/core/socket.h>

static int check(const char *what, int got, int want)
{
	printf("%-52s %s (%d)\n", what, got == want ? "OK" : "FAIL", got);
	return got == want ? 0 : 1;
}

int main(void)
{
	int fail = 0;
	int rx = socket(AF_INET, SOCK_DGRAM, 0);
	int tx = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(23111),
				 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	struct sockaddr_in b = { .sin_family = AF_INET, .sin_port = htons(23112),
				 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	struct osmo_sockaddr from, cfg, other;
	socklen_t fl = sizeof(from);
	char buf[4];

	if (rx < 0 || tx < 0 || bind(rx, (struct sockaddr *)&a, sizeof(a)) ||
	    bind(tx, (struct sockaddr *)&b, sizeof(b))) {
		perror("socket/bind");
		return 2;
	}
	if (sendto(tx, "x", 1, 0, (struct sockaddr *)&a, sizeof(a)) != 1 ||
	    recvfrom(rx, buf, sizeof(buf), 0, &from.u.sa, &fl) != 1) {
		perror("sendto/recvfrom");
		return 2;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.u.sin = b;			/* what a config file produces */
	other = cfg;
	other.u.sin.sin_port = htons(23113);

	printf("recvfrom() sin_len=%u, configured sin_len=%u\n",
	       from.u.sin.sin_len, cfg.u.sin.sin_len);
	fail += check("configured == received (same peer)", osmo_sockaddr_cmp(&cfg, &from), 0);
	fail += check("received == configured (symmetric)", osmo_sockaddr_cmp(&from, &cfg), 0);
	fail += check("different port is not equal", osmo_sockaddr_cmp(&cfg, &other) != 0, 1);
	fail += check("a != b implies b != a", osmo_sockaddr_cmp(&other, &cfg) != 0, 1);

	struct osmo_sockaddr s6a, s6b;
	memset(&s6a, 0, sizeof(s6a)); memset(&s6b, 0, sizeof(s6b));
	s6a.u.sin6.sin6_family = s6b.u.sin6.sin6_family = AF_INET6;
	s6a.u.sin6.sin6_port = s6b.u.sin6.sin6_port = htons(23000);
	s6a.u.sin6.sin6_addr = s6b.u.sin6.sin6_addr = in6addr_loopback;
	s6b.u.sin6.sin6_len = sizeof(struct sockaddr_in6);
	fail += check("IPv6: only sin6_len differs, equal", osmo_sockaddr_cmp(&s6a, &s6b), 0);
	s6b.u.sin6.sin6_scope_id = 1;
	fail += check("IPv6: scope id differs, not equal", osmo_sockaddr_cmp(&s6a, &s6b) != 0, 1);

	close(rx); close(tx);
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail ? 1 : 0;
}
