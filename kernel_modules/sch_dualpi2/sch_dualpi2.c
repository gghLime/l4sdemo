// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2019 Nokia.
 *
 * Author: Koen De Schepper <koen.de_schepper@nokia-bell-labs.com>
 * Author: Olga Albisser <olga@albisser.org>
 * Author: Henrik Steen <henrist@henrist.net>
 * Author: Olivier Tilmans <olivier.tilmans@nokia-bell-labs.com>
 *
 * DualPI Improved with a Square (dualpi2):
 *   Supports scalable congestion controls (e.g., DCTCP)
 *   Supports coupled dual-queue with PI2
 *   Supports L4S ECN identifier
 *
 * References:
 *   draft-ietf-tsvwg-aqm-dualq-coupled:
 *     http://tools.ietf.org/html/draft-ietf-tsvwg-aqm-dualq-coupled-08
 *   De Schepper, Koen, et al. "PI 2: A linearized AQM for both classic and
 *   scalable TCP."  in proc. ACM CoNEXT'16, 2016.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <net/pkt_sched.h>
#include <net/inet_ecn.h>
#include <linux/string.h>

#include "compat-linux.h"

/* 32b enable to support flows with windows up to ~8.6 * 1e9 packets
 * i.e., twice the maximal snd_cwnd.
 * MAX_PROB must be consistent with the RNG in dualpi2_roll().
 */
#define MAX_PROB ((u32)(~((u32)0)))
/* Apha/beta are in units of 64ns to enable to use their full range of values */
#define ALPHA_BETA_GRANULARITY 6
/* alpha/beta values exchanged over netlink */
#define ALPHA_BETA_SHIFT (8 - ALPHA_BETA_GRANULARITY)
#define ALPHA_BETA_MAX (40000 << ALPHA_BETA_SHIFT)
/* We express the weights (wc, wl) in %, i.e., wc + wl = 100 */
#define MAX_WC 100

struct dualpi2_sched_data {
	struct Qdisc *l_queue;	/* The L4S LL queue */
	struct Qdisc *sch;	/* The classic queue (owner of this struct) */

	struct { /* PI2 parameters */
		u64	target;	/* Target delay in nanoseconds */
		u32	tupdate;/* timer frequency (in jiffies) */
		u32	prob;	/* Base PI2 probability */
		u32	alpha;	/* Gain factor for the integral rate response */
		u32	beta;	/* Gain factor for the proportional response */
		struct timer_list timer; /* prob update timer */
	} pi2;

	struct { /* Step AQM (L4S queue only) parameters */
		u32 thresh;	/* Step threshold */
		bool in_packets;/* Whether the step is in packets or time */
	} step;

	struct { /* Classic queue starvation protection */
		s32	credit; /* Credit (sign indicates which queue) */
		s32	init;	/* Reset value of the credit */
		u8	wc;	/* C queue weight (between 0 and MAX_WC) */
		u8	wl;	/* L queue weight (MAX_WC - wc) */
	} c_protection;

	/* General dualQ parameters */
	u8	coupling_factor;/* Coupling factor (k) between both queues */
	u8	ecn_mask;	/* Mask to match L4S packets */
	bool	drop_early;	/* Drop at enqueue instead of dequeue if true */
	bool	drop_overload;	/* Drop (1) on overload, or overflow (0) */


	/* Statistics */
	u64	qdelay_c;	/* Classic Q delay */
	u64	qdelay_l;	/* L4S Q delay */
	u32	packets_in_c;	/* Number of packets enqueued in C queue */
	u32	packets_in_l;	/* Number of packets enqueued in L queue */
	u32	maxq;		/* maximum queue size */
	u32	ecn_mark;	/* packets marked with ECN */
	u32	step_marks;	/* ECN marks due to the step AQM */
#ifdef IS_TESTBED
	struct testbed_metrics testbed;
#endif

	struct { /* Deferred drop statistics */
		u32 cnt;	/* Packets dropped */
		u32 len;	/* Bytes dropped */
	} deferred_drops;
};

struct dualpi2_skb_cb {
	u64 ts;		/* Timestamp at enqueue */
	u8 apply_step:1,/* Can we apply the step threshold (if l4444*/
	   l4s:1,	/* Packet has been classified as L4S */
	   ect:2;	/* Packet ECT codepoint */
};

static inline struct dualpi2_skb_cb *dualpi2_skb_cb(struct sk_buff *skb)
{
	qdisc_cb_private_validate(skb, sizeof(struct dualpi2_skb_cb));
	return (struct dualpi2_skb_cb *)qdisc_skb_cb(skb)->data;
}

static inline u64 skb_sojourn_time(struct sk_buff *skb, u64 reference)
{
        return reference - dualpi2_skb_cb(skb)->ts;
}

static inline u64 qdelay_in_ns(struct Qdisc *q, u64 now)
{
	struct sk_buff *skb = qdisc_peek_head(q);
	return skb ? skb_sojourn_time(skb, now) : 0;
}

static inline u32 dualpi2_scale_alpha_beta(u32 param)
{
	return ((u64)param * MAX_PROB >> ALPHA_BETA_SHIFT) / NSEC_PER_SEC;
}

 static inline u32 dualpi2_unscale_alpha_beta(u32 param)
{
	return ((u64)param * NSEC_PER_SEC << ALPHA_BETA_SHIFT) / MAX_PROB;
}

static inline bool skb_is_l4s(struct sk_buff *skb)
{
	return dualpi2_skb_cb(skb)->l4s != 0;
}

static inline void dualpi2_mark(struct dualpi2_sched_data *q,
				struct sk_buff *skb)
{
	if (INET_ECN_set_ce(skb))
		q->ecn_mark++;
}

static inline void dualpi2_reset_c_protection(struct dualpi2_sched_data *q)
{
	q->c_protection.credit = q->c_protection.init;
}

static inline void dualpi2_calculate_c_protection(struct Qdisc *sch,
						  struct dualpi2_sched_data *q,
						  u32 wc)
{
	q->c_protection.wc = wc;
	q->c_protection.wl = MAX_WC - wc;
	/* Start with L queue if wl > wc */
	q->c_protection.init = (s32)psched_mtu(qdisc_dev(sch)) *
		((int)q->c_protection.wc - (int)q->c_protection.wl);
	dualpi2_reset_c_protection(q);
}

static inline bool dualpi2_roll(u32 prob)
{
	return prandom_u32() <= prob;
}

static inline bool dualpi2_squared_roll(struct dualpi2_sched_data *q)
{
	return dualpi2_roll(q->pi2.prob) && dualpi2_roll(q->pi2.prob);
}

static inline bool dualpi2_is_overloaded(u64 prob)
{
	return prob > MAX_PROB;
}

static bool must_drop(struct Qdisc *sch, struct dualpi2_sched_data *q,
		      struct sk_buff *skb)
{
	u64 local_l_prob;

	/* Never drop if we have fewer than 2 mtu-sized packets;
	 * similar to min_th in RED.
	 */
	if (sch->qstats.backlog < 2 * psched_mtu(qdisc_dev(sch)))
		return false;

	local_l_prob = (u64)q->pi2.prob * q->coupling_factor;

	if (skb_is_l4s(skb)) {
		if (dualpi2_is_overloaded(local_l_prob)) {
			/* On overload, preserve delay by doing a classic drop
			 * in the L queue. Otherwise, let both queues grow until
			 * we reach the limit and cannot enqueue anymore
			 * (sacrifice delay to avoid drops).
			 */
			if (q->drop_overload && dualpi2_squared_roll(q))
				goto drop;
			else
				goto mark;
		/* Apply scalable marking with a (prob * k) probability. */
		} else if (dualpi2_roll(local_l_prob)) {
			goto mark;
		}
	/* Apply classic marking with a (prob * prob) probability.
	 * Force drops for ECN-capable traffic on overload.
	 */
	} else if (dualpi2_squared_roll(q)) {
		if (dualpi2_skb_cb(skb)->ect &&
		    !dualpi2_is_overloaded(local_l_prob))
			goto mark;
		else
			goto drop;
	}
	return false;

mark:
	dualpi2_mark(q, skb);
	return false;

drop:
#ifdef IS_TESTBED
	testbed_inc_drop_count(&q->testbed, skb);
#endif
	return true;
}

static void dualpi2_skb_classify(struct dualpi2_sched_data *q,
				 struct sk_buff *skb)
{
	struct dualpi2_skb_cb *cb = dualpi2_skb_cb(skb);
	int wlen = skb_network_offset(skb);

	switch (tc_skb_protocol(skb)) {
	case htons(ETH_P_IP):
		wlen += sizeof(struct iphdr);
		if (!pskb_may_pull(skb, wlen) ||
		    skb_try_make_writable(skb, wlen))
			goto not_ecn;

		cb->ect = ipv4_get_dsfield(ip_hdr(skb)) & INET_ECN_MASK;
		break;
	case htons(ETH_P_IPV6):
		wlen += sizeof(struct ipv6hdr);
		if (!pskb_may_pull(skb, wlen) ||
		    skb_try_make_writable(skb, wlen))
			goto not_ecn;

		cb->ect = ipv6_get_dsfield(ipv6_hdr(skb)) & INET_ECN_MASK;
		break;
	default:
		goto not_ecn;
	}
	cb->l4s = (cb->ect & q->ecn_mask) != 0;
	return;

not_ecn:
	/* Not ECN capable or not non pullable/writable packets can only be
	 * dropped hence go the the classic queue.
	 */
	cb->ect = INET_ECN_NOT_ECT;
	cb->l4s = 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
static int dualpi2_qdisc_enqueue(struct sk_buff *skb, struct Qdisc *sch)
#else
static int dualpi2_qdisc_enqueue(struct sk_buff *skb, struct Qdisc *sch,
				 struct sk_buff **to_free)
#endif
{
	struct dualpi2_sched_data *q = qdisc_priv(sch);
	int err;

	if (unlikely(qdisc_qlen(sch) >= sch->limit)) {
		qdisc_qstats_overlimit(sch);
		err = NET_XMIT_DROP;
#ifdef IS_TESTBED
		testbed_inc_drop_count(&q->testbed, skb);
#endif
		goto drop;
	}

	dualpi2_skb_classify(q, skb);

	/* drop early if configured */
	if (q->drop_early && must_drop(sch, q, skb)) {
		err = NET_XMIT_SUCCESS | __NET_XMIT_BYPASS;
		goto drop;
	}

        dualpi2_skb_cb(skb)->ts = ktime_get_ns();

	if (qdisc_qlen(sch) > q->maxq)
		q->maxq = qdisc_qlen(sch);

	if (skb_is_l4s(skb)) {
		dualpi2_skb_cb(skb)->apply_step = qdisc_qlen(q->l_queue) > 0;
		/* Keep the overall qdisc stats consistent */
		++sch->q.qlen;
		qdisc_qstats_backlog_inc(sch, skb);
		++q->packets_in_l;
		return qdisc_enqueue_tail(skb, q->l_queue);
	}
	++q->packets_in_c;
	return qdisc_enqueue_tail(skb, sch);

drop:
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
	qdisc_drop(skb, sch);
#else
	qdisc_drop(skb, sch, to_free);
#endif
	return err;
}

static struct sk_buff *dualpi2_qdisc_dequeue(struct Qdisc *sch)
{
	struct dualpi2_sched_data *q = qdisc_priv(sch);
	struct sk_buff *skb;
	int qlen_c, credit_change;

pick_packet:
	/* L queue packets are also accounted for in qdisc_qlen(sch)! */
	qlen_c = qdisc_qlen(sch) - qdisc_qlen(q->l_queue);
	skb = NULL;
	/* We can drop after qdisc_dequeue_head() calls.
	 * Manage statistics by hand to keep them consistent if that happens.
	 * */
	if (qdisc_qlen(q->l_queue) > 0 &&
	    (qlen_c <= 0 || q->c_protection.credit <= 0)) {
		/* Dequeue and increase the credit by wc if qlen_c != 0 */
		skb = __qdisc_dequeue_head(&q->l_queue->q);
		credit_change = qlen_c ?
			q->c_protection.wc * qdisc_pkt_len(skb) : 0;
		/* The global backlog will be updated later. */
		qdisc_qstats_backlog_dec(q->l_queue, skb);
		/* Propagate the dequeue to the global stats. */
		--sch->q.qlen;
	} else if (qlen_c > 0) {
		/* Dequeue and decrease the credit by wl if qlen_l != 0 */
		skb = __qdisc_dequeue_head(&sch->q);
		credit_change = qdisc_qlen(q->l_queue) ?
			(s32)(-1) * q->c_protection.wl * qdisc_pkt_len(skb) : 0;
	} else {
		dualpi2_reset_c_protection(q);
		goto exit;
	}
	qdisc_qstats_backlog_dec(sch, skb);

	/* Drop on dequeue? */
	if (!q->drop_early && must_drop(sch, q, skb)) {
		++q->deferred_drops.cnt;
		q->deferred_drops.len += qdisc_pkt_len(skb);
		consume_skb(skb);
		qdisc_qstats_drop(sch);
		/* try next packet */
		goto pick_packet;
	}

	/* Apply the Step AQM to packets coming out of the L queue. */
	if (skb_is_l4s(skb)) {
		u64 qdelay = 0;

		if (q->step.in_packets)
			qdelay = qdisc_qlen(q->l_queue);
		else
			/* Only apply time-based if the packet causes a queue.
			 * Convert in us.
			 */
			qdelay = skb_sojourn_time(skb, ktime_get_ns())
				/ NSEC_PER_USEC;
		/* Apply the step */
		if (likely(dualpi2_skb_cb(skb)->apply_step) &&
		    qdelay > q->step.thresh) {
			dualpi2_mark(q, skb);
			++q->step_marks;
		}
		qdisc_bstats_update(q->l_queue, skb);
	}

	q->c_protection.credit += credit_change;
	qdisc_bstats_update(sch, skb);
#ifdef IS_TESTBED
	testbed_add_metrics(skb, &q->testbed,
			    skb_sojourn_time(skb, ktime_get_ns()) >> 10);
#endif

exit:
	/* We cannot call qdisc_tree_reduce_backlog() if our qlen is 0,
	 * or HTB crashes.
	 */
	if (q->deferred_drops.cnt && qdisc_qlen(sch)) {
		qdisc_tree_reduce_backlog(sch, q->deferred_drops.cnt,
					  q->deferred_drops.len);
		q->deferred_drops.cnt = 0;
		q->deferred_drops.len = 0;
	}
	return skb;
}

static u32 calculate_probability(struct Qdisc *sch)
{
	struct dualpi2_sched_data *q = qdisc_priv(sch);
	u64 qdelay, qdelay_old, now;
	u32 new_prob;
	s64 delta;

	qdelay_old = max_t(u64, q->qdelay_c, q->qdelay_l);
	now = ktime_get_ns();
	q->qdelay_l = qdelay_in_ns(q->l_queue, now);
	q->qdelay_c = qdelay_in_ns(sch, now);
	qdelay = max_t(u64, q->qdelay_c, q->qdelay_l);
	/* Alpha and beta take at most 31b. This leaves 33b for delay difference
	 * computations,
	 * i.e, would overflow for queueing delay differences > ~8sec.
	 */
	delta = ((s64)((s64)qdelay - q->pi2.target) * q->pi2.alpha)
		/ ((1 << (ALPHA_BETA_GRANULARITY + 1)) - 1);
	delta += ((s64)((s64)qdelay - qdelay_old) * q->pi2.beta)
		/ ((1 << (ALPHA_BETA_GRANULARITY + 1)) - 1);
	new_prob = (s64)delta + q->pi2.prob;
	/* Prevent overflow */
	if (delta > 0) {
		if (new_prob < q->pi2.prob)
			new_prob = MAX_PROB;
	/* Prevent underflow */
	} else if (new_prob > q->pi2.prob)
		new_prob = 0;
	/* If we do not drop on overload, ensure we cap the L4S probability to
	 * 100% to keep window fairness when overflowing.
	 */
	if (!q->drop_overload)
		return min_t(u32, new_prob, MAX_PROB / q->coupling_factor);
	return new_prob;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
static void dualpi2_timer(unsigned long arg)
{
	struct Qdisc *sch = (struct Qdisc *)arg;
	struct dualpi2_sched_data *q = qdisc_priv(sch);
#else
static void dualpi2_timer(struct timer_list *timer)
{
        struct dualpi2_sched_data *q = from_timer(q, timer, pi2.timer);
	struct Qdisc *sch = q->sch;
#endif
	spinlock_t *root_lock; /* spinlock for qdisc parameter update */

	root_lock = qdisc_lock(qdisc_root_sleeping(sch));
	spin_lock(root_lock);

	q->pi2.prob = calculate_probability(sch);
	mod_timer(&q->pi2.timer, jiffies + q->pi2.tupdate);

	spin_unlock(root_lock);
}

static const struct nla_policy dualpi2_policy[TCA_DUALPI2_MAX + 1] = {
	[TCA_DUALPI2_LIMIT] = {.type = NLA_U32},
	[TCA_DUALPI2_TARGET] = {.type = NLA_U32},
	[TCA_DUALPI2_TUPDATE] = {.type = NLA_U32},
	[TCA_DUALPI2_ALPHA] = {.type = NLA_U32},
	[TCA_DUALPI2_BETA] = {.type = NLA_U32},
	[TCA_DUALPI2_STEP_THRESH] = {.type = NLA_U32},
	[TCA_DUALPI2_STEP_PACKETS] = {.type = NLA_U8},
	[TCA_DUALPI2_COUPLING] = {.type = NLA_U8},
	[TCA_DUALPI2_DROP_OVERLOAD] = {.type = NLA_U8},
	[TCA_DUALPI2_DROP_EARLY] = {.type = NLA_U8},
	[TCA_DUALPI2_C_PROTECTION] = {.type = NLA_U8},
	[TCA_DUALPI2_ECN_MASK] = {.type = NLA_U8},
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
static int dualpi2_change(struct Qdisc *sch, struct nlattr *opt)
#else
static int dualpi2_change(struct Qdisc *sch, struct nlattr *opt,
			  struct netlink_ext_ack *extack)
#endif
{
	struct dualpi2_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_DUALPI2_MAX + 1];
	unsigned int old_qlen, dropped = 0;
	int err;

	if (!opt)
		return -EINVAL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
	err = nla_parse_nested(tb, TCA_DUALPI2_MAX, opt, dualpi2_policy);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
	err = nla_parse_nested(tb, TCA_DUALPI2_MAX, opt, dualpi2_policy, NULL);
#else
	err = nla_parse_nested(tb, TCA_DUALPI2_MAX, opt, dualpi2_policy,
			       extack);
#endif
	if (err < 0)
		return err;

	sch_tree_lock(sch);

	if (tb[TCA_DUALPI2_LIMIT]) {
		u32 limit = nla_get_u32(tb[TCA_DUALPI2_LIMIT]);

		if (!limit) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
			NL_SET_ERR_MSG_ATTR(extack, tb[TCA_DUALPI2_LIMIT],
				    "limit must be greater than 0 !");
#endif
			return -EINVAL;
		}
		sch->limit = limit;
	}

	if (tb[TCA_DUALPI2_TARGET])
		q->pi2.target = (u64)nla_get_u32(tb[TCA_DUALPI2_TARGET]) *
			NSEC_PER_USEC;

	if (tb[TCA_DUALPI2_TUPDATE]) {
		u64 tupdate =
			usecs_to_jiffies(nla_get_u32(tb[TCA_DUALPI2_TUPDATE]));

		if (!tupdate) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
			NL_SET_ERR_MSG_ATTR(extack, tb[TCA_DUALPI2_TUPDATE],
				    "tupdate cannot be 0 jiffies!");
#endif
			return -EINVAL;
		}
		q->pi2.tupdate = tupdate;
	}

	if (tb[TCA_DUALPI2_ALPHA]) {
		u32 alpha = nla_get_u32(tb[TCA_DUALPI2_ALPHA]);

		if (alpha > ALPHA_BETA_MAX) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
			NL_SET_ERR_MSG_ATTR(extack, tb[TCA_DUALPI2_ALPHA],
					    "alpha is too large!");
#endif
			return -EINVAL;
		}
		q->pi2.alpha = dualpi2_scale_alpha_beta(alpha);
	}

	if (tb[TCA_DUALPI2_BETA]) {
		u32 beta = nla_get_u32(tb[TCA_DUALPI2_BETA]);

		if (beta > ALPHA_BETA_MAX) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
			NL_SET_ERR_MSG_ATTR(extack, tb[TCA_DUALPI2_BETA],
					    "beta is too large!");
#endif
			return -EINVAL;

		}
		q->pi2.beta = dualpi2_scale_alpha_beta(beta);
	}

	if (tb[TCA_DUALPI2_STEP_THRESH])
		q->step.thresh = nla_get_u32(tb[TCA_DUALPI2_STEP_THRESH]);

	if (tb[TCA_DUALPI2_COUPLING])
		q->coupling_factor = nla_get_u8(tb[TCA_DUALPI2_COUPLING]);

	if (tb[TCA_DUALPI2_STEP_PACKETS])
                q->step.in_packets = nla_get_u8(tb[TCA_DUALPI2_STEP_PACKETS]);

	if (tb[TCA_DUALPI2_DROP_OVERLOAD])
		q->drop_overload = nla_get_u8(tb[TCA_DUALPI2_DROP_OVERLOAD]);

	if (tb[TCA_DUALPI2_DROP_EARLY])
		q->drop_early = nla_get_u8(tb[TCA_DUALPI2_DROP_EARLY]);

	if (tb[TCA_DUALPI2_C_PROTECTION]) {
		u8 wc = nla_get_u8(tb[TCA_DUALPI2_C_PROTECTION]);

		if (wc > MAX_WC) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
			NL_SET_ERR_MSG_ATTR(extack,
					    tb[TCA_DUALPI2_C_PROTECTION],
					    "c_protection must be <= 100!");
#endif
			return -EINVAL;
		}
		dualpi2_calculate_c_protection(sch, q, wc);
	}

	if (tb[TCA_DUALPI2_ECN_MASK])
		q->ecn_mask = nla_get_u8(tb[TCA_DUALPI2_ECN_MASK]);

	/* Drop excess packets if new limit is lower */
	old_qlen = qdisc_qlen(sch);
	while (qdisc_qlen(sch) > sch->limit) {
		struct sk_buff *skb = __qdisc_dequeue_head(&sch->q);
		dropped += qdisc_pkt_len(skb);
		qdisc_qstats_backlog_dec(sch, skb);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0)
		qdisc_drop(skb, sch);
#else
		rtnl_qdisc_drop(skb, sch);
#endif
	}
	qdisc_tree_reduce_backlog(sch, old_qlen - qdisc_qlen(sch), dropped);

	sch_tree_unlock(sch);
	return 0;
}

static void dualpi2_reset_default(struct dualpi2_sched_data *q)
{
	q->sch->limit = 10000; /* Holds 125ms at 1G */

	q->pi2.target = 15 * NSEC_PER_MSEC; /* 15 ms */
	q->pi2.tupdate = usecs_to_jiffies(16 * USEC_PER_MSEC); /* 16 ms */
	q->pi2.alpha = dualpi2_scale_alpha_beta(41); /* ~0.16 Hz */
	q->pi2.beta = dualpi2_scale_alpha_beta(819); /* ~3.2 Hz */
	/* These values give a 10dB stability margin with max_rtt=100ms */

	q->step.thresh = 1 * USEC_PER_MSEC; /* 1ms */
	q->step.in_packets = false; /* Step in time not packets */

	dualpi2_calculate_c_protection(q->sch, q, 10); /* Defaults to wc = 10 */

	q->ecn_mask = INET_ECN_ECT_1; /* l4s-id */
	q->coupling_factor = 2; /* window fairness for equal RTTs */
	q->drop_overload = true; /* Preserve latency by dropping on overload */
	q->drop_early = false; /* PI2 drop on dequeue */
#ifdef IS_TESTBED
	testbed_metrics_init(&q->testbed);
#endif
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
static int dualpi2_init(struct Qdisc *sch, struct nlattr *opt)
#else
static int dualpi2_init(struct Qdisc *sch, struct nlattr *opt,
			struct netlink_ext_ack *extack)
#endif
{
	struct dualpi2_sched_data *q = qdisc_priv(sch);
	int err;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
	if (!(q->l_queue = qdisc_create_dflt(sch->dev_queue, &pfifo_qdisc_ops,
					     TC_H_MAKE(sch->handle, 1))))
#else
	if (!(q->l_queue = qdisc_create_dflt(sch->dev_queue, &pfifo_qdisc_ops,
	 				     TC_H_MAKE(sch->handle, 1), extack)))
#endif
		return -ENOMEM;

	q->sch = sch;
	dualpi2_reset_default(q);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
	setup_timer(&q->pi2.timer, dualpi2_timer, (unsigned long)sch);
#else
        timer_setup(&q->pi2.timer, dualpi2_timer, 0);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
	if (opt && (err = dualpi2_change(sch, opt)))
#else
	if (opt && (err = dualpi2_change(sch, opt, extack)))
#endif
		return err;

	mod_timer(&q->pi2.timer, jiffies + HZ / 2);
	return 0;
}

static int dualpi2_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct nlattr *opts = nla_nest_start(skb, TCA_OPTIONS);
	struct dualpi2_sched_data *q = qdisc_priv(sch);
	u64 target_usec = q->pi2.target;

	if (!opts)
		goto nla_put_failure;

        do_div(target_usec, NSEC_PER_USEC);

	if (nla_put_u32(skb, TCA_DUALPI2_LIMIT, sch->limit) ||
	    nla_put_u32(skb, TCA_DUALPI2_TARGET, target_usec) ||
	    nla_put_u32(skb, TCA_DUALPI2_TUPDATE,
			jiffies_to_usecs(q->pi2.tupdate)) ||
	    nla_put_u32(skb, TCA_DUALPI2_ALPHA,
			dualpi2_unscale_alpha_beta(q->pi2.alpha)) ||
	    nla_put_u32(skb, TCA_DUALPI2_BETA,
			dualpi2_unscale_alpha_beta(q->pi2.beta)) ||
	    nla_put_u32(skb, TCA_DUALPI2_STEP_THRESH, q->step.thresh) ||
	    nla_put_u8(skb, TCA_DUALPI2_COUPLING, q->coupling_factor) ||
	    nla_put_u8(skb, TCA_DUALPI2_DROP_OVERLOAD, q->drop_overload) ||
	    nla_put_u8(skb, TCA_DUALPI2_STEP_PACKETS, q->step.in_packets) ||
	    nla_put_u8(skb, TCA_DUALPI2_DROP_EARLY, q->drop_early) ||
	    nla_put_u8(skb, TCA_DUALPI2_C_PROTECTION, q->c_protection.wc) ||
	    nla_put_u8(skb, TCA_DUALPI2_ECN_MASK, q->ecn_mask))
		goto nla_put_failure;

	return nla_nest_end(skb, opts);

nla_put_failure:
	nla_nest_cancel(skb, opts);
	return -1;
}

static int dualpi2_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	struct dualpi2_sched_data *q = qdisc_priv(sch);
	u64 qdelay_c_usec = q->qdelay_c;
        u64 qdelay_l_usec = q->qdelay_l;
	struct tc_dualpi2_xstats st = {
		.prob		= q->pi2.prob,
		.packets_in_c	= q->packets_in_c,
		.packets_in_l	= q->packets_in_l,
		.maxq		= q->maxq,
		.ecn_mark	= q->ecn_mark,
		.credit		= q->c_protection.credit,
		.step_marks	= q->step_marks,
	};

        do_div(qdelay_c_usec, NSEC_PER_USEC);
        do_div(qdelay_l_usec, NSEC_PER_USEC);
	st.delay_c = qdelay_c_usec;
	st.delay_l = qdelay_l_usec;
	return gnet_stats_copy_app(d, &st, sizeof(st));
}

static void dualpi2_reset(struct Qdisc *sch)
{
	struct dualpi2_sched_data *q = qdisc_priv(sch);

	qdisc_reset_queue(sch);
	qdisc_reset_queue(q->l_queue);
	q->qdelay_c = 0;
	q->qdelay_l = 0;
	q->pi2.prob = 0;
	q->packets_in_c = 0;
	q->packets_in_l = 0;
	q->maxq = 0;
	q->ecn_mark = 0;
	q->step_marks = 0;
	dualpi2_reset_c_protection(q);
}

static void dualpi2_destroy(struct Qdisc *sch)
{
	struct dualpi2_sched_data *q = qdisc_priv(sch);

	q->pi2.tupdate = 0;
	del_timer_sync(&q->pi2.timer);
	if (q->l_queue)
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 20, 0)
		qdisc_destroy(q->l_queue);
#else
		qdisc_put(q->l_queue);
#endif
}

static struct Qdisc_ops dualpi2_qdisc_ops __read_mostly = {
	.id = "dualpi2",
	.priv_size	= sizeof(struct dualpi2_sched_data),
	.enqueue	= dualpi2_qdisc_enqueue,
	.dequeue	= dualpi2_qdisc_dequeue,
	.peek		= qdisc_peek_dequeued,
	.init		= dualpi2_init,
	.destroy	= dualpi2_destroy,
	.reset		= dualpi2_reset,
	.change		= dualpi2_change,
	.dump		= dualpi2_dump,
	.dump_stats	= dualpi2_dump_stats,
	.owner		= THIS_MODULE,
};

static int __init dualpi2_module_init(void)
{
	return register_qdisc(&dualpi2_qdisc_ops);
}

static void __exit dualpi2_module_exit(void)
{
	unregister_qdisc(&dualpi2_qdisc_ops);
}

module_init(dualpi2_module_init);
module_exit(dualpi2_module_exit);

MODULE_DESCRIPTION("Dual Queue with Proportional Integral controller "
		   "Improved with a Square (dualpi2) scheduler");
MODULE_AUTHOR("Koen De Schepper");
MODULE_AUTHOR("Olga Albisser");
MODULE_AUTHOR("Henrik Steen");
MODULE_AUTHOR("Olivier Tilmans");
MODULE_LICENSE("GPL");
