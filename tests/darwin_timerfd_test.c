/*
 * libosmocore-macos-arm64: standalone test for the Darwin timerfd emulation.
 *
 * Exercises the raw timerfd API from a select() loop: 100 ms periodic
 * ticks (the timing check), a first expiration distinct from the interval,
 * coalescing of missed expirations, timerfd_gettime(), disarm, one-shot,
 * CLOCK_REALTIME, rejected flags, close() detection with descriptor reuse,
 * and fork(). Build and run instructions are in the README.
 */

#include <sys/timerfd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>

static long ms_since(const struct timeval *t0)
{
	struct timeval t1;
	gettimeofday(&t1, NULL);
	return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_usec - t0->tv_usec) / 1000;
}

static int wait_readable(int fd, int timeout_ms)
{
	fd_set rfds;
	struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	return select(fd + 1, &rfds, NULL, NULL, &tv);
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Test 1: the handover case. 100 ms periodic, 5 ticks, each 80..120 ms apart. */
static void test_periodic(void)
{
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	struct itimerspec spec = {
		.it_value = { 0, 100000000 },
		.it_interval = { 0, 100000000 },
	};
	struct timeval t0;
	long prev = 0;
	int i;

	printf("test 1: periodic 100 ms x 5\n");
	CHECK(fd >= 0, "timerfd_create: %s", strerror(errno));
	CHECK(timerfd_settime(fd, 0, &spec, NULL) == 0, "settime: %s", strerror(errno));
	gettimeofday(&t0, NULL);
	for (i = 0; i < 5; i++) {
		uint64_t exp = 0;
		int n = wait_readable(fd, 1000);
		long ms = ms_since(&t0);
		ssize_t rc;
		CHECK(n > 0, "select timeout at iter %d", i);
		rc = read(fd, &exp, sizeof(exp));
		printf("  tick %d at %ld ms (delta %ld ms), expirations=%llu, read=%zd\n",
		       i, ms, ms - prev, (unsigned long long)exp, rc);
		CHECK(rc == 8, "read returned %zd, not 8", rc);
		CHECK(exp == 1, "expirations %llu, expected 1", (unsigned long long)exp);
		CHECK(ms - prev >= 80 && ms - prev <= 120, "delta %ld ms outside 80..120", ms - prev);
		prev = ms;
	}
	/* Non-blocking read with nothing pending must give EAGAIN, as on Linux. */
	{
		uint64_t exp;
		ssize_t rc = read(fd, &exp, sizeof(exp));
		CHECK(rc < 0 && errno == EAGAIN, "empty read: rc=%zd errno=%d", rc, errno);
	}
	close(fd);
}

/* Test 2: first expiration differs from the interval (stats.c uses 1 us then 5 s). */
static void test_first_then_interval(void)
{
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	struct itimerspec spec = {
		.it_value = { 0, 1000 },		/* 1 us */
		.it_interval = { 0, 150000000 },	/* 150 ms */
	};
	struct timeval t0;
	uint64_t exp;
	long ms;

	printf("test 2: first 1 us, then 150 ms\n");
	CHECK(timerfd_settime(fd, 0, &spec, NULL) == 0, "settime");
	gettimeofday(&t0, NULL);
	CHECK(wait_readable(fd, 1000) > 0, "first shot did not arrive");
	ms = ms_since(&t0);
	read(fd, &exp, sizeof(exp));
	printf("  first at %ld ms, expirations=%llu\n", ms, (unsigned long long)exp);
	CHECK(ms <= 20, "first shot late: %ld ms", ms);
	CHECK(wait_readable(fd, 1000) > 0, "second shot did not arrive");
	ms = ms_since(&t0);
	read(fd, &exp, sizeof(exp));
	printf("  second at %ld ms, expirations=%llu\n", ms, (unsigned long long)exp);
	CHECK(ms >= 130 && ms <= 170, "second shot at %ld ms, expected ~150", ms);
	close(fd);
}

/* Test 3: a slow reader gets the coalesced count, and gettime reports the remainder. */
static void test_coalesce_and_gettime(void)
{
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	struct itimerspec spec = {
		.it_value = { 0, 50000000 },
		.it_interval = { 0, 50000000 },
	};
	struct itimerspec cur;
	uint64_t exp = 0, total = 0;
	ssize_t rc;

	printf("test 3: coalescing and gettime\n");
	CHECK(timerfd_settime(fd, 0, &spec, NULL) == 0, "settime");
	CHECK(timerfd_gettime(fd, &cur) == 0, "gettime");
	printf("  gettime right after settime: value=%ld.%09ld interval=%ld.%09ld\n",
	       (long)cur.it_value.tv_sec, cur.it_value.tv_nsec,
	       (long)cur.it_interval.tv_sec, cur.it_interval.tv_nsec);
	CHECK(cur.it_value.tv_nsec > 30000000 && cur.it_value.tv_nsec <= 50000000, "remaining out of range");
	CHECK(cur.it_interval.tv_nsec == 50000000, "interval not reported");
	usleep(375000);	/* 7 expirations at 50, 100, ..., 350 ms */
	while ((rc = read(fd, &exp, sizeof(exp))) == 8)
		total += exp;
	printf("  after 375 ms sleep: total expirations=%llu\n", (unsigned long long)total);
	CHECK(total >= 6 && total <= 8, "expected 7 (6..8), got %llu", (unsigned long long)total);
	close(fd);
}

/* Test 4: disarm stops the ticks; one-shot fires once. */
static void test_disarm_and_oneshot(void)
{
	int fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
	struct itimerspec spec = {
		.it_value = { 0, 30000000 },
		.it_interval = { 0, 30000000 },
	};
	struct itimerspec zero = { { 0, 0 }, { 0, 0 } };
	struct itimerspec old;
	uint64_t exp;

	printf("test 4: disarm, one-shot, CLOCK_REALTIME\n");
	CHECK(fd >= 0, "create CLOCK_REALTIME: %s", strerror(errno));
	CHECK(timerfd_settime(fd, 0, &spec, NULL) == 0, "settime");
	CHECK(wait_readable(fd, 500) > 0, "no first tick");
	read(fd, &exp, sizeof(exp));
	/* Let a tick pile up unread, then disarm without read()ing: settime()
	 * resets the count on Linux, so the fd must not stay readable. A
	 * caller that disarms from its read callback (osmo-trx) spins
	 * otherwise. */
	CHECK(wait_readable(fd, 500) > 0, "no second tick");
	CHECK(timerfd_settime(fd, 0, &zero, &old) == 0, "disarm");
	CHECK(old.it_interval.tv_nsec == 30000000, "old_value interval");
	CHECK(read(fd, &exp, sizeof(exp)) < 0 && errno == EAGAIN, "pending count survived settime");
	CHECK(wait_readable(fd, 150) == 0, "tick after disarm");
	CHECK(timerfd_gettime(fd, &old) == 0 && old.it_value.tv_sec == 0 && old.it_value.tv_nsec == 0,
	      "gettime after disarm not zero");

	spec.it_interval.tv_nsec = 0;	/* one-shot 30 ms */
	CHECK(timerfd_settime(fd, 0, &spec, NULL) == 0, "settime one-shot");
	CHECK(wait_readable(fd, 500) > 0, "one-shot did not fire");
	read(fd, &exp, sizeof(exp));
	CHECK(exp == 1, "one-shot count %llu", (unsigned long long)exp);
	CHECK(wait_readable(fd, 100) == 0, "one-shot fired twice");

	/* Rejected flags and clocks. */
	CHECK(timerfd_settime(fd, TFD_TIMER_CANCEL_ON_SET, &spec, NULL) < 0 && errno == EINVAL, "CANCEL_ON_SET accepted");
	CHECK(timerfd_create(7, 0) < 0 && errno == EINVAL, "bogus clockid accepted");
	close(fd);
}

/* Test 5: close() releases the timer; the fd number is not reused by a stale entry. */
static void test_close(void)
{
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	struct itimerspec spec = { { 0, 10000000 }, { 0, 10000000 } };
	struct itimerspec cur;
	int fd2, p[2];

	printf("test 5: close detection\n");
	CHECK(timerfd_settime(fd, 0, &spec, NULL) == 0, "settime");
	usleep(35000);
	close(fd);
	usleep(50000);	/* let the worker see the EOF */
	/* Take the same number with a plain pipe: it must not look like a timerfd. */
	pipe(p);
	printf("  closed fd %d, new pipe got %d/%d\n", fd, p[0], p[1]);
	CHECK(p[0] == fd, "fd number was not reused, test inconclusive");
	CHECK(timerfd_gettime(p[0], &cur) < 0 && errno == EINVAL, "stale entry still answers for fd %d", fd);
	close(p[0]);
	close(p[1]);
	/* And a new timerfd on the same number works. */
	fd2 = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	CHECK(fd2 == fd, "second create got %d, expected %d", fd2, fd);
	CHECK(timerfd_settime(fd2, 0, &spec, NULL) == 0, "settime on reused fd");
	CHECK(wait_readable(fd2, 500) > 0, "reused fd never ticks");
	close(fd2);
}

/* Test 6: fork. The child must keep receiving ticks (osmo_daemonize). */
static void test_fork(void)
{
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	struct itimerspec spec = { { 0, 100000000 }, { 0, 100000000 } };
	pid_t pid;
	int status;

	printf("test 6: fork\n");
	CHECK(timerfd_settime(fd, 0, &spec, NULL) == 0, "settime");
	usleep(20000);
	pid = fork();
	if (pid == 0) {
		struct timeval t0;
		int i, ok = 0;
		gettimeofday(&t0, NULL);
		for (i = 0; i < 3; i++) {
			uint64_t exp;
			if (wait_readable(fd, 1000) <= 0)
				_exit(10 + i);
			read(fd, &exp, sizeof(exp));
			printf("  child tick %d at %ld ms, expirations=%llu\n", i, ms_since(&t0), (unsigned long long)exp);
			ok++;
		}
		_exit(ok == 3 ? 0 : 20);
	}
	CHECK(pid > 0, "fork failed");
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit status %d", WEXITSTATUS(status));
	close(fd);
}

int main(void)
{
	test_periodic();
	test_first_then_interval();
	test_coalesce_and_gettime();
	test_disarm_and_oneshot();
	test_close();
	test_fork();
	printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
	return fails ? 1 : 0;
}
