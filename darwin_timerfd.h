/*
 * libosmocore-macos-arm64: sys/timerfd.h for Darwin.
 *
 * Darwin has neither the timerfd API nor struct itimerspec. This header
 * declares both with the Linux names and constants, so that the upstream
 * src/core/select.c compiles its osmo_timerfd_* wrappers unchanged. The
 * implementation lives in src/core/darwin_timerfd.c and is exported from
 * libosmocore.dylib. No system library on Darwin defines these symbols, so
 * there is nothing to collide with.
 *
 * install.sh copies this file to include/sys/timerfd.h in the source tree,
 * which makes the configure check for sys/timerfd.h succeed and defines
 * HAVE_SYS_TIMERFD_H, and installs it again under $PREFIX/include/sys/ for
 * consumers that include <sys/timerfd.h> directly.
 *
 * See darwin_timerfd.c for the semantics that are and are not emulated.
 */

#ifndef LIBOSMOCORE_DARWIN_SYS_TIMERFD_H
#define LIBOSMOCORE_DARWIN_SYS_TIMERFD_H

#ifdef __APPLE__

#include <fcntl.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX interval timer specification. Darwin has no timer_create() and
 * therefore no itimerspec; the layout is the one POSIX defines. */
struct itimerspec {
	struct timespec it_interval;	/* period, zero for one-shot */
	struct timespec it_value;	/* first expiration, zero disarms */
};

/* timerfd_create() flags. Linux reuses the O_* values, and so do we. */
#define TFD_CLOEXEC		O_CLOEXEC
#define TFD_NONBLOCK		O_NONBLOCK

/* timerfd_settime() flags. */
#define TFD_TIMER_ABSTIME	(1 << 0)
#define TFD_TIMER_CANCEL_ON_SET	(1 << 1)

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
		    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */

#endif /* LIBOSMOCORE_DARWIN_SYS_TIMERFD_H */
