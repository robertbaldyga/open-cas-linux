/*
* Copyright(c) 2026 Unvertical
* SPDX-License-Identifier: BSD-3-Clause
*/

#include "cas_cache.h"
#include "vol_wbt.h"
#include <linux/swap.h>

extern u32 wbt_lat_usec;
extern u32 wbt_core_feedback;
extern u32 wbt_core_sync_lat_usec;

#define CAS_WBT_WINDOW_MS	100
#define CAS_WBT_DEF_DEPTH	16
#define CAS_WBT_MAX_DEPTH	512
#define CAS_WBT_UNKNOWN_BUMP	5
#define CAS_WBT_CLOSE_IO	(HZ / 10)

struct cas_wbt_stat {
	atomic64_t min_lat;
	unsigned long reads;
	unsigned long writes;
	atomic64_t core_min_read;
	atomic64_t core_min_sync;
	unsigned long core_reads;
	unsigned long core_syncs;
};

/* Verdict of one class of samples over a window, ordered by precedence */
enum cas_wbt_lat_status {
	CAS_WBT_LAT_UNKNOWN,
	CAS_WBT_LAT_OK,
	CAS_WBT_LAT_EXCEEDED,
};

static void cas_wbt_update_min(atomic64_t *min_lat, u64 lat)
{
	s64 cur = atomic64_read(min_lat), old;

	while ((u64)cur > lat) {
		old = atomic64_cmpxchg(min_lat, cur, lat);
		if (old == cur)
			break;
		cur = old;
	}
}

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

static inline u64 cas_wbt_sync_target_ns(void)
{
	return (u64)READ_ONCE(wbt_core_sync_lat_usec) * NSEC_PER_USEC;
}

/*
 * When flooded, a request may not complete within the window at all, so one
 * stuck for the whole window, or past the target with no completion sampled,
 * counts as a violation too.
 */
static enum cas_wbt_lat_status cas_wbt_lat_status(u64 min_lat,
		unsigned long nr, atomic64_t *issue, u64 target, u64 now)
{
	u64 issued = atomic64_read(issue);
	u64 stuck = issued ? now - issued : 0;

	if (stuck > CAS_WBT_WINDOW_MS * NSEC_PER_MSEC ||
			(stuck > target && !nr)) {
		return CAS_WBT_LAT_EXCEEDED;
	}

	if (!nr)
		return CAS_WBT_LAT_UNKNOWN;

	return min_lat > target ? CAS_WBT_LAT_EXCEEDED : CAS_WBT_LAT_OK;
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
	u64 sync_target = cas_wbt_sync_target_ns();
	u64 min_lat = U64_MAX, core_read = U64_MAX, core_sync = U64_MAX;
	unsigned long reads = 0, writes = 0, core_reads = 0, core_syncs = 0;
	unsigned long nr_reads, nr_writes, nr_core_reads, nr_core_syncs;
	enum cas_wbt_lat_status status;
	bool pending;
	u64 now;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cas_wbt_stat *stat = per_cpu_ptr(wbt->stat, cpu);

		min_lat = min_t(u64, min_lat,
				atomic64_xchg(&stat->min_lat, -1));
		core_read = min_t(u64, core_read,
				atomic64_xchg(&stat->core_min_read, -1));
		core_sync = min_t(u64, core_sync,
				atomic64_xchg(&stat->core_min_sync, -1));
		reads += READ_ONCE(stat->reads);
		writes += READ_ONCE(stat->writes);
		core_reads += READ_ONCE(stat->core_reads);
		core_syncs += READ_ONCE(stat->core_syncs);
	}

	nr_reads = reads - wbt->prev_reads;
	nr_writes = writes - wbt->prev_writes;
	nr_core_reads = core_reads - wbt->prev_core_reads;
	nr_core_syncs = core_syncs - wbt->prev_core_syncs;
	wbt->prev_reads = reads;
	wbt->prev_writes = writes;
	wbt->prev_core_reads = core_reads;
	wbt->prev_core_syncs = core_syncs;

	if (!target) {
		/* Disabled - go back to defaults and release the waiters */
		wbt->scale_step = 0;
		cas_wbt_calc_limits(wbt);
		wake_up_all(&wbt->wait);
		return;
	}

	now = ktime_get_ns();

	if (READ_ONCE(wbt_core_feedback)) {
		/*
		 * Judge the core instead of the exported object, where cache
		 * hits hide the queue: core reads against wbt_lat_usec, core
		 * sync writes against wbt_core_sync_lat_usec (0 - they don't
		 * steer). Either class over its target is a violation; a class
		 * without samples doesn't vote.
		 */
		status = cas_wbt_lat_status(core_read, nr_core_reads,
				&wbt->core_read_issue, target, now);
		if (sync_target) {
			status = max(status, cas_wbt_lat_status(core_sync,
					nr_core_syncs, &wbt->core_sync_issue,
					sync_target, now));
		}
		pending = atomic64_read(&wbt->core_read_issue) ||
			atomic64_read(&wbt->core_sync_issue);
	} else {
		status = cas_wbt_lat_status(min_lat, nr_reads,
				&wbt->sync_issue, target, now);
		pending = atomic64_read(&wbt->sync_issue);
	}

	switch (status) {
	case CAS_WBT_LAT_EXCEEDED:
		wbt->unknown_cnt = 0;
		cas_wbt_scale_down(wbt);
		break;
	case CAS_WBT_LAT_OK:
		wbt->unknown_cnt = 0;
		cas_wbt_scale_up(wbt);
		break;
	case CAS_WBT_LAT_UNKNOWN:
		if (nr_writes) {
			/* Writes only - let them go faster */
			wbt->unknown_cnt = 0;
			cas_wbt_scale_up(wbt);
		} else if (++wbt->unknown_cnt >= CAS_WBT_UNKNOWN_BUMP) {
			/* No samples for a while - slowly return to defaults */
			if (wbt->scale_step > 0) {
				cas_wbt_scale_up(wbt);
			} else if (wbt->scale_step < 0) {
				wbt->scale_step++;
				cas_wbt_calc_limits(wbt);
			}
		}
		break;
	}

	if (wbt->scale_step || atomic_read(&wbt->inflight) || pending)
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
		cas_wbt_update_min(&raw_cpu_ptr(wbt->stat)->min_lat,
				ktime_get_ns() - start_ns);
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

u64 cas_wbt_core_issue(struct cas_wbt *wbt, int dir, uint64_t flags)
{
	atomic64_t *issue;
	u64 now;

	if (!READ_ONCE(wbt_lat_usec) || !READ_ONCE(wbt_core_feedback))
		return 0;

	/* Throttled (non-sync) writes cause the queueing, they are no signal */
	if (dir == OCF_READ)
		issue = &wbt->core_read_issue;
	else if ((flags & (REQ_SYNC | REQ_FUA)) &&
			READ_ONCE(wbt_core_sync_lat_usec))
		issue = &wbt->core_sync_issue;
	else
		return 0;

	now = ktime_get_ns();

	/* Detects core io stuck for the whole window, see window_fn */
	if (!atomic64_read(issue))
		atomic64_cmpxchg(issue, 0, now);

	return now;
}

void cas_wbt_core_done(struct cas_wbt *wbt, struct bio *bio, u64 start_ns)
{
	struct cas_wbt_stat *stat = raw_cpu_ptr(wbt->stat);
	u64 lat = ktime_get_ns() - start_ns;

	/* Only the io that set an issue time clears it; later bios no-op */
	if (bio_op(bio) == REQ_OP_READ) {
		cas_wbt_update_min(&stat->core_min_read, lat);
		this_cpu_inc(wbt->stat->core_reads);
		atomic64_cmpxchg(&wbt->core_read_issue, start_ns, 0);
	} else {
		cas_wbt_update_min(&stat->core_min_sync, lat);
		this_cpu_inc(wbt->stat->core_syncs);
		atomic64_cmpxchg(&wbt->core_sync_issue, start_ns, 0);
	}
}

int cas_wbt_init(struct cas_wbt *wbt, struct workqueue_struct *wq)
{
	int cpu;

	wbt->stat = alloc_percpu(struct cas_wbt_stat);
	if (!wbt->stat)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct cas_wbt_stat *stat = per_cpu_ptr(wbt->stat, cpu);

		atomic64_set(&stat->min_lat, -1);
		atomic64_set(&stat->core_min_read, -1);
		atomic64_set(&stat->core_min_sync, -1);
	}

	atomic_set(&wbt->inflight, 0);
	init_waitqueue_head(&wbt->wait);
	wbt->scale_step = 0;
	cas_wbt_calc_limits(wbt);
	wbt->unknown_cnt = 0;
	wbt->last_read = jiffies - CAS_WBT_CLOSE_IO - 1;
	atomic64_set(&wbt->sync_issue, 0);
	atomic64_set(&wbt->core_read_issue, 0);
	atomic64_set(&wbt->core_sync_issue, 0);
	wbt->prev_reads = 0;
	wbt->prev_writes = 0;
	wbt->prev_core_reads = 0;
	wbt->prev_core_syncs = 0;
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
