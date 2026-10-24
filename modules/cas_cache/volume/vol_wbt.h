/*
* Copyright(c) 2026 Unvertical
* SPDX-License-Identifier: BSD-3-Clause
*/

#ifndef __VOL_WBT_H__
#define __VOL_WBT_H__

#include <linux/workqueue.h>
#include <linux/wait.h>
#include "../linux_kernel_version.h"

/*
 * Writeback throttling for exported objects.
 *
 * Bio-based devices don't get the block layer's rq_qos (wbt) nor tag based
 * backpressure, so page cache writeback can flood OCF with an unbounded
 * number of writes and starve reads. This limits the number of in-flight
 * buffered writes per exported object, putting the submitter to sleep when
 * the limit is reached, and scales the limit based on the observed read
 * latency, the same way blk-wbt does it.
 */

struct cas_wbt_stat;

struct cas_wbt {
	atomic_t inflight;
		/*!< Number of in-flight tracked writes */

	wait_queue_head_t wait;
		/*!< Submitters waiting for the in-flight count to drop */

	unsigned int max_depth;
	unsigned int wb_normal;
	unsigned int wb_background;
		/*!< Current limits for sync, regular and background writes */

	int scale_step;
		/*!< Negative scales the depth up, positive scales it down */

	bool scaled_max;
		/*!< Depth can't be scaled up any further */

	unsigned int unknown_cnt;
		/*!< Windows in a row without any samples */

	unsigned long last_read;
		/*!< Jiffies of the last read issue or completion */

	atomic64_t sync_issue;
		/*!< Issue time of a single in-flight read, zero if none */

	atomic64_t core_read_issue;
	atomic64_t core_sync_issue;
		/*!< Issue time of a single in-flight sampled core read / core
		 * sync write, zero if none (wbt_core_feedback)
		 */

	struct cas_wbt_stat __percpu *stat;

	unsigned long prev_reads;
	unsigned long prev_writes;
	unsigned long prev_core_reads;
	unsigned long prev_core_syncs;
		/*!< Totals seen at the end of the previous window */

	struct workqueue_struct *wq;
	struct delayed_work window_work;
};

#define CAS_WBT_TRACKED	(1 << 0)
#define CAS_WBT_READ	(1 << 1)
#define CAS_WBT_READ_COOKIE	(1 << 2)

int cas_wbt_init(struct cas_wbt *wbt, struct workqueue_struct *wq);

void cas_wbt_deinit(struct cas_wbt *wbt);

/**
 * @brief Account a data bio before it is submitted to OCF, waiting for
 *	in-flight writes to drop below the limit if necessary
 *
 * @param start_ns returns the timestamp to be passed to cas_wbt_done()
 *
 * @return flags to be passed to cas_wbt_done() on bio completion
 */
unsigned int cas_wbt_throttle(struct cas_wbt *wbt, struct bio *bio,
		u64 *start_ns);

void cas_wbt_done(struct cas_wbt *wbt, unsigned int flags, u64 start_ns);

/**
 * @brief Account an io forwarded to the core device (wbt_core_feedback)
 *
 * Two classes are sampled, the foreground io queued behind the writeback:
 * core reads (cache misses), judged against wbt_lat_usec, and core sync
 * writes (e.g. a database WAL), judged against wbt_core_sync_lat_usec. They
 * have their own targets because their latency floors differ by an order of
 * magnitude (a SATA SSD: ~250 us reads, ~2 ms sync writes).
 *
 * @param dir OCF_READ or OCF_WRITE
 * @param flags flags of the forwarded io
 *
 * @return issue timestamp to be passed to cas_wbt_core_done() for each bio
 *	of the io, zero if the io is not sampled
 */
u64 cas_wbt_core_issue(struct cas_wbt *wbt, int dir, uint64_t flags);

void cas_wbt_core_done(struct cas_wbt *wbt, struct bio *bio, u64 start_ns);

#endif /* __VOL_WBT_H__ */
