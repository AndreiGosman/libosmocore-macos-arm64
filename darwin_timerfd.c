/*
 * libosmocore-macos-arm64: timerfd emulation for Darwin.
 *
 * Linux timerfd_create() returns a descriptor that becomes readable when the
 * timer expires; read() returns the number of expirations (uint64_t) since
 * the previous read. libosmocore wraps it in osmo_timerfd_setup() and
 * osmo_timerfd_schedule() (src/core/select.c) and registers the descriptor
 * with osmo_fd_register(), so the stats and rate counter subsystems get their
 * periodic tick through the ordinary event loop.
 *
 * Darwin has no timerfd. The native timing primitive is kqueue EVFILT_TIMER,
 * but a kqueue descriptor only reports readiness; it cannot be read(), and
 * stats.c asserts that read() returns exactly eight bytes. This file
 * therefore emulates the Linux contract with three parts per timer:
 *
 *   1. A pipe. The read end is the descriptor returned to the caller. It
 *      works with select(), poll() and plain close(), and read() on it
 *      returns the eight-byte expiration count the caller expects.
 *   2. A kqueue with an EVFILT_TIMER. kqueue coalesces missed expirations
 *      into the event's data field, which is exactly the count Linux
 *      reports through read().
 *   3. A worker thread that waits on the kqueue and writes the count into
 *      the pipe on every expiration.
 *
 * Emulated:
 *   - CLOCK_MONOTONIC and CLOCK_REALTIME. Both run on the kqueue timer,
 *     which is monotonic; the clock id only matters for TFD_TIMER_ABSTIME,
 *     where the absolute value is converted to a delay on that clock.
 *   - TFD_NONBLOCK and TFD_CLOEXEC on the returned descriptor.
 *   - it_value as the first expiration and it_interval as the period, as
 *     separate values. The first shot is a one-shot kevent; the worker then
 *     re-arms a repeating kevent with the period, which does not drift.
 *   - it_value of zero disarms, as on Linux. it_interval of zero is one-shot.
 *   - TFD_TIMER_ABSTIME, by conversion to a relative delay at settime.
 *   - timerfd_gettime() and the old_value argument of timerfd_settime():
 *     time remaining until the next expiration, and the current interval.
 *   - close(): the worker watches the write end with EVFILT_WRITE, and
 *     kqueue raises EV_EOF there when the reader is gone. The worker then
 *     releases the timer and exits. Nothing has to be called besides close().
 *   - fork(): threads and kqueue descriptors do not survive a fork on Darwin,
 *     but the pipe does. An atfork child handler rebuilds the kqueue, re-arms
 *     the timer from its recorded deadline and starts a new worker, so that
 *     osmo_daemonize() keeps the stats timers alive in the daemon child.
 *
 * Not emulated, with the reason:
 *   - TFD_TIMER_CANCEL_ON_SET returns EINVAL. It needs a notification when
 *     CLOCK_REALTIME jumps, which Darwin does not offer; Osmocom does not
 *     use it.
 *   - A settime() does not discard an expiration that is already in the pipe
 *     but not yet read. Linux resets the count to zero at that moment. The
 *     effect here is at most one extra tick after a reschedule.
 *   - A TFD_TIMER_ABSTIME timer on CLOCK_REALTIME is converted once, at
 *     settime(), and does not follow later wall clock changes.
 *   - If another process inherited the read end (a fork without exec, or an
 *     exec without TFD_CLOEXEC), close() in this process does not produce
 *     the EOF and the worker stays until that process closes it too.
 *
 * All timers share one mutex. An Osmocom daemon has two to four of them
 * (stats, rate counters, TCP stats), so contention is not a concern.
 *
 * This file is compiled into libosmocore.la by adding it to
 * libosmocore_la_SOURCES in src/core/Makefile.am, next to darwin_stubs.c.
 * The declarations are in include/sys/timerfd.h, put there by install.sh
 * so that the configure check for sys/timerfd.h succeeds and select.c
 * compiles its timerfd wrappers unchanged.
 */

#ifdef __APPLE__

#include <sys/timerfd.h>

#include <sys/event.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_S 1000000000LL

/* kevent identifiers. The timer uses the generation number as its ident,
 * so that an expiration of a previous schedule that is still in flight can
 * be told apart from the current one. The other two filters have fixed
 * idents; kqueue keys a knote by (ident, filter), so they cannot collide
 * with the timer. */
#define IDENT_WAKE 0

struct darwin_timerfd {
	struct darwin_timerfd *next;	/* global list */
	int rfd;			/* read end, handed to the caller */
	int wfd;			/* write end, used by the worker only */
	int kq;				/* one kqueue per timer */
	int clockid;
	pthread_t worker;
	int armed;			/* a kevent timer is registered */
	int periodic;			/* that timer is the repeating one, not the first shot */
	uintptr_t gen;			/* ident of the current kevent timer, bumped on every settime */
	struct itimerspec spec;		/* current schedule, relative */
	int64_t next_ns;		/* CLOCK_MONOTONIC deadline of the next expiration, while armed */
	uint64_t pending;		/* expirations the full pipe could not take yet */
	int dead;			/* unlinked from the list; the worker frees it */
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct darwin_timerfd *g_list;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* ---- time helpers ---- */

static int64_t ts_to_ns(const struct timespec *ts)
{
	if (ts->tv_sec > INT64_MAX / NS_PER_S - 1)
		return INT64_MAX;
	return (int64_t)ts->tv_sec * NS_PER_S + ts->tv_nsec;
}

static void ns_to_ts(int64_t ns, struct timespec *ts)
{
	if (ns < 0)
		ns = 0;
	ts->tv_sec = ns / NS_PER_S;
	ts->tv_nsec = ns % NS_PER_S;
}

static int ts_valid(const struct timespec *ts)
{
	return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < NS_PER_S;
}

static int64_t now_ns(clockid_t clk)
{
	struct timespec ts;

	if (clock_gettime(clk, &ts) < 0)
		return 0;
	return ts_to_ns(&ts);
}

/* ---- kqueue helpers, all called with g_lock held or from the owner ---- */

static int kq_arm(struct darwin_timerfd *t, int64_t delay_ns, int oneshot)
{
	struct kevent ev;
	uint16_t flags = EV_ADD | EV_ENABLE;

	if (oneshot)
		flags |= EV_ONESHOT;
	if (delay_ns < 1)
		delay_ns = 1;
	EV_SET(&ev, t->gen, EVFILT_TIMER, flags, NOTE_NSECONDS, (intptr_t)delay_ns, NULL);
	return kevent(t->kq, &ev, 1, NULL, 0, NULL);
}

static void kq_disarm(struct darwin_timerfd *t)
{
	struct kevent ev;

	/* ENOENT is expected when a one-shot has already fired: the kernel
	 * removed the knote itself. */
	EV_SET(&ev, t->gen, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
	kevent(t->kq, &ev, 1, NULL, 0, NULL);
}

/* Watch the write end of the pipe. EV_CLEAR makes this edge triggered,
 * otherwise a writable pipe would wake the worker in a loop. When the last
 * reader closes, kqueue delivers the event with EV_EOF set. */
static int kq_watch_reader(struct darwin_timerfd *t)
{
	struct kevent ev;

	EV_SET(&ev, t->wfd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
	return kevent(t->kq, &ev, 1, NULL, 0, NULL);
}

/* A user event to wake the worker when the timer is dropped from the list
 * without the reader being closed first, see reap_stale(). */
static int kq_add_wake(struct darwin_timerfd *t)
{
	struct kevent ev;

	EV_SET(&ev, IDENT_WAKE, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
	return kevent(t->kq, &ev, 1, NULL, 0, NULL);
}

static void kq_wake(struct darwin_timerfd *t)
{
	struct kevent ev;

	EV_SET(&ev, IDENT_WAKE, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
	kevent(t->kq, &ev, 1, NULL, 0, NULL);
}

/* ---- list helpers, g_lock held ---- */

static struct darwin_timerfd *lookup(int fd)
{
	struct darwin_timerfd *t;

	for (t = g_list; t; t = t->next)
		if (t->rfd == fd && !t->dead)
			return t;
	return NULL;
}

static void unlink_entry(struct darwin_timerfd *t)
{
	struct darwin_timerfd **pp;

	for (pp = &g_list; *pp; pp = &(*pp)->next) {
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	}
	t->next = NULL;
	t->dead = 1;
}

/*
 * The kernel just handed out descriptor number fd. If an entry in the list
 * still carries that number as its read end, the caller closed it and the
 * worker has not processed the EOF yet. Drop the entry now, so that a
 * lookup on the new descriptor cannot land on the old timer, and wake the
 * worker so that it releases the rest.
 */
static void reap_stale(int fd)
{
	struct darwin_timerfd *t = lookup(fd);

	if (!t)
		return;
	unlink_entry(t);
	kq_wake(t);
}

/* Time remaining and interval, in the shape timerfd_gettime() returns. */
static void fill_current(const struct darwin_timerfd *t, struct itimerspec *its)
{
	int64_t interval_ns, remaining;

	memset(its, 0, sizeof(*its));
	its->it_interval = t->spec.it_interval;
	if (!t->armed)
		return;

	interval_ns = ts_to_ns(&t->spec.it_interval);
	remaining = t->next_ns - now_ns(CLOCK_MONOTONIC);
	if (remaining <= 0) {
		/* Expired, but the worker has not advanced the deadline yet. */
		if (interval_ns <= 0)
			return;
		remaining = interval_ns - ((-remaining) % interval_ns);
	}
	ns_to_ts(remaining, &its->it_value);
}

/* ---- worker ---- */

/* Write the accumulated count. The write end is non-blocking; a full pipe
 * keeps the count in t->pending and the EVFILT_WRITE event retries it when
 * the reader has drained some. Called with g_lock held. */
static void flush_pending(struct darwin_timerfd *t)
{
	uint64_t count = t->pending;
	ssize_t rc;

	if (!count)
		return;
	rc = write(t->wfd, &count, sizeof(count));
	if (rc == (ssize_t)sizeof(count))
		t->pending = 0;
	/* EAGAIN: keep pending. EPIPE: the reader is gone, the EOF event
	 * follows and the worker exits. Either way there is nothing to do. */
}

static void worker_exit(struct darwin_timerfd *t)
{
	pthread_mutex_lock(&g_lock);
	if (!t->dead)
		unlink_entry(t);
	pthread_mutex_unlock(&g_lock);

	close(t->wfd);
	close(t->kq);
	free(t);
}

static void *worker(void *arg)
{
	struct darwin_timerfd *t = arg;
	struct kevent ev;
	int64_t interval_ns;
	uint64_t count;
	int n;

	for (;;) {
		n = kevent(t->kq, NULL, 0, &ev, 1, NULL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			continue;

		if (ev.filter == EVFILT_USER)
			break;

		if (ev.filter == EVFILT_WRITE) {
			if (ev.flags & EV_EOF)
				break;
			/* Space in the pipe again, or a spurious edge. */
			pthread_mutex_lock(&g_lock);
			if (!t->dead)
				flush_pending(t);
			pthread_mutex_unlock(&g_lock);
			continue;
		}

		if (ev.filter != EVFILT_TIMER)
			continue;

		pthread_mutex_lock(&g_lock);
		if (t->dead) {
			pthread_mutex_unlock(&g_lock);
			break;
		}
		if (!t->armed || ev.ident != t->gen) {
			/* An expiration of a schedule that settime() replaced. */
			pthread_mutex_unlock(&g_lock);
			continue;
		}

		/* For the repeating timer, data is the number of periods that
		 * elapsed since the last kevent(), which is the coalesced count
		 * Linux reports. For a one-shot knote XNU fills in the same
		 * quotient, (now - deadline) / delay + 1, so a 1 us first shot
		 * that is picked up 20 us late reads as 20. A one-shot fires
		 * once; only the periodic branch trusts data. */
		count = 1;
		interval_ns = ts_to_ns(&t->spec.it_interval);

		if (interval_ns == 0) {
			/* One-shot. The kernel removed the knote already. */
			t->armed = 0;
			t->periodic = 0;
		} else if (!t->periodic) {
			/* The first shot fired. The repeating timer takes over
			 * with the same ident; the one-shot knote is gone. */
			if (kq_arm(t, interval_ns, 0) == 0) {
				t->periodic = 1;
				t->next_ns += interval_ns;
			} else {
				t->armed = 0;
			}
		} else {
			if (ev.data > 1)
				count = (uint64_t)ev.data;
			t->next_ns += interval_ns * (int64_t)count;
		}

		t->pending += count;
		flush_pending(t);
		pthread_mutex_unlock(&g_lock);
	}

	worker_exit(t);
	return NULL;
}

static int start_worker(struct darwin_timerfd *t)
{
	pthread_attr_t attr;
	int rc;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	rc = pthread_create(&t->worker, &attr, worker, t);
	pthread_attr_destroy(&attr);
	return rc == 0 ? 0 : -1;
}

/* ---- fork ---- */

static void atfork_prepare(void)
{
	pthread_mutex_lock(&g_lock);
}

static void atfork_parent(void)
{
	pthread_mutex_unlock(&g_lock);
}

/*
 * In the child: the pipe descriptors are inherited, the kqueue descriptors
 * are not (the child does not even get their numbers), and the worker
 * threads do not exist. Rebuild each live timer. The lock is held by the
 * thread that forked, so the list is consistent.
 */
static void atfork_child(void)
{
	struct darwin_timerfd *t;
	int64_t remaining;

	for (t = g_list; t; t = t->next) {
		t->kq = kqueue();
		if (t->kq < 0) {
			t->armed = 0;
			continue;
		}
		fcntl(t->kq, F_SETFD, FD_CLOEXEC);
		kq_watch_reader(t);
		kq_add_wake(t);

		if (t->armed) {
			remaining = t->next_ns - now_ns(CLOCK_MONOTONIC);
			t->gen++;
			/* One shot with what is left of the current period; the
			 * worker re-arms the repeating timer after it. */
			t->periodic = 0;
			if (kq_arm(t, remaining, 1) < 0)
				t->armed = 0;
		}
		start_worker(t);
	}
	pthread_mutex_unlock(&g_lock);
}

static void init_once(void)
{
	pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
}

/* ---- public API ---- */

int timerfd_create(int clockid, int flags)
{
	struct darwin_timerfd *t;
	int fds[2];
	int saved;

	if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME) {
		errno = EINVAL;
		return -1;
	}
	if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
		errno = EINVAL;
		return -1;
	}

	pthread_once(&g_once, init_once);

	if (pipe(fds) < 0)
		return -1;

	/* The write end is ours: never inherited across exec, never blocking
	 * the worker, never raising SIGPIPE in the daemon. */
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
	fcntl(fds[1], F_SETNOSIGPIPE, 1);
	if (flags & TFD_NONBLOCK)
		fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
	if (flags & TFD_CLOEXEC)
		fcntl(fds[0], F_SETFD, FD_CLOEXEC);

	t = calloc(1, sizeof(*t));
	if (!t) {
		saved = ENOMEM;
		goto fail_fds;
	}
	t->rfd = fds[0];
	t->wfd = fds[1];
	t->clockid = clockid;
	t->gen = 1;

	t->kq = kqueue();
	if (t->kq < 0) {
		saved = errno;
		goto fail_alloc;
	}
	fcntl(t->kq, F_SETFD, FD_CLOEXEC);
	if (kq_watch_reader(t) < 0 || kq_add_wake(t) < 0) {
		saved = errno;
		goto fail_kq;
	}

	pthread_mutex_lock(&g_lock);
	reap_stale(fds[0]);
	reap_stale(fds[1]);
	if (start_worker(t) < 0) {
		pthread_mutex_unlock(&g_lock);
		saved = EAGAIN;
		goto fail_kq;
	}
	t->next = g_list;
	g_list = t;
	pthread_mutex_unlock(&g_lock);

	return fds[0];

fail_kq:
	close(t->kq);
fail_alloc:
	free(t);
fail_fds:
	close(fds[0]);
	close(fds[1]);
	errno = saved;
	return -1;
}

int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
		    struct itimerspec *old_value)
{
	struct darwin_timerfd *t;
	int64_t value_ns, delay_ns;

	if (!new_value) {
		errno = EFAULT;
		return -1;
	}
	if (flags & ~TFD_TIMER_ABSTIME) {
		/* TFD_TIMER_CANCEL_ON_SET and anything unknown. */
		errno = EINVAL;
		return -1;
	}
	if (!ts_valid(&new_value->it_value) || !ts_valid(&new_value->it_interval)) {
		errno = EINVAL;
		return -1;
	}
	if (fcntl(fd, F_GETFD) < 0) {
		errno = EBADF;
		return -1;
	}

	pthread_mutex_lock(&g_lock);
	t = lookup(fd);
	if (!t) {
		pthread_mutex_unlock(&g_lock);
		errno = EINVAL;
		return -1;
	}

	if (old_value)
		fill_current(t, old_value);

	/* Replace the schedule. A new generation makes the worker drop any
	 * expiration of the old one that is still in flight. */
	if (t->armed)
		kq_disarm(t);
	t->armed = 0;
	t->periodic = 0;
	t->pending = 0;
	t->gen++;
	t->spec.it_interval = new_value->it_interval;

	value_ns = ts_to_ns(&new_value->it_value);
	if (value_ns == 0) {
		/* Disarm, whatever the interval says. */
		memset(&t->spec.it_value, 0, sizeof(t->spec.it_value));
		pthread_mutex_unlock(&g_lock);
		return 0;
	}

	if (flags & TFD_TIMER_ABSTIME)
		delay_ns = value_ns - now_ns(t->clockid);
	else
		delay_ns = value_ns;
	if (delay_ns < 1)
		delay_ns = 1;

	ns_to_ts(delay_ns, &t->spec.it_value);
	t->next_ns = now_ns(CLOCK_MONOTONIC) + delay_ns;
	if (kq_arm(t, delay_ns, 1) < 0) {
		int saved = errno;
		pthread_mutex_unlock(&g_lock);
		errno = saved;
		return -1;
	}
	t->armed = 1;
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int timerfd_gettime(int fd, struct itimerspec *curr_value)
{
	struct darwin_timerfd *t;

	if (!curr_value) {
		errno = EFAULT;
		return -1;
	}
	if (fcntl(fd, F_GETFD) < 0) {
		errno = EBADF;
		return -1;
	}

	pthread_mutex_lock(&g_lock);
	t = lookup(fd);
	if (!t) {
		pthread_mutex_unlock(&g_lock);
		errno = EINVAL;
		return -1;
	}
	fill_current(t, curr_value);
	pthread_mutex_unlock(&g_lock);
	return 0;
}

#endif /* __APPLE__ */
