/*
 * libosmocore-macos-arm64: stub-uri no-op pentru simboluri publice exportate
 * din fisiere Linux-only wrap-uite in #ifdef __linux__.
 *
 * Consumatori (libosmovty, bindings Python, gr-gsm) cheama aceste functii/
 * citesc aceste variabile prin symbol name. Fara stub-uri, dyld esueaza
 * la runtime cu "symbol not found in flat namespace" la incarcarea
 * modulului Python `gnuradio.gsm`.
 *
 * Semantic: functiile intorc -1 sau 0 (fail benign), variabilele sunt
 * zero-initialized. Consumatorii ar trebui sa gestioneze fallback.
 *
 * Se compileaza in libosmocore.la prin adaugare in src/core/Makefile.am
 * la libosmocore_la_SOURCES.
 */

#ifdef __APPLE__

#include <stddef.h>

/* ============================================================
 * Din src/core/stats_tcp.c - Linux-only (needs sys/timerfd.h)
 * ============================================================ */

/* Variabila globala config, tipul real e struct osmo_stats_tcp_entry_cfg
 * dar consumatorii doar citesc/scriu campuri. 256 bytes aliniata acopera
 * orice struct rezonabila. */
char osmo_tcp_stats_config[256] __attribute__((aligned(16))) = {0};

struct osmo_fd; /* forward decl - definitia reala in osmocom/core/select.h */

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
 * Din src/core/select.c pentru timerfd - Linux-only sys/timerfd.h
 * Chemate de stats.c, rate_ctr.c prin osmo_fd polling
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
 * Din src/core/tun.c - Linux-only (linux/if_tun.h)
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


/* La nevoie, adauga stubs pentru simboluri suplimentare aici pe masura
 * ce dyld semnaleaza "symbol not found in flat namespace" la runtime.
 * Simbolurile listate mai sus au fost identificate iterativ prin rulare
 * import Python cu observarea erorii de dyld si adaugare pas cu pas. */

#endif /* __APPLE__ */
