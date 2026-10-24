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

	struct cas_wbt_stat __percpu *stat;

	unsigned long prev_reads;
	unsigned long prev_writes;
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

#endif /* __VOL_WBT_H__ */
