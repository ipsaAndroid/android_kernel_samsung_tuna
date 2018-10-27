/* kernel/time/sched_debug.c
 *
 * Print the CFS rbtree
 *
 * Copyright(C) 2007, Red Hat, Inc., Ingo Molnar
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/fs.h>			/* for sprintf */
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/kallsyms.h>
#include <linux/utsname.h>

#define RQE_Q_CAP 35000
#define RQ_DEBUG 1
#define BLCK_SIZE 8192
#define BLK_PAD 200

static char logEntry[BLCK_SIZE];
static int firstWrite = 1;
static char debugFailed[9];
static unsigned int failedWrites=0;
static DEFINE_SPINLOCK(sched_debug_lock);

static int rtnStatus=1;
static int stopIndex=0;



/*
 * This allows printing both to /proc/sched_debug and
 * to the console
 */
#define SEQ_printf(m, x...)			\
 do {						\
	if (m)					\
		seq_printf(m, x);		\
	else					\
		printk(x);			\
 } while (0)

/*
 * Ease the printing of nsec fields:
 */
static long long nsec_high(unsigned long long nsec)
{
	if ((long long)nsec < 0) {
		nsec = -nsec;
		do_div(nsec, 1000000);
		return -nsec;
	}
	do_div(nsec, 1000000);

	return nsec;
}

static unsigned long nsec_low(unsigned long long nsec)
{
	if ((long long)nsec < 0)
		nsec = -nsec;

	return do_div(nsec, 1000000);
}

#define SPLIT_NS(x) nsec_high(x), nsec_low(x)

#ifdef CONFIG_FAIR_GROUP_SCHED
static void print_cfs_group_stats(struct seq_file *m, int cpu, struct task_group *tg)
{
	struct sched_entity *se = tg->se[cpu];
	if (!se)
		return;

#define P(F) \
	SEQ_printf(m, "  .%-30s: %lld\n", #F, (long long)F)
#define PN(F) \
	SEQ_printf(m, "  .%-30s: %lld.%06ld\n", #F, SPLIT_NS((long long)F))

	PN(se->exec_start);
	PN(se->vruntime);
	PN(se->sum_exec_runtime);
#ifdef CONFIG_SCHEDSTATS
	PN(se->statistics.wait_start);
	PN(se->statistics.sleep_start);
	PN(se->statistics.block_start);
	PN(se->statistics.sleep_max);
	PN(se->statistics.block_max);
	PN(se->statistics.exec_max);
	PN(se->statistics.slice_max);
	PN(se->statistics.wait_max);
	PN(se->statistics.wait_sum);
	P(se->statistics.wait_count);
#endif
	P(se->load.weight);
#undef PN
#undef P
}
#endif

#ifdef CONFIG_CGROUP_SCHED
static char group_path[PATH_MAX];

static char *task_group_path(struct task_group *tg)
{
	if (autogroup_path(tg, group_path, PATH_MAX))
		return group_path;

	/*
	 * May be NULL if the underlying cgroup isn't fully-created yet
	 */
	if (!tg->css.cgroup) {
		group_path[0] = '\0';
		return group_path;
	}
	cgroup_path(tg->css.cgroup, group_path, PATH_MAX);
	return group_path;
}
#endif

static void
print_task(struct seq_file *m, struct rq *rq, struct task_struct *p)
{
	if (rq->curr == p)
		SEQ_printf(m, "R");
	else
		SEQ_printf(m, " ");

	SEQ_printf(m, "%15s %5d %9Ld.%06ld %9Ld %5d ",
		p->comm, p->pid,
		SPLIT_NS(p->se.vruntime),
		(long long)(p->nvcsw + p->nivcsw),
		p->prio);
#ifdef CONFIG_SCHEDSTATS
	SEQ_printf(m, "%9Ld.%06ld %9Ld.%06ld %9Ld.%06ld",
		SPLIT_NS(p->se.vruntime),
		SPLIT_NS(p->se.sum_exec_runtime),
		SPLIT_NS(p->se.statistics.sum_sleep_runtime));
#else
	SEQ_printf(m, "%15Ld %15Ld %15Ld.%06ld %15Ld.%06ld %15Ld.%06ld",
		0LL, 0LL, 0LL, 0L, 0LL, 0L, 0LL, 0L);
#endif
#ifdef CONFIG_CGROUP_SCHED
	SEQ_printf(m, " %s", task_group_path(task_group(p)));
#endif

	SEQ_printf(m, "\n");
}

static void print_rq(struct seq_file *m, struct rq *rq, int rq_cpu)
{
	struct task_struct *g, *p;
	unsigned long flags;

	SEQ_printf(m,
	"\nrunnable tasks:\n"
	"            task   PID         tree-key  switches  prio"
	"     exec-runtime         sum-exec        sum-sleep\n"
	"------------------------------------------------------"
	"----------------------------------------------------\n");

	read_lock_irqsave(&tasklist_lock, flags);

	do_each_thread(g, p) {
		if (!p->on_rq || task_cpu(p) != rq_cpu)
			continue;

		print_task(m, rq, p);
	} while_each_thread(g, p);

	read_unlock_irqrestore(&tasklist_lock, flags);
}

void print_cfs_rq(struct seq_file *m, int cpu, struct cfs_rq *cfs_rq)
{
	s64 MIN_vruntime = -1, min_vruntime, max_vruntime = -1,
		spread, rq0_min_vruntime, spread0;
	struct rq *rq = cpu_rq(cpu);
	struct sched_entity *last;
	unsigned long flags;

#ifdef CONFIG_FAIR_GROUP_SCHED
	SEQ_printf(m, "\ncfs_rq[%d]:%s\n", cpu, task_group_path(cfs_rq->tg));
#else
	SEQ_printf(m, "\ncfs_rq[%d]:\n", cpu);
#endif
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "exec_clock",
			SPLIT_NS(cfs_rq->exec_clock));

	raw_spin_lock_irqsave(&rq->lock, flags);
	if (cfs_rq->rb_leftmost)
		MIN_vruntime = (__pick_first_entity(cfs_rq))->vruntime;
	last = __pick_last_entity(cfs_rq);
	if (last)
		max_vruntime = last->vruntime;
	min_vruntime = cfs_rq->min_vruntime;
	rq0_min_vruntime = cpu_rq(0)->cfs.min_vruntime;
	raw_spin_unlock_irqrestore(&rq->lock, flags);
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "MIN_vruntime",
			SPLIT_NS(MIN_vruntime));
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "min_vruntime",
			SPLIT_NS(min_vruntime));
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "max_vruntime",
			SPLIT_NS(max_vruntime));
	spread = max_vruntime - MIN_vruntime;
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "spread",
			SPLIT_NS(spread));
	spread0 = min_vruntime - rq0_min_vruntime;
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "spread0",
			SPLIT_NS(spread0));
	SEQ_printf(m, "  .%-30s: %d\n", "nr_spread_over",
			cfs_rq->nr_spread_over);
	SEQ_printf(m, "  .%-30s: %ld\n", "nr_running", cfs_rq->nr_running);
	SEQ_printf(m, "  .%-30s: %ld\n", "load", cfs_rq->load.weight);
#ifdef CONFIG_FAIR_GROUP_SCHED
#ifdef CONFIG_SMP
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "load_avg",
			SPLIT_NS(cfs_rq->load_avg));
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", "load_period",
			SPLIT_NS(cfs_rq->load_period));
	SEQ_printf(m, "  .%-30s: %ld\n", "load_contrib",
			cfs_rq->load_contribution);
	SEQ_printf(m, "  .%-30s: %d\n", "load_tg",
			atomic_read(&cfs_rq->tg->load_weight));
#endif

	print_cfs_group_stats(m, cpu, cfs_rq->tg);
#endif
}

void print_rt_rq(struct seq_file *m, int cpu, struct rt_rq *rt_rq)
{
#ifdef CONFIG_RT_GROUP_SCHED
	SEQ_printf(m, "\nrt_rq[%d]:%s\n", cpu, task_group_path(rt_rq->tg));
#else
	SEQ_printf(m, "\nrt_rq[%d]:\n", cpu);
#endif

#define P(x) \
	SEQ_printf(m, "  .%-30s: %Ld\n", #x, (long long)(rt_rq->x))
#define PN(x) \
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", #x, SPLIT_NS(rt_rq->x))

	P(rt_nr_running);
	P(rt_throttled);
	PN(rt_time);
	PN(rt_runtime);

#undef PN
#undef P
}

extern __read_mostly int sched_clock_running;

static void print_cpu(struct seq_file *m, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

#ifdef CONFIG_X86
	{
		unsigned int freq = cpu_khz ? : 1;

		SEQ_printf(m, "\ncpu#%d, %u.%03u MHz\n",
			   cpu, freq / 1000, (freq % 1000));
	}
#else
	SEQ_printf(m, "\ncpu#%d\n", cpu);
#endif

#define P(x) \
	SEQ_printf(m, "  .%-30s: %Ld\n", #x, (long long)(rq->x))
#define PN(x) \
	SEQ_printf(m, "  .%-30s: %Ld.%06ld\n", #x, SPLIT_NS(rq->x))

	P(nr_running);
	SEQ_printf(m, "  .%-30s: %lu\n", "load",
		   rq->load.weight);
	P(nr_switches);
	P(nr_load_updates);
	P(nr_uninterruptible);
	PN(next_balance);
	P(curr->pid);
	PN(clock);
	P(cpu_load[0]);
	P(cpu_load[1]);
	P(cpu_load[2]);
	P(cpu_load[3]);
	P(cpu_load[4]);
#undef P
#undef PN

#ifdef CONFIG_SCHEDSTATS
#define P(n) SEQ_printf(m, "  .%-30s: %d\n", #n, rq->n);
#define P64(n) SEQ_printf(m, "  .%-30s: %Ld\n", #n, rq->n);

	P(yld_count);

	P(sched_switch);
	P(sched_count);
	P(sched_goidle);
#ifdef CONFIG_SMP
	P64(avg_idle);
#endif

	P(ttwu_count);
	P(ttwu_local);

#undef P
#undef P64
#endif
	spin_lock_irqsave(&sched_debug_lock, flags);
	print_cfs_stats(m, cpu);
	print_rt_stats(m, cpu);

	rcu_read_lock();
	print_rq(m, rq, cpu);
	rcu_read_unlock();
	spin_unlock_irqrestore(&sched_debug_lock, flags);
}

static const char *sched_tunable_scaling_names[] = {
	"none",
	"logaritmic",
	"linear"
};

static int sched_debug_show(struct seq_file *m, void *v)
{
	u64 ktime, sched_clk, cpu_clk;
	unsigned long flags;
	int cpu;

	local_irq_save(flags);
	ktime = ktime_to_ns(ktime_get());
	sched_clk = sched_clock();
	cpu_clk = local_clock();
	local_irq_restore(flags);

	SEQ_printf(m, "Sched Debug Version: v0.10, %s %.*s\n",
		init_utsname()->release,
		(int)strcspn(init_utsname()->version, " "),
		init_utsname()->version);

#define P(x) \
	SEQ_printf(m, "%-40s: %Ld\n", #x, (long long)(x))
#define PN(x) \
	SEQ_printf(m, "%-40s: %Ld.%06ld\n", #x, SPLIT_NS(x))
	PN(ktime);
	PN(sched_clk);
	PN(cpu_clk);
	P(jiffies);
#ifdef CONFIG_HAVE_UNSTABLE_SCHED_CLOCK
	P(sched_clock_stable);
#endif
#undef PN
#undef P

	SEQ_printf(m, "\n");
	SEQ_printf(m, "sysctl_sched\n");

#define P(x) \
	SEQ_printf(m, "  .%-40s: %Ld\n", #x, (long long)(x))
#define PN(x) \
	SEQ_printf(m, "  .%-40s: %Ld.%06ld\n", #x, SPLIT_NS(x))
	PN(sysctl_sched_latency);
	PN(sysctl_sched_min_granularity);
	PN(sysctl_sched_wakeup_granularity);
	P(sysctl_sched_child_runs_first);
	P(sysctl_sched_features);
#undef PN
#undef P

	SEQ_printf(m, "  .%-40s: %d (%s)\n", "sysctl_sched_tunable_scaling",
		sysctl_sched_tunable_scaling,
		sched_tunable_scaling_names[sysctl_sched_tunable_scaling]);

	for_each_online_cpu(cpu)
		print_cpu(m, cpu);

	SEQ_printf(m, "\n");

	return 0;
}

static int rqRESET(struct seq_file *m, void *v)
{
  struct rq *rq = cpu_rq(0);
  
  SEQ_printf(m,"---- RESET ----\n");
  rq->rqe_Queue.rqe_insertIdx = 0;
  rq->rqe_Queue.rqe_deleteIdx = 0;
  rq->rqe_Queue.rqe_seqNum = 0;
  rq->rqe_Queue.rqe_population = 0;
  rq->rqe_Queue.rqe_initialized = 1;
  rq->rqe_Queue.rqe_system_thread_calls = 0;
  rq->rqe_Queue.rqe_logging_threads_calls = 0;
  rq->rqe_Queue.rqe_queue_kworker_start_time = -1;
  rq->rqe_Queue.rqe_queue_kworker_sum_total = 0;
  rq->rqe_Queue.rqe_kworker_thread = -1;
  
  failedWrites = 0;
  return 0;
}

//For only Rq0, add more cases for more run cores
static int rqe_Get(struct seq_file *m, void *v)
{
  struct rq *rq = cpu_rq(0);
  if (rq->rqe_Queue.rqe_deleteIdx == stopIndex){
    return NULL;
  }
  int logEntryLen=0,
    lenWritten=0,
    first_seqNum = rq->rqe_Queue.buffer[rq->rqe_Queue.rqe_deleteIdx].seqNum,
    last_seqNum,
    first_qIndx = rq->rqe_Queue.rqe_deleteIdx, last_qIndx;
   
  if (firstWrite) {
    char buf[100];
    int nChars = sprintf(buf, "yy1: start=%d, end=%d\n", first_seqNum, last_seqNum);
    seq_write(m, "yy1\n", 4);
    firstWrite = 0;
  }

  int i=sprintf(&debugFailed,"yy2:%d %d sys %d sum sys time %d log\n",failedWrites, rq->rqe_Queue.rqe_system_thread_calls,rq->rqe_Queue.rqe_queue_kworker_sum_total,rq->rqe_Queue.rqe_logging_threads_calls);
  seq_write(m,debugFailed,i);
  //While memory remains for the proc entry keep filling buffer
  while ((logEntryLen <= (BLCK_SIZE - BLK_PAD)) && (rq->rqe_Queue.rqe_deleteIdx != stopIndex)) {
    int target=stopIndex-rq->rqe_Queue.rqe_deleteIdx; 
    // print single entry into log
    logEntryLen +=
      sprintf(&logEntry[logEntryLen], "%lld,%s,%ld,%c,%Ld.%06ld,%Ld.%06ld,%ld,%llu\n",
	      rq->rqe_Queue.buffer[rq->rqe_Queue.rqe_deleteIdx].seqNum, /* long long */
	      rq->rqe_Queue.buffer[rq->rqe_Queue.rqe_deleteIdx].name, /* str */
	      rq->rqe_Queue.buffer[rq->rqe_Queue.rqe_deleteIdx].taskId, /* int */
	      rq->rqe_Queue.buffer[rq->rqe_Queue.rqe_deleteIdx].eventType, /* char!! */
	      SPLIT_NS(rq->rqe_Queue.buffer[rq->rqe_Queue.rqe_deleteIdx].eventTime), /* times, in ns */
	      SPLIT_NS(rq->rqe_Queue.buffer[rq->rqe_Queue.rqe_deleteIdx].vrun), /*float in ns*/
	      rq->rqe_Queue.buffer[rq->rqe_Queue.rqe_deleteIdx].normPrio, /*int*/
	      rq->rqe_Queue.buffer[rq->rqe_Queue.rqe_deleteIdx].switches /*long long*/
	      ); 
    rq->rqe_Queue.rqe_population--;
    rq->rqe_Queue.rqe_deleteIdx = (rq->rqe_Queue.rqe_deleteIdx + 1) % RQE_Q_CAP;
  } // end While
  last_seqNum = rq->rqe_Queue.buffer[rq->rqe_Queue.rqe_deleteIdx].seqNum;
  last_qIndx = rq->rqe_Queue.rqe_deleteIdx;
  // block write entries into seq file
  lenWritten=seq_write(m, logEntry, logEntryLen);

  //If the write fails, do it again upon the next block write
  if (lenWritten){
    rq->rqe_Queue.rqe_deleteIdx=first_qIndx;
    rq->rqe_Queue.rqe_population-=(last_qIndx - first_qIndx);
    failedWrites++;
  }

  //printk("yy2 seqNums %d..%d pop%d: tried to write- %d wrote-%d \n",
  //	first_seqNum, last_seqNum, rq->rqe_Queue.rqe_population, logEntryLen,lenWritten);

  if(!logEntryLen || rq->rqe_Queue.rqe_deleteIdx == stopIndex)
    return NULL;
  else
    return 1;
}

static void sysrq_sched_debug_show(void)
{
	sched_debug_show(NULL, NULL);
}

static void sysrq_rqDebug(void)
{
	rqe_Get(NULL, NULL);
}

static void sysrq_rqRESET(void)
{
	rqRESET(NULL, NULL);
}

static int sched_debug_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_debug_show, NULL);
}

static int rq_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, rqe_Get, NULL);
}


static int hr_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, rqRESET, NULL);
}

static const struct file_operations sched_debug_fops = {
	.open		= sched_debug_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations RQ_fops = {
	.open		= rq_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations HR_fops = {
	.open		= hr_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init init_sched_debug_procfs(void)
{
  struct proc_dir_entry *pe;

  pe = proc_create("sched_debug", 0444, NULL, &sched_debug_fops);
  if (!pe)
    return -ENOMEM;
  return 0;
}

static void *rq_seq_start(struct seq_file *s, loff_t *pos)
{
  struct rq *rq = cpu_rq(0); 
  if(rq->rqe_Queue.rqe_deleteIdx == stopIndex){
    if (RQ_DEBUG) {printk("END CONDITION MET %d <--> %d!\n",rq->rqe_Queue.rqe_deleteIdx,stopIndex);}
    return NULL;
  }
  if (RQ_DEBUG) {printk("BEGIN LOGS %d <--> %d!\n",rq->rqe_Queue.rqe_deleteIdx,stopIndex);}
  return rtnStatus;
}

static void *rq_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
  struct rq *rq = cpu_rq(0);
  if(rq->rqe_Queue.rqe_deleteIdx == stopIndex){
    if (RQ_DEBUG) {printk("SHOULD EXIT %d <--> %d!\n",rq->rqe_Queue.rqe_deleteIdx,stopIndex);}
    return NULL;
  }
  if (RQ_DEBUG) {printk("RESUME %d <--> %d!\n",rq->rqe_Queue.rqe_deleteIdx,stopIndex);}
  return rtnStatus;
}

static void rq_seq_stop(struct seq_file *s, void *v)
{
  return;
}

static int rq_seq_show(struct seq_file *s, void *v)
{
  rtnStatus=rqe_Get(s,v);
  return 0;
}

static struct seq_operations rq_seq_ops = {
  .start = rq_seq_start,
  .next  = rq_seq_next,
  .stop  = rq_seq_stop,
  .show  = rq_seq_show
};

static int rq0_open(struct inode *inode, struct file *file)
{
  struct rq *rq = cpu_rq(0);
  if (RQ_DEBUG) {printk("RQ_DEBUG HAS BEEN OPENED!\n");}
  rq->rqe_Queue.rqe_initialized=1;
  firstWrite=1;
  rtnStatus=1;
  stopIndex=rq->rqe_Queue.rqe_insertIdx;
  return seq_open(file, &rq_seq_ops);
}

static struct file_operations rq_file_ops = {
  .owner   = THIS_MODULE,
  .open    = rq0_open,
  .read    = seq_read,
  .llseek  = seq_lseek,
  .release = seq_release
};

static int __init init_seq_procfs(void)
{
        struct proc_dir_entry *lib;

        lib = proc_create("SEQ_RQ", 0444, NULL, &rq_file_ops);
	if (!lib)
		return -ENOMEM;
	return 0;
} 

//This is my proc file system for the library
static int __init init_RQ_procfs(void)
{
        struct proc_dir_entry *lib;

        lib = proc_create("RQ_Debug", 0444, NULL, &RQ_fops);
	if (!lib)
		return -ENOMEM;
	return 0;
} 

static int __init init_HR(void)
{
        struct proc_dir_entry *lib;

        lib = proc_create("HARD_RESET", 0444, NULL, &HR_fops);
	if (!lib)
		return -ENOMEM;
	return 0;
} 


__initcall(init_sched_debug_procfs);

//My init calls
__initcall(init_RQ_procfs);
__initcall(init_HR);
__initcall(init_seq_procfs);

void proc_sched_show_task(struct task_struct *p, struct seq_file *m)
{
	unsigned long nr_switches;

	SEQ_printf(m, "%s (%d, #threads: %d)\n", p->comm, p->pid,
						get_nr_threads(p));
	SEQ_printf(m,
		"---------------------------------------------------------\n");
#define __P(F) \
	SEQ_printf(m, "%-35s:%21Ld\n", #F, (long long)F)
#define P(F) \
	SEQ_printf(m, "%-35s:%21Ld\n", #F, (long long)p->F)
#define __PN(F) \
	SEQ_printf(m, "%-35s:%14Ld.%06ld\n", #F, SPLIT_NS((long long)F))
#define PN(F) \
	SEQ_printf(m, "%-35s:%14Ld.%06ld\n", #F, SPLIT_NS((long long)p->F))

	PN(se.exec_start);
	PN(se.vruntime);
	PN(se.sum_exec_runtime);

	nr_switches = p->nvcsw + p->nivcsw;

#ifdef CONFIG_SCHEDSTATS
	PN(se.statistics.wait_start);
	PN(se.statistics.sleep_start);
	PN(se.statistics.block_start);
	PN(se.statistics.sleep_max);
	PN(se.statistics.block_max);
	PN(se.statistics.exec_max);
	PN(se.statistics.slice_max);
	PN(se.statistics.wait_max);
	PN(se.statistics.wait_sum);
	P(se.statistics.wait_count);
	PN(se.statistics.iowait_sum);
	P(se.statistics.iowait_count);
	P(se.nr_migrations);
	P(se.statistics.nr_migrations_cold);
	P(se.statistics.nr_failed_migrations_affine);
	P(se.statistics.nr_failed_migrations_running);
	P(se.statistics.nr_failed_migrations_hot);
	P(se.statistics.nr_forced_migrations);
	P(se.statistics.nr_wakeups);
	P(se.statistics.nr_wakeups_sync);
	P(se.statistics.nr_wakeups_migrate);
	P(se.statistics.nr_wakeups_local);
	P(se.statistics.nr_wakeups_remote);
	P(se.statistics.nr_wakeups_affine);
	P(se.statistics.nr_wakeups_affine_attempts);
	P(se.statistics.nr_wakeups_passive);
	P(se.statistics.nr_wakeups_idle);

	{
		u64 avg_atom, avg_per_cpu;

		avg_atom = p->se.sum_exec_runtime;
		if (nr_switches)
			do_div(avg_atom, nr_switches);
		else
			avg_atom = -1LL;

		avg_per_cpu = p->se.sum_exec_runtime;
		if (p->se.nr_migrations) {
			avg_per_cpu = div64_u64(avg_per_cpu,
						p->se.nr_migrations);
		} else {
			avg_per_cpu = -1LL;
		}

		__PN(avg_atom);
		__PN(avg_per_cpu);
	}
#endif
	__P(nr_switches);
	SEQ_printf(m, "%-35s:%21Ld\n",
		   "nr_voluntary_switches", (long long)p->nvcsw);
	SEQ_printf(m, "%-35s:%21Ld\n",
		   "nr_involuntary_switches", (long long)p->nivcsw);

	P(se.load.weight);
	P(policy);
	P(prio);
#undef PN
#undef __PN
#undef P
#undef __P

	{
		unsigned int this_cpu = raw_smp_processor_id();
		u64 t0, t1;

		t0 = cpu_clock(this_cpu);
		t1 = cpu_clock(this_cpu);
		SEQ_printf(m, "%-35s:%21Ld\n",
			   "clock-delta", (long long)(t1-t0));
	}
}

void proc_sched_set_task(struct task_struct *p)
{
#ifdef CONFIG_SCHEDSTATS
	memset(&p->se.statistics, 0, sizeof(p->se.statistics));
#endif
}

