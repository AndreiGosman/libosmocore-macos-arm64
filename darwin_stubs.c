/*
 * libosmocore-macos-arm64: no-op stubs for the public symbols exported by
 * the Linux-only source files that this port wraps in #ifdef __linux__.
 *
 * Consumers (libosmovty, the Python bindings, gr-gsm) call these functions
 * and read these variables by symbol name. Without the stubs, dyld fails at
 * runtime with "symbol not found in flat namespace" when the Python module
 * gnuradio.gsm is loaded.
 *
 * Semantics: the functions return -1 or 0, a benign failure, and consumers
 * are expected to handle the fallback. A variable is a different matter. If
 * upstream code dereferences it unconditionally, the stub has to carry a
 * usable value rather than a zeroed placeholder. See osmo_tcp_stats_config
 * below.
 *
 * This file is compiled into libosmocore.la by adding it to
 * libosmocore_la_SOURCES in src/core/Makefile.am.
 */

#ifdef __APPLE__

#include <stddef.h>

/* stats_tcp.h names struct osmo_fd in its prototypes without declaring it,
 * so select.h has to come first for the definitions below to match the
 * header. Patch 004 fixes the header upstream; this include keeps the file
 * correct either way. */
#include <osmocom/core/select.h>
#include <osmocom/core/stats_tcp.h>
#include <osmocom/core/tun.h>

/* ============================================================
 * From src/core/stats_tcp.c, Linux only (needs sys/timerfd.h)
 * ============================================================ */

/*
 * Upstream declares this as a pointer and defines it outside its
 * HAVE_LINUX_TCP_H guard on purpose, because osmo_stats_init() dereferences
 * it on every platform:
 *
 *     osmo_stats_tcp_set_interval(osmo_tcp_stats_config->interval);
 *
 * An earlier version of this file exported a zeroed char[256] instead. The
 * first eight bytes then read back as a null pointer and every Osmocom
 * daemon died with SIGSEGV at address 0 during startup, before it could
 * print its own version string. The stub has to be a real pointer to a real
 * structure.
 */
static struct osmo_tcp_stats_config s_tcp_stats_config = {
	.interval = TCP_STATS_DEFAULT_INTERVAL,
};
struct osmo_tcp_stats_config *osmo_tcp_stats_config = &s_tcp_stats_config;

/* The signatures below match include/osmocom/core/stats_tcp.h exactly. The
 * register function takes two arguments, not one: passing a second argument
 * to a one-argument definition happens to work on arm64, where the extra
 * value simply sits unused in a register, but it is still wrong. */
int osmo_stats_tcp_osmo_fd_register(const struct osmo_fd *fd, const char *name)
{
	(void)fd;
	(void)name;
	return 0;
}

int osmo_stats_tcp_osmo_fd_unregister(const struct osmo_fd *fd)
{
	(void)fd;
	return 0;
}

int osmo_stats_tcp_set_interval(int interval)
{
	(void)interval;
	return 0;
}

/*
 * osmo_stats_tcp_set_batch_size, osmo_stats_tcp_get_interval,
 * osmo_stats_tcp_start and osmo_stats_tcp_stop used to be stubbed here. No
 * such symbols exist anywhere in libosmocore, so the stubs only added four
 * invented entries to the export table of this port.
 */

/*
 * osmo_timerfd_disable, osmo_timerfd_schedule and osmo_timerfd_setup from
 * src/core/select.c used to be stubbed here, returning -1. That disabled the
 * stats and rate counter timers of every daemon at startup, with two
 * stats.c error lines as the only symptom. Since v0.2.1 the upstream
 * functions compile as they are, on top of the timerfd emulation in
 * darwin_timerfd.c and the sys/timerfd.h header that install.sh provides.
 */

/* ============================================================
 * From src/core/tun.c, Linux only (linux/if_tun.h)
 * ============================================================ */

struct osmo_tundev *osmo_tundev_alloc(void *ctx, const char *name)
{
	(void)ctx;
	(void)name;
	return NULL;
}

void osmo_tundev_free(struct osmo_tundev *tundev)
{
	(void)tundev;
}

int osmo_tundev_open(struct osmo_tundev *tundev)
{
	(void)tundev;
	return -1;
}

int osmo_tundev_close(struct osmo_tundev *tundev)
{
	(void)tundev;
	return -1;
}

/* Add stubs for further symbols here as dyld reports "symbol not found in
 * flat namespace" at runtime. The symbols above were found one at a time,
 * by running the Python import, reading the dyld error and adding the
 * missing symbol. Check any new signature against the upstream header
 * rather than guessing it from the call site. */

#endif /* __APPLE__ */
