/*
 * libosmocore-macos-arm64: compat header for Linux constants missing on Darwin.
 *
 * Included in every compilation unit through `-include darwin_compat.h` in
 * CFLAGS. The dummy CLOCK_* values are chosen so that they do NOT collide
 * with the Darwin _clock_id enum (`_time.h`), which occupies 0, 4, 5, 6, 8,
 * 9, 12 and 16. At runtime clock_gettime() returns EINVAL for them, and
 * libosmocore falls back to the standard CLOCK_MONOTONIC through its own
 * try/if logic.
 */

#ifndef LIBOSMOCORE_DARWIN_COMPAT_H
#define LIBOSMOCORE_DARWIN_COMPAT_H

#ifdef __APPLE__

/* SO_PRIORITY: Linux socket QoS option, a no-op on Darwin.
 * The value 999 is safe: setsockopt() returns EINVAL at runtime, silently. */
#ifndef SO_PRIORITY
#define SO_PRIORITY 999
#endif

/* CLOCK_*_COARSE and CLOCK_BOOTTIME: Linux clock IDs that do not exist on
 * Darwin. The values 100-102 are dummies above the Darwin _clock_id enum. */
#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE 100
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE 101
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 102
#endif

/* gettid(): Linux syscall for the kernel thread ID. Darwin has no cheap
 * portable equivalent; the exact alternative is pthread_threadid_np(), but
 * it is an API call with an output pointer. Alias it to getpid(). The
 * semantics degrade in a multi-threaded process (every thread reports the
 * same ID), which is acceptable for the logging context of libosmocore. */
#include <unistd.h>
#include <sys/types.h>
#define gettid() ((pid_t)getpid())

#endif /* __APPLE__ */

#endif /* LIBOSMOCORE_DARWIN_COMPAT_H */
