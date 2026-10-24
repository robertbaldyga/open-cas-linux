/*
* Copyright(c) 2026 Unvertical
* SPDX-License-Identifier: BSD-3-Clause
*/

#include "cas_cache.h"
#include "vol_wbt.h"
#include <linux/swap.h>

extern u32 wbt_lat_usec;

#define CAS_WBT_WINDOW_MS	100
#define CAS_WBT_DEF_DEPTH	16
#define CAS_WBT_MAX_DEPTH	512
#define CAS_WBT_UNKNOWN_BUMP	5
#define CAS_WBT_CLOSE_IO	(HZ / 10)

struct cas_wbt_stat {
	atomic64_t min_lat;
	unsigned long reads;
	unsigned long writes;
};

/* Buffered writes; O_DIRECT writes are REQ_SYNC | REQ_IDLE */
static inline bool cas_wbt_bio_tracked(struct bio *bio)
{
	uint64_t flags = CAS_BIO_OP_FLAGS(bio);

	return bio_op(bio) == REQ_OP_WRITE && !(flags & REQ_PREFLUSH) &&
		(flags & (REQ_SYNC | REQ_IDLE)) != (REQ_SYNC | REQ_IDLE);
}

static inline u64 cas_wbt_target_ns(void)
{
	return (u64)READ_ONCE(wbt_lat_usec) * NSEC_PER_USEC;
}

static void cas_wbt_calc_limits(struct cas_wbt *wbt)
{
	unsigned int depth = CAS_WBT_DEF_DEPTH;

	wbt->scaled_max = false;

	if (wbt->scale_step > 0) {
		depth = 1 + ((depth - 1) >> min(31, wbt->scale_step));
	} else if (wbt->scale_step < 0) {
		depth = 1 + ((depth - 1) << min(16, -wbt->scale_step));
		if (depth >= CAS_WBT_MAX_DEPTH) {
			depth = CAS_WBT_MAX_DEPTH;
			wbt->scaled_max = true;
		}
	}

	WRITE_ONCE(wbt->max_depth, depth);
	WRITE_ONCE(wbt->wb_normal, (depth + 1) / 2);
	WRITE_ONCE(wbt->wb_background, (depth + 3) / 4);
}

static void cas_wbt_scale_up(struct cas_wbt *wbt)
{
	if (wbt->scaled_max)
		return;

	wbt->scale_step--;
	cas_wbt_calc_limits(wbt);
	wake_up_all(&wbt->wait);
}

static void cas_wbt_scale_down(struct cas_wbt *wbt)
{
	if (wbt->max_depth == 1)
		return;

	/* Drop any scale up gained while only writes were seen at once */
	if (wbt->scale_step < 0)
		wbt->scale_step = 0;
	else
		wbt->scale_step++;

	cas_wbt_calc_limits(wbt);
}

static void cas_wbt_arm(struct cas_wbt *wbt)
{
	if (!delayed_work_pending(&wbt->window_work)) {
		queue_delayed_work(wbt->wq, &wbt->window_work,
				msecs_to_jiffies(CAS_WBT_WINDOW_MS));
	}
}

static void cas_wbt_window_fn(struct work_struct *work)
{
	struct cas_wbt *wbt = container_of(to_delayed_work(work),
			struct cas_wbt, window_work);
	u64 target = cas_wbt_target_ns();
	u64 min_lat = U64_MAX, sync_lat = 0, sync_issue;
	unsigned long reads = 0, writes = 0, nr_reads, nr_writes;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cas_wbt_stat *stat = per_cpu_ptr(wbt->stat, cpu);

		min_lat = min_t(u64, min_lat,
				atomic64_xchg(&stat->min_lat, -1));
		reads += READ_ONCE(stat->reads);
		writes += READ_ONCE(stat->writes);
	}

	nr_reads = reads - wbt->prev_reads;
	nr_writes = writes - wbt->prev_writes;
	wbt->prev_reads = reads;
	wbt->prev_writes = writes;

	if (!target) {
		/* Disabled - go back to defaults and release the waiters */
		wbt->scale_step = 0;
		cas_wbt_calc_limits(wbt);
		wake_up_all(&wbt->wait);
		return;
	}

	sync_issue = atomic64_read(&wbt->sync_issue);
	if (sync_issue)
		sync_lat = ktime_get_ns() - sync_issue;

	/*
	 * When the exported object is flooded a read may not complete within
	 * the window at all, so count a read stuck for too long as a latency
	 * violation, even though no read completion was sampled.
	 */
	if (sync_lat > CAS_WBT_WINDOW_MS * NSEC_PER_MSEC ||
			(sync_lat > target && !nr_reads)) {
		wbt->unknown_cnt = 0;
		cas_wbt_scale_down(wbt);
	} else if (nr_reads) {
		wbt->unknown_cnt = 0;
		if (min_lat > target)
			cas_wbt_scale_down(wbt);
		else
			cas_wbt_scale_up(wbt);
	} else if (nr_writes) {
		/* Writes only - let them go faster */
		wbt->unknown_cnt = 0;
		cas_wbt_scale_up(wbt);
	} else if (++wbt->unknown_cnt >= CAS_WBT_UNKNOWN_BUMP) {
		/* No samples for a while - slowly return to the defaults */
		if (wbt->scale_step > 0) {
			cas_wbt_scale_up(wbt);
		} else if (wbt->scale_step < 0) {
			wbt->scale_step++;
			cas_wbt_calc_limits(wbt);
		}
	}

	if (wbt->scale_step || atomic_read(&wbt->inflight) || sync_issue)
		cas_wbt_arm(wbt);
}

static bool cas_wbt_inc_below(struct cas_wbt *wbt, unsigned int limit)
{
	int cur = atomic_read(&wbt->inflight);
	int old;

	for (;;) {
		if (cur >= (int)limit)
			return false;
		old = atomic_cmpxchg(&wbt->inflight, cur, cur + 1);
		if (old == cur)
			return true;
		cur = old;
	}
}

static unsigned int cas_wbt_limit(struct cas_wbt *wbt, struct bio *bio)
{
	uint64_t flags = CAS_BIO_OP_FLAGS(bio);

	if ((flags & (REQ_SYNC | REQ_FUA)) || current_is_kswapd())
		return READ_ONCE(wbt->max_depth);

	if ((flags & REQ_BACKGROUND) || time_before(jiffies,
			READ_ONCE(wbt->last_read) + CAS_WBT_CLOSE_IO)) {
		return READ_ONCE(wbt->wb_background);
	}

	return READ_ONCE(wbt->wb_normal);
}

static bool cas_wbt_may_wait(void)
{
	/* Waiting for completions might require memory to be reclaimed */
	if (current->flags & PF_MEMALLOC)
		return false;

	/*
	 * Bios queued on current->bio_list are not submitted until we return,
	 * and in-flight IO might depend on them (on kernels older than 4.11
	 * the list holds the lower level bios too).
	 */
	if (current->bio_list && !bio_list_empty(current->bio_list))
		return false;

	return true;
}

static void cas_wbt_wait(struct cas_wbt *wbt, struct bio *bio)
{
	DEFINE_WAIT(wait);

	if (cas_wbt_inc_below(wbt, cas_wbt_limit(wbt, bio)))
		return;

	if (!cas_wbt_may_wait()) {
		atomic_inc(&wbt->inflight);
		return;
	}

	for (;;) {
		prepare_to_wait(&wbt->wait, &wait, TASK_UNINTERRUPTIBLE);
		if (!READ_ONCE(wbt_lat_usec)) {
			atomic_inc(&wbt->inflight);
			break;
		}
		if (cas_wbt_inc_below(wbt, cas_wbt_limit(wbt, bio)))
			break;
		io_schedule();
	}
	finish_wait(&wbt->wait, &wait);
}

static void cas_wbt_touch_read(struct cas_wbt *wbt)
{
	unsigned long now = jiffies;

	/* Avoid dirtying the shared cacheline more than once per jiffy */
	if (READ_ONCE(wbt->last_read) != now)
		WRITE_ONCE(wbt->last_read, now);
}

unsigned int cas_wbt_throttle(struct cas_wbt *wbt, struct bio *bio,
		u64 *start_ns)
{
	unsigned int flags;

	if (!READ_ONCE(wbt_lat_usec))
		return 0;

	if (bio_op(bio) == REQ_OP_READ) {
		flags = CAS_WBT_READ;
		*start_ns = ktime_get_ns();
		cas_wbt_touch_read(wbt);
		if (!atomic64_read(&wbt->sync_issue) &&
				!atomic64_cmpxchg(&wbt->sync_issue, 0,
					*start_ns)) {
			flags |= CAS_WBT_READ_COOKIE;
		}
		return flags;
	}

	if (!cas_wbt_bio_tracked(bio))
		return 0;

	cas_wbt_wait(wbt, bio);
	cas_wbt_arm(wbt);

	return CAS_WBT_TRACKED;
}

void cas_wbt_done(struct cas_wbt *wbt, unsigned int flags, u64 start_ns)
{
	if (flags & CAS_WBT_READ) {
		atomic64_t *min_lat = &raw_cpu_ptr(wbt->stat)->min_lat;
		u64 lat = ktime_get_ns() - start_ns;
		s64 cur = atomic64_read(min_lat), old;

		while ((u64)cur > lat) {
			old = atomic64_cmpxchg(min_lat, cur, lat);
			if (old == cur)
				break;
			cur = old;
		}
		this_cpu_inc(wbt->stat->reads);
		cas_wbt_touch_read(wbt);

		if (flags & CAS_WBT_READ_COOKIE)
			atomic64_set(&wbt->sync_issue, 0);
	}

	if (flags & CAS_WBT_TRACKED) {
		int inflight = atomic_dec_return(&wbt->inflight);
		int limit = READ_ONCE(wbt->wb_normal);

		this_cpu_inc(wbt->stat->writes);

		/* Batch the wake ups, as blk-wbt does */
		if (inflight && inflight >= limit)
			return;

		if (wq_has_sleeper(&wbt->wait) && (!inflight ||
				limit - inflight >=
					READ_ONCE(wbt->wb_background) / 2)) {
			wake_up_all(&wbt->wait);
		}
	}
}

int cas_wbt_init(struct cas_wbt *wbt, struct workqueue_struct *wq)
{
	int cpu;

	wbt->stat = alloc_percpu(struct cas_wbt_stat);
	if (!wbt->stat)
		return -ENOMEM;

	for_each_possible_cpu(cpu)
		atomic64_set(&per_cpu_ptr(wbt->stat, cpu)->min_lat, -1);

	atomic_set(&wbt->inflight, 0);
	init_waitqueue_head(&wbt->wait);
	wbt->scale_step = 0;
	cas_wbt_calc_limits(wbt);
	wbt->unknown_cnt = 0;
	wbt->last_read = jiffies - CAS_WBT_CLOSE_IO - 1;
	atomic64_set(&wbt->sync_issue, 0);
	wbt->prev_reads = 0;
	wbt->prev_writes = 0;
	wbt->wq = wq;
	INIT_DELAYED_WORK(&wbt->window_work, cas_wbt_window_fn);

	return 0;
}

void cas_wbt_deinit(struct cas_wbt *wbt)
{
	cancel_delayed_work_sync(&wbt->window_work);
	free_percpu(wbt->stat);
	wbt->stat = NULL;
}
