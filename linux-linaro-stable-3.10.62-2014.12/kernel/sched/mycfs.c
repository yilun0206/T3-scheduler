/*
 * Author: Yilun Naman
 */

#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include "sched.h"

//#define DEBUG_MYCFS
#ifdef DEBUG_MYCFS
#define mycfs_debug(fmt, ...) 				\
	pr_info("%s() "fmt"\n",				\
			__func__, __VA_ARGS__)
#else
static inline void mycfs_debug(const char *fmt, ...) {  }
#endif /* DEBUG_MYCFS */

/* 
 * we are 32-bit arm machine
 * load->weight * load->inv_weight = WMULT_CONST(2^32)
 */
#define WMULT_CONST	(~0UL)
#define WMULT_SHIFT	32

#define __unused
#define MYCFS_PERIOD	100000ULL	/* 
					 * 100ms, units: usecs
					 */

static unsigned int sched_nr_latency = 8;
unsigned int mycfs_sched_latency = 10000000ULL;	/* 10ms, units: nanoseconds */

#define SRR(x, y) (((x) + (1UL << ((y) - 1))) >> (y))

/* per declare */
const struct sched_class mycfs_sched_class;

/*
 * update inverted weight if it become 0
 */
static inline void __update_inv_weight(struct load_weight *lw)
{
	unsigned long w;

	if (likely(lw->inv_weight))
		return;
	
	/* currently scale_load_down do nothing */
	w = scale_load_down(lw->weight);
	/* could only happen on 64-bit machine */
	if (BITS_PER_LONG > 32 && unlikely(w >= WMULT_CONST))
		lw->inv_weight = WMULT_CONST;
	else if (unlikely(!w))
		lw->inv_weight = WMULT_CONST;
	else
		lw->inv_weight = WMULT_CONST / w;
}

/*
 *
 */
static unsigned long
__calc_delta(unsigned long delta_exec, unsigned long weight,
	   struct load_weight *lw)
{
	u64 tmp;
	unsigned long retval;

	if (likely(weight > (1UL << SCHED_LOAD_RESOLUTION)))
		tmp = (u64)delta_exec * scale_load_down(weight);
	else
		tmp = (u64)delta_exec;
	
	__update_inv_weight(lw);

	/* 
	 * current tmp value would overflow
	 * unsinged long on 32-bit machine
	 */
	if (unlikely(tmp > WMULT_CONST))
		tmp = SRR(SRR(tmp, WMULT_SHIFT/2) * lw->inv_weight,
			WMULT_SHIFT/2);
	else
		tmp = SRR(tmp * lw->inv_weight, WMULT_SHIFT);

	retval = (unsigned long)min(tmp, (u64)(unsigned long)LONG_MAX);

	mycfs_debug("delta:%lu, delta_weighted:%lu", delta_exec, retval);
	return retval;
}

/*
 * getting task_struct of a given se
 */
static inline struct task_struct *task_of(struct sched_entity *se)
{
	/*
	 * Marco: container_of(ptr, type, member)
	 * first se is ptr, last se is se in task_struct
	 */
	return container_of(se, struct task_struct, se);
}

/*
 * getting generic runqueue from mycfs_rq
 */
static inline struct rq *rq_of(struct mycfs_rq *mycfs)
{
	return container_of(mycfs, struct rq, mycfs);
}

/*
 * getting mycfs_rq from a given se
 */
static inline struct mycfs_rq *mycfs_rq_of(struct sched_entity *se)
{
	struct task_struct *tsk = task_of(se);
	struct rq *rq = task_rq(tsk);

	return &rq->mycfs;
}

static inline struct mycfs_rq *task_mycfs_rq(struct task_struct *p)
{
	return &task_rq(p)->mycfs;
}

static inline u64 max_vruntime(u64 max_vruntime, u64 vruntime)
{
	s64 delta = (s64)(vruntime - max_vruntime);
	if (delta > 0)
		max_vruntime = vruntime;

	return max_vruntime;
}

static inline u64 min_vruntime(u64 min_vruntime, u64 vruntime)
{
	s64 delta = (s64)(vruntime - min_vruntime);
	if (delta < 0)
		min_vruntime = vruntime;

	return min_vruntime;
}

static inline int entity_before(struct sched_entity *a,
				struct sched_entity *b)
{
	return (s64)(a->vruntime - b->vruntime) < 0;
}

static inline void account_entity_enqueue(struct mycfs_rq *mycfs_rq,
				struct sched_entity __unused *se)
{
	mycfs_rq->nr_running++;
}

static inline void account_entity_dequeue(struct mycfs_rq *mycfs_rq,
				struct sched_entity __unused *se)
{
	mycfs_rq->nr_running--;
}

/* buddies ops */
static inline void set_last_buddy(struct sched_entity *se)
{
	mycfs_rq_of(se)->last = se;
}

static inline void set_next_buddy(struct sched_entity *se)
{
	mycfs_rq_of(se)->next = se;
}

static inline void set_skip_buddy(struct sched_entity *se)
{
	mycfs_rq_of(se)->skip = se;
}

static inline void clear_last_buddy(struct sched_entity *se)
{
	mycfs_rq_of(se)->last = NULL;
}

static inline void clear_next_buddy(struct sched_entity *se)
{
	mycfs_rq_of(se)->next = NULL;
}

static inline void clear_skip_buddy(struct sched_entity *se)
{
	mycfs_rq_of(se)->skip = NULL;
}

static void clear_buddies(struct mycfs_rq *mycfs_rq, struct sched_entity *se)
{
	/* only only when se is last, skip or next */
	if (mycfs_rq->next == se)
		clear_next_buddy(se);
	if (mycfs_rq->last == se)
		clear_last_buddy(se);
	if (mycfs_rq->skip == se)
		clear_last_buddy(se);
}

/* select container of leftmost node */
struct sched_entity *__pick_first_entity_mycfs(struct mycfs_rq *mycfs_rq)
{
	struct rb_node *leftmost = mycfs_rq->rb_leftmost;

	if (!leftmost)
		return NULL;
	
	return rb_entry(leftmost, struct sched_entity, run_node);
}

struct sched_entity *__pick_last_entity_mycfs(struct mycfs_rq *mycfs_rq)
{
	struct rb_node *last = rb_last(&mycfs_rq->tasks_timeline);

	if (!last)
		return NULL;
	
	return rb_entry(last, struct sched_entity, run_node);
}

struct sched_entity *__pick_next_entity_mycfs(struct sched_entity *se)
{
	struct rb_node *next = rb_next(&se->run_node);

	if (!next)
		return NULL;
	
	return rb_entry(next, struct sched_entity, run_node);
}

/* 
 * now is unweighted, thus delta /= weight
 */
static inline unsigned long
calc_delta_mycfs(unsigned long delta_exec, struct sched_entity *se)
{
	return delta_exec;
}

/*
 * The idea is to set a period in which each task runs once.
 *
 * When there are too many tasks (sched_nr_latency) we have to stretch
 * this period because otherwise the slices get too small.
 *
 * p = (nr <= nl) ? l : l*nr/nl
 */
static u64 __sched_period(unsigned long nr_running)
{
	if (unlikely(nr_running > sched_nr_latency))
		return nr_running * sysctl_sched_min_granularity;
	else
		return mycfs_sched_latency;
}

static u64 sched_slice(struct mycfs_rq *mycfs_rq, struct sched_entity *se)
{
	u64 oldslice, slice;
	unsigned long nr_running = mycfs_rq->nr_running;
	struct load_weight *load = &mycfs_rq->load;
	struct load_weight lw;

	BUG_ON(!se);
	if (unlikely(!se->on_rq))
		nr_running++;
	oldslice = __sched_period(nr_running);

	/* ?? */
	if (unlikely(!se->on_rq)) {
		lw = mycfs_rq->load;

		/* this func would make inv_weight to 0 */
		update_load_add(&lw, se->load.weight);
		load = &lw;
	}
	/* inv_weight will be updated here also */
	slice = __calc_delta(oldslice, se->load.weight, load);

	mycfs_debug("oldslice:%llu, slice:%llu, se->load.weight:%lu, mycfs->load.weight:%lu, load->weight:%lu",
			oldslice, slice, se->load.weight, mycfs_rq->load.weight, load->weight);

	return slice;
}

static u64 sched_vslice(struct mycfs_rq *mycfs_rq, struct sched_entity *se)
{
	return calc_delta_mycfs(sched_slice(mycfs_rq, se), se);
}

/* TODO: understand */
static void
place_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se, int initial)
{
	u64 vruntime = mycfs_rq->min_vruntime;

	/*
	 * The 'current' period is already promised to the current tasks,
	 * however the extra weight of the new task will slow them down a
	 * little, place the new task so that it fits in the slot that
	 * stays open at the end.
	 */
	if (initial && sched_feat(START_DEBIT))
		vruntime += sched_vslice(mycfs_rq, se);

	/* sleeps up to a single latency don't count. */
	if (!initial) {
		unsigned long thresh = mycfs_sched_latency;

		/*
		 * Halve their sleep time's effect, to allow
		 * for a gentler effect of sleepers:
		 */
		if (sched_feat(GENTLE_FAIR_SLEEPERS))
			thresh >>= 1;

		vruntime -= thresh;
	}

	/* ensure we never gain time by being placed backwards. */
	se->vruntime = max_vruntime(se->vruntime, vruntime);
}

/*
 * Trying to understand
 */
static void update_min_vruntime(struct mycfs_rq *mycfs)
{
	u64 vruntime = mycfs->min_vruntime;

	if (mycfs->curr)
		vruntime = mycfs->curr->vruntime;

	if (mycfs->rb_leftmost) {
		struct sched_entity *se = rb_entry(mycfs->rb_leftmost,
						   struct sched_entity,
						   run_node);

		if (!mycfs->curr)
			vruntime = se->vruntime;
		else
			vruntime = min_vruntime(vruntime, se->vruntime);
	}

	mycfs->min_vruntime = max_vruntime(mycfs->min_vruntime, vruntime);
}

static inline void
__update_curr(struct mycfs_rq *mycfs_rq, struct sched_entity *curr,
	      unsigned long delta_exec)
{
	unsigned long delta_exec_weighted;

	BUG_ON(!curr);
	schedstat_set(curr->statistics.exec_max,
			max((u64)delta_exec, curr->statistics.exec_max));
	
	curr->sum_exec_runtime += delta_exec;
	schedstat_add(mycfs_rq, exec_clock, delta_exec);
	delta_exec_weighted = calc_delta_mycfs(delta_exec, curr);

	curr->vruntime += delta_exec_weighted;
	update_min_vruntime(mycfs_rq);
}

/*
 * update following stats:
 * @mycfs_rq->min_vruntime
 * @mycfs_rq->exec_clock
 * @se->vruntime
 * @se->sum_exec_runtime
 * @se->exec_start
 * @se->statistics.exec_max
 */
static void update_curr(struct mycfs_rq *mycfs_rq)
{
	struct sched_entity *curr = mycfs_rq->curr;
	u64 now = rq_of(mycfs_rq)->clock_task;
	unsigned long delta_exec;

	if (unlikely(!curr))
		return;

	delta_exec = (unsigned long) (now - curr->exec_start);
	if (!delta_exec)
		return;

	__update_curr(mycfs_rq, curr, delta_exec);

	/* update curr->exec_start */
	curr->exec_start = now;
}

#if 0
static void update_curr_mycfs(struct rq *rq)
{
	return update_curr(&rq->mycfs);
}
#endif

static void __enqueue_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se)
{
	/* rb_node of rb-tree tree of runqueue */
	struct rb_node **link = &mycfs_rq->tasks_timeline.rb_node;
	struct rb_node *parent = NULL;
	int leftmost = 1;
	struct sched_entity *entry;

	mycfs_debug("pid: %d", task_of(se)->pid);
	/* search on rb-tree */
	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct sched_entity, run_node);

		if (entity_before(se, entry)) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	/* update leftmost rb_node if current ready-to inserted se become leftmost */
	if (leftmost)
		mycfs_rq->rb_leftmost = &se->run_node;
	
	rb_link_node(&se->run_node, parent, link);
	rb_insert_color(&se->run_node, &mycfs_rq->tasks_timeline);
}

static void __dequeue_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se)
{
	mycfs_debug("pid : %d", task_of(se)->pid);

	/* if se is leftmost node of rb-tree, update rb-tree leftmost node */
	if (mycfs_rq->rb_leftmost == &se->run_node) {
		struct rb_node *next;

		next = rb_next(&se->run_node);
		mycfs_rq->rb_leftmost = next;
	}

	rb_erase(&se->run_node, &mycfs_rq->tasks_timeline);
}

static void
enqueue_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se, int flags)
{
	/* 
	 * vruntime need to be normolized before calling
	 * update_curr(), seems only happen in group_scheduling, and we do not
	 * consider group scheduling, thus normally should not be called
	 */
	if (!(flags & ENQUEUE_WAKEUP) || (flags & ENQUEUE_WAKING)) {
		//WARN(1, "se->vruntime need to be normalized.\n");
		se->vruntime += mycfs_rq->min_vruntime;
	}
	
	update_curr(mycfs_rq);
	account_entity_enqueue(mycfs_rq, se);

	if (flags & ENQUEUE_WAKEUP)
		place_entity(mycfs_rq, se, 0);
	
	if (mycfs_rq->curr != se)
		__enqueue_entity(mycfs_rq, se);
	se->on_rq = 1;
}

static void
enqueue_task_mycfs(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_entity *se = &p->se;
	struct mycfs_rq *mycfs_rq = &rq->mycfs;

	enqueue_entity(mycfs_rq, se, flags);
	mycfs_rq->h_nr_running++;
	inc_nr_running(rq);
}

static void
dequeue_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se, int flags)
{
	update_curr(mycfs_rq);
	clear_buddies(mycfs_rq, se);

	/* ?? */
	if (se != mycfs_rq->curr)
		__dequeue_entity(mycfs_rq, se);
	se->on_rq = 0;
	account_entity_dequeue(mycfs_rq, se);

	/* normalize */
	if (!(flags & DEQUEUE_SLEEP))
		se->vruntime -= mycfs_rq->min_vruntime;
	
	update_min_vruntime(mycfs_rq);
}

static void
dequeue_task_mycfs(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_entity *se = &p->se;
	struct mycfs_rq *mycfs_rq = &rq->mycfs;

	dequeue_entity(mycfs_rq, se, flags);
	mycfs_rq->h_nr_running--;
	dec_nr_running(rq);
}

static unsigned long
wakeup_gran(struct sched_entity *curr, struct sched_entity *se)
{
	unsigned long gran = sysctl_sched_wakeup_granularity;

	/*
	 * Since its curr running now, convert the gran from real-time
	 * to virtual-time in his units.
	 *
	 * By using 'se' instead of 'curr' we penalize light tasks, so
	 * they get preempted easier. That is, if 'se' < 'curr' then
	 * the resulting gran will be larger, therefore penalizing the
	 * lighter, if otoh 'se' > 'curr' then the resulting gran will
	 * be smaller, again penalizing the lighter task.
	 *
	 * This is especially important for buddies when the leftmost
	 * task is higher priority than the buddy.
	 */
	return calc_delta_mycfs(gran, se);
}

/*
 * Should 'se' preempt 'curr'.
 *
 *             |s1
 *        |s2
 *   |s3
 *         g
 *      |<--->|c
 *
 *  w(c, s1) = -1
 *  w(c, s2) =  0
 *  w(c, s3) =  1
 *
 */
static int
wakeup_preempt_entity(struct sched_entity *curr, struct sched_entity *se)
{
	s64 gran, vdiff = curr->vruntime - se->vruntime;

	if (vdiff <= 0)
		return -1;

	gran = wakeup_gran(curr, se);
	if (vdiff > gran)
		return 1;

	return 0;
}

static struct sched_entity *pick_next_entity(struct mycfs_rq *mycfs_rq)
{
	/* TODO: implement */
	struct sched_entity *se = __pick_first_entity_mycfs(mycfs_rq);
	struct sched_entity *left = se;

	/* skip current se */
	if (mycfs_rq->skip == se) {
		struct sched_entity *second = __pick_next_entity_mycfs(se);
		/*
		 * we don't use !entity_before because we don't want
		 * second and left are too far away from each other
		 */
		if (second && wakeup_preempt_entity(second, left) < 1)
			se = second;
	}

	/* if last is not to far away (in vruntime) from left, pick last */
	if (mycfs_rq->last && wakeup_preempt_entity(mycfs_rq->last, left) < 1)
		se = mycfs_rq->last;

	/* likely to go here, if next is not NULL */
	if (mycfs_rq->next && wakeup_preempt_entity(mycfs_rq->next, left) < 1)
		se = mycfs_rq->next;

	clear_buddies(mycfs_rq, se);

	return se;
}

/* Do nothing */
static inline void update_load_avg(struct sched_entity *se, int flags)
{}

/*
 * updates current task start time 
 */
static inline void
update_stats_curr_start(struct mycfs_rq *mycfs_rq, struct sched_entity *se)
{
	se->exec_start = rq_of(mycfs_rq)->clock_task;
}

static void put_prev_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *prev)
{
	mycfs_debug("pid: %d, prev->on_rq: %d, prev: %p, curr: %p",
			task_of(prev)->pid, prev->on_rq, prev, mycfs_rq->curr);

	if (prev->on_rq) {
		update_curr(mycfs_rq);

		/* put the prev entity back into mycfs_rq */
		__enqueue_entity(mycfs_rq, prev);
	}
	/* if prev is not on rq */
	mycfs_rq->curr = NULL;
}

/* 
 * TODO: check what exactly it does
 */
static void
set_next_entity(struct mycfs_rq *mycfs_rq, struct sched_entity *se)
{
	if (se->on_rq) {
		__dequeue_entity(mycfs_rq, se);
	}

	update_stats_curr_start(mycfs_rq, se);
	mycfs_rq->curr = se;
	
	se->prev_sum_exec_runtime = se->sum_exec_runtime;
}

static struct task_struct *pick_next_task_mycfs(struct rq *rq)
{
	struct task_struct *p;
	struct mycfs_rq *mycfs_rq = &rq->mycfs;
	struct sched_entity *se;

	/* empty runqueue, return NULL */
	if (!mycfs_rq->nr_running)
		return NULL;

	se = pick_next_entity(mycfs_rq);
	set_next_entity(mycfs_rq, se);

	p = task_of(se);

	mycfs_debug("picked task->pid: %d, mycfs-rq->nr_running %d",
			p->pid, mycfs_rq->nr_running);

	return p;
}

static void put_prev_task_mycfs(struct rq *rq, struct task_struct *prev)
{
	struct mycfs_rq *mycfs_rq = &rq->mycfs;
	struct sched_entity *se = &prev->se;

	mycfs_debug("put task->pid: %d, mycfs-rq->nr_running %d",
			prev->pid, mycfs_rq->nr_running);

	put_prev_entity(mycfs_rq, se);
}

static void yield_task_mycfs(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct mycfs_rq *mycfs_rq = &task_rq(curr)->mycfs;
	struct sched_entity *se = &curr->se;

	/* cannot yield when I am the only on this CPU */
	if (unlikely(rq->nr_running == 1))
		return;

	clear_buddies(mycfs_rq, se);
	update_rq_clock(rq);
	update_curr(mycfs_rq);
	rq->skip_clock_update = 1;
	set_skip_buddy(se);
}

/*
 * yield_to task p
 */
static bool yield_to_task_mycfs(struct rq *rq, struct task_struct *p, bool preempt)
{
	struct sched_entity *se = &p->se;

	if (!se->on_rq)
		return false;
	
	set_next_buddy(se);

	yield_task_mycfs(rq);

	return true;
}

static void
check_preempt_wakeup_mycfs(struct rq *rq, struct task_struct *p, int wake_flags)
{
	struct task_struct *curr = rq->curr;
	struct sched_entity *se = &curr->se;	/* To be preempted */
	struct sched_entity *pse = &p->se;	/* whole preempt this CPU */
	
	/* */ 
	if (unlikely(se == pse))
		return;

	if (test_tsk_need_resched(curr))
		return;

	update_curr(&rq->mycfs);

	/* current's vruntime - p vruntime > gran */
	if (wakeup_preempt_entity(se, pse) == 1) {
		mycfs_debug("pid %d preempt current: %d",
				p->pid, curr->pid);
		/* 
		 * preempt current task
		 * set TIF_NEED_RESCHED of curr
		 */
		resched_task(curr);
	}
}

/* */
static void set_curr_task_mycfs(struct rq *rq)
{
	struct sched_entity *se = &rq->curr->se;
	struct mycfs_rq *mycfs_rq = &rq->mycfs;

	set_next_entity(mycfs_rq, se);

	mycfs_debug("pid: %d", rq->curr->pid);
}

/* */
static void
task_tick_mycfs(struct rq *rq, struct task_struct *curr, int queued)
{
	update_curr(&rq->mycfs);

	mycfs_debug("pid %d", curr->pid);
}

/*
 * child is not on the tasklist yet
 * preemption disabled
 *@p: parent process
 */
static void task_fork_mycfs(struct task_struct *p)
{
	struct mycfs_rq *mycfs_rq;
	struct sched_entity *se = &p->se, *curr;
	/* Actually we do not really care about SMP */
	int this_cpu = smp_processor_id();
	struct rq *rq = this_rq();
	unsigned long flags;

	/* 
	 * MARCO: flags will be assigned with _raw_spin_lock_irqsave(&rq->lock)
	 * spin_lock disable preemption
	 */
	raw_spin_lock_irqsave(&rq->lock, flags);

	WARN_ON(p->policy != SCHED_MYCFS);

	update_rq_clock(rq);

	mycfs_rq = task_mycfs_rq(current);
	curr = mycfs_rq->curr;

	/* not really useful for single core */
	rcu_read_lock();
	__set_task_cpu(p, this_cpu);
	rcu_read_unlock();

	update_curr(mycfs_rq);
	
	/* 
	 * new forked process vruntime should not start with 0,
	 * otherwise, it actually consume most of CPU time
	 */
	if (curr)
		se->vruntime = curr->vruntime;
	place_entity(mycfs_rq, se, 1);

	if (sysctl_sched_child_runs_first && curr && entity_before(curr, se)) {
		/*
		 * Upon rescheduling, sched_class::put_prev_task() will place
		 * 'current' within the tree based on its new key value.
		 */
		swap(curr->vruntime, se->vruntime);
		resched_task(rq->curr);
	}

	se->vruntime -= mycfs_rq->min_vruntime;
	
	mycfs_debug("task: %d\n", p->pid);

	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

/*
 * Priority of the task has changed. Check to see if we preempt
 * the current task.
 */
static void
prio_changed_mycfs(struct rq *rq, struct task_struct *p, int oldprio)
{
	mycfs_debug("pid: %d, oldprio: %d, currprio:%d",
			p->pid, p->prio, oldprio);

	if (!p->se.on_rq)
		return;

	if (rq->curr == p) {
		if (p->prio > oldprio)
			resched_task(rq->curr);
	} else
		check_preempt_curr(rq, p, 0);
}

/*
 * switch from mycfs to another sched_class
 */
static void switched_from_mycfs(struct rq *rq, struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct mycfs_rq *mycfs_rq = mycfs_rq_of(se);

	if (!p->on_rq && p->state != TASK_RUNNING) {
		place_entity(mycfs_rq, se, 0);
		se->vruntime -= mycfs_rq->min_vruntime;
	}
}

/*
 * switch from another sched_class to mycfs
 */
static void switched_to_mycfs(struct rq *rq, struct task_struct *p)
{
	if (!p->se.on_rq)
		return;

	if (rq->curr == p)
		resched_task(rq->curr);
	else
		check_preempt_curr(rq, p, 0);
}

/*
 * Do nothing
 */
static unsigned int get_rr_interval_mycfs(struct rq *rq, struct task_struct *p)
{
	return 0;
}

const struct sched_class mycfs_sched_class = {
	.next			= &idle_sched_class,
	.enqueue_task		= enqueue_task_mycfs,
	.dequeue_task		= dequeue_task_mycfs,
	.yield_task		= yield_task_mycfs,
	.yield_to_task		= yield_to_task_mycfs,

	.check_preempt_curr	= check_preempt_wakeup_mycfs,

	.pick_next_task		= pick_next_task_mycfs,
	.put_prev_task		= put_prev_task_mycfs,

	.set_curr_task          = set_curr_task_mycfs,
	.task_tick		= task_tick_mycfs,
	.task_fork		= task_fork_mycfs,

	.prio_changed		= prio_changed_mycfs,
	.switched_from		= switched_from_mycfs,
	.switched_to		= switched_to_mycfs,

	.get_rr_interval	= get_rr_interval_mycfs,
};

/* init mycfs rq */
void init_mycfs_rq(struct mycfs_rq *mycfs_rq)
{
	mycfs_rq->tasks_timeline = RB_ROOT;
	mycfs_rq->min_vruntime = 0;

	printk_once("init mycfs_rq.\n");
}
