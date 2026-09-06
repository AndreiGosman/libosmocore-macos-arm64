/*
 * libosmocore-macos-arm64: no-op stubs for the public symbols exported by
 * the Linux-only source files that this port wraps in #ifdef __linux__.
 *
 * Consumers (libosmovty, the Python bindings, gr-gsm) call these functions
 * and read these variables by symbol name. Without the stubs, dyld fails at
 * runtime with "symbol not found in flat namespace" when the Python module
 * gnuradio.gsm is loaded.
 *
 * Semantics: the functions return -1 or 0, a benign failure, and the
 * variables are zero initialised. Consumers are expected to handle the
 * fallback.
 *
 * This file is compiled into libosmocore.la by adding it to
 * libosmocore_la_SOURCES in src/core/Makefile.am.
 */

#ifdef __APPLE__

#include <stddef.h>

/* ============================================================
 * From src/core/stats_tcp.c, Linux only (needs sys/timerfd.h)
 * ============================================================ */

/* Global configuration variable. The real type is
 * struct osmo_stats_tcp_entry_cfg, but consumers only read and write its
 * fields. An aligned 256 byte object covers any reasonable structure. */
char osmo_tcp_stats_config[256] __attribute__((aligned(16))) = {0};

struct osmo_fd; /* forward declaration, the real one is in osmocom/core/select.h */

int osmo_stats_tcp_osmo_fd_register(struct osmo_fd *fd) {
    (void)fd;
    return 0;
}

int osmo_stats_tcp_osmo_fd_unregister(const struct osmo_fd *fd) {
    (void)fd;
    return 0;
}

int osmo_stats_tcp_set_interval(int interval) {
    (void)interval;
    return 0;
}

int osmo_stats_tcp_set_batch_size(int size) {
    (void)size;
    return 0;
}

int osmo_stats_tcp_get_interval(void) {
    return 0;
}

void osmo_stats_tcp_start(void) {}
void osmo_stats_tcp_stop(void) {}


/* ============================================================
 * From src/core/select.c, the timerfd family, Linux only sys/timerfd.h.
 * Called by stats.c and rate_ctr.c through osmo_fd polling.
 * ============================================================ */

struct timespec;

int osmo_timerfd_disable(struct osmo_fd *ofd) {
    (void)ofd;
    return -1;
}

int osmo_timerfd_schedule(struct osmo_fd *ofd,
                          const struct timespec *first,
                          const struct timespec *interval) {
    (void)ofd; (void)first; (void)interval;
    return -1;
}

int osmo_timerfd_setup(struct osmo_fd *ofd,
                      int (*cb)(struct osmo_fd *, unsigned int),
                      void *data) {
    (void)ofd; (void)cb; (void)data;
    return -1;
}


/* ============================================================
 * From src/core/tun.c, Linux only (linux/if_tun.h)
 * ============================================================ */

struct osmo_tundev {
    int placeholder;
};

struct osmo_tundev *osmo_tundev_alloc(void *ctx, const char *name) {
    (void)ctx; (void)name;
    return NULL;
}

void osmo_tundev_free(struct osmo_tundev *tun) {
    (void)tun;
}

int osmo_tundev_open(struct osmo_tundev *tun) {
    (void)tun;
    return -1;
}

int osmo_tundev_close(struct osmo_tundev *tun) {
    (void)tun;
    return -1;
}


/* Add stubs for further symbols here as dyld reports "symbol not found in
 * flat namespace" at runtime. The symbols above were found one at a time,
 * by running the Python import, reading the dyld error and adding the
 * missing symbol. */

#endif /* __APPLE__ */
