/*
 * libosmocore-macos-arm64: compat header pentru constante Linux lipsa pe Darwin.
 *
 * Se include automat la compile prin `-include darwin_compat.h` in CFLAGS.
 * Valorile pentru CLOCK_* dummy sunt alese astfel incat sa NU se ciocneasca
 * cu enum-ul _clock_id din Darwin (`_time.h`), care ocupa 0,4,5,6,8,9,12,16.
 * La runtime, clock_gettime() cu ele intoarce EINVAL - libosmocore are
 * fallback la CLOCK_MONOTONIC standard prin try/if.
 */

#ifndef LIBOSMOCORE_DARWIN_COMPAT_H
#define LIBOSMOCORE_DARWIN_COMPAT_H

#ifdef __APPLE__

/* SO_PRIORITY: Linux socket QoS option, no-op pe Darwin.
 * Valoarea 999 e safe - setsockopt() va intoarce EINVAL la runtime silent. */
#ifndef SO_PRIORITY
#define SO_PRIORITY 999
#endif

/* CLOCK_*_COARSE si CLOCK_BOOTTIME: Linux clock IDs, nu exista pe Darwin.
 * Valorile 100-102 sunt dummy alese peste enum-ul Darwin _clock_id. */
#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE 100
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE 101
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 102
#endif

/* gettid(): Linux syscall pentru thread ID kernel. Pe Darwin nu exista
 * echivalent portabil ieftin; alternativa exacta e pthread_threadid_np() dar
 * cere apel API cu pointer output. Aliazam la getpid() - semantic degradat
 * intr-un proces multi-thread (toate thread-urile raporteaza acelasi ID),
 * dar acceptabil pentru contextul logging al libosmocore. */
#include <unistd.h>
#include <sys/types.h>
#define gettid() ((pid_t)getpid())

#endif /* __APPLE__ */

#endif /* LIBOSMOCORE_DARWIN_COMPAT_H */
