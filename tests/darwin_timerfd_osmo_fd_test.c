/*
 * libosmocore-macos-arm64: the timerfd emulation through the libosmocore
 * path the daemons use: osmo_timerfd_setup(), osmo_timerfd_schedule() and
 * osmo_select_main(). Schedules a 1 us first expiration followed by 500 ms
 * intervals, reschedules once right away as osmo_stats_set_interval() does
 * after osmo_stats_init(), and expects six callbacks in 2.6 s.
 */

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/select.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/timer.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

static struct log_info_cat cats[] = { { .name = "DMAIN", .enabled = 1, .loglevel = LOGL_NOTICE } };
static const struct log_info log_info = { .cat = cats, .num_cat = 1 };
static struct osmo_fd tfd = { .fd = -1 };
static struct timespec t0;
static int ticks;

static long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000; }

static int cb(struct osmo_fd *ofd, unsigned int what)
{
	uint64_t n = 0;
	int rc = read(ofd->fd, &n, sizeof(n));
	printf("cb at %ld ms what=%u read=%d count=%llu\n", ms(), what, rc, (unsigned long long)n);
	ticks++;
	return 0;
}

int main(void)
{
	void *ctx = talloc_named_const(NULL, 0, "t");
	struct timespec first = { 0, 1000 }, interval = { 0, 500000000 };
	int rc;
	osmo_init_logging2(ctx, &log_info);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = osmo_timerfd_setup(&tfd, cb, NULL);
	printf("setup rc=%d fd=%d\n", rc, tfd.fd);
	rc = osmo_timerfd_schedule(&tfd, &first, &interval);
	printf("schedule rc=%d\n", rc);
	/* reschedule right away, as osmo_stats_set_interval() does after osmo_stats_init() */
	rc = osmo_timerfd_schedule(&tfd, &first, &interval);
	printf("reschedule rc=%d\n", rc);
	while (ms() < 2600)
		osmo_select_main(1), usleep(1000);
	printf("ticks=%d in 2.6 s (expected 1 + 5)\n", ticks);
	return 0;
}
