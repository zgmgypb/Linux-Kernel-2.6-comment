/*
 *  linux/kernel/timer.c
 *
 *  Kernel internal timers, kernel timekeeping, basic process system calls
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-01-28  Modified by Finn Arne Gangstad to make timers scale better.
 *
 *  1997-09-10  Updated NTP code according to technical memorandum Jan '96
 *              "A Kernel Model for Precision Timekeeping" by Dave Mills
 *  1998-12-24  Fixed a xtime SMP race (we need the xtime_lock rw spinlock to
 *              serialize accesses to xtime/lost_ticks).
 *                              Copyright (C) 1998  Andrea Arcangeli
 *  1999-03-10  Improved NTP compatibility by Ulrich Windl
 *  2002-05-31	Move sys_sysinfo here and make its locking sane, Robert Love
 *  2000-10-05  Implemented scalable SMP per-CPU timer handling.
 *                              Copyright (C) 2000, 2001, 2002  Ingo Molnar
 *              Designed by David S. Miller, Alexey Kuznetsov and Ingo Molnar
 */

#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/notifier.h>
#include <linux/thread_info.h>
#include <linux/time.h>
#include <linux/jiffies.h>
#include <linux/cpu.h>
#include <linux/syscalls.h>

#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/div64.h>
#include <asm/timex.h>
#include <asm/io.h>

#ifdef CONFIG_TIME_INTERPOLATION
static void time_interpolator_update(long delta_nsec);
#else
#define time_interpolator_update(x)
#endif

/*
 * per-CPU timer vector definitions:
 */
#define TVN_BITS 6
#define TVR_BITS 8
#define TVN_SIZE (1 << TVN_BITS)
#define TVR_SIZE (1 << TVR_BITS)
#define TVN_MASK (TVN_SIZE - 1)
#define TVR_MASK (TVR_SIZE - 1)

typedef struct tvec_s {
	struct list_head vec[TVN_SIZE];
} tvec_t;

typedef struct tvec_root_s {
	struct list_head vec[TVR_SIZE];
} tvec_root_t;

/**
 * 每个CPU上的动态定时器元素
 */
struct tvec_t_base_s {
	spinlock_t lock;
	/**
	 * 需要检查的动态定时器的最早到期时间。
	 * 如果这个值与jiffies一样，说明可延迟函数没有积压。
	 * 如果这个值小于jiffies，说明前几个节拍相关的可延迟函数必须被处理。
	 * 在初始化时，它的值与jiffies相等，然后由run_timer_softieq函数增加它的值。
	 * 如果相关的可延迟函数很长一段时间都没有被执行时，timer_jiffies可能会落后于jiffies很多。
	 */
	unsigned long timer_jiffies;
	/**
	 * 在多CPU上，running_timer指向本地CPU当前正处理的动态定时器的timer_list结构。
	 */
	struct timer_list *running_timer;
	/**
	 * 它包含一个数组，包含在紧接着到来的255个节拍内将要到期的所有动态定时器。
	 */
	tvec_root_t tv1;
	/**
	 * 它包含一个数组，包含在紧接着到来的2^14-1个节拍内将要到期的所有动态定时器。
	 */
	tvec_t tv2;
	/**
	 * 它包含一个数组，包含在紧接着到来的2^20-1个节拍内将要到期的所有动态定时器。
	 */
	tvec_t tv3;
	/**
	 * 它包含一个数组，包含在紧接着到来的2^26-1个节拍内将要到期的所有动态定时器。
	 */
	tvec_t tv4;
	/**
	 * 它包含一个数组，包含在一个大的expires内到期的动态定时器。
	 */
	tvec_t tv5;
} ____cacheline_aligned_in_smp;

typedef struct tvec_t_base_s tvec_base_t;

static inline void set_running_timer(tvec_base_t *base,
					struct timer_list *timer)
{
#ifdef CONFIG_SMP
	base->running_timer = timer;
#endif
}

/* Fake initialization */
/**
 * 动态定时器的主要结构。每个元素是一个tvec_base_t对象。
 * 它包含动态定时器需要的所有数据。
 */
static DEFINE_PER_CPU(tvec_base_t, tvec_bases) = { SPIN_LOCK_UNLOCKED };

static void check_timer_failed(struct timer_list *timer)
{
	static int whine_count;
	if (whine_count < 16) {
		whine_count++;
		printk("Uninitialised timer!\n");
		printk("This is just a warning.  Your computer is OK\n");
		printk("function=0x%p, data=0x%lx\n",
			timer->function, timer->data);
		dump_stack();
	}
	/*
	 * Now fix it up
	 */
	spin_lock_init(&timer->lock);
	timer->magic = TIMER_MAGIC;
}

static inline void check_timer(struct timer_list *timer)
{
	if (timer->magic != TIMER_MAGIC)
		check_timer_failed(timer);
}


static void internal_add_timer(tvec_base_t *base, struct timer_list *timer)
{
	unsigned long expires = timer->expires;
	unsigned long idx = expires - base->timer_jiffies;
	struct list_head *vec;

	if (idx < TVR_SIZE) {
		int i = expires & TVR_MASK;
		vec = base->tv1.vec + i;
	} else if (idx < 1 << (TVR_BITS + TVN_BITS)) {
		int i = (expires >> TVR_BITS) & TVN_MASK;
		vec = base->tv2.vec + i;
	} else if (idx < 1 << (TVR_BITS + 2 * TVN_BITS)) {
		int i = (expires >> (TVR_BITS + TVN_BITS)) & TVN_MASK;
		vec = base->tv3.vec + i;
	} else if (idx < 1 << (TVR_BITS + 3 * TVN_BITS)) {
		int i = (expires >> (TVR_BITS + 2 * TVN_BITS)) & TVN_MASK;
		vec = base->tv4.vec + i;
	} else if ((signed long) idx < 0) {
		/*
		 * Can happen if you add a timer with expires == jiffies,
		 * or you set a timer to go off in the past
		 */
		vec = base->tv1.vec + (base->timer_jiffies & TVR_MASK);
	} else {
		int i;
		/* If the timeout is larger than 0xffffffff on 64-bit
		 * architectures then we use the maximum timeout:
		 */
		if (idx > 0xffffffffUL) {
			idx = 0xffffffffUL;
			expires = idx + base->timer_jiffies;
		}
		i = (expires >> (TVR_BITS + 3 * TVN_BITS)) & TVN_MASK;
		vec = base->tv5.vec + i;
	}
	/*
	 * Timers are FIFO:
	 */
	list_add_tail(&timer->entry, vec);
}

int __mod_timer(struct timer_list *timer, unsigned long expires)
{
	tvec_base_t *old_base, *new_base;
	unsigned long flags;
	int ret = 0;

	BUG_ON(!timer->function);

	check_timer(timer);

	spin_lock_irqsave(&timer->lock, flags);
	new_base = &__get_cpu_var(tvec_bases);
repeat:
	old_base = timer->base;

	/*
	 * Prevent deadlocks via ordering by old_base < new_base.
	 */
	if (old_base && (new_base != old_base)) {
		if (old_base < new_base) {
			spin_lock(&new_base->lock);
			spin_lock(&old_base->lock);
		} else {
			spin_lock(&old_base->lock);
			spin_lock(&new_base->lock);
		}
		/*
		 * The timer base might have been cancelled while we were
		 * trying to take the lock(s):
		 */
		if (timer->base != old_base) {
			spin_unlock(&new_base->lock);
			spin_unlock(&old_base->lock);
			goto repeat;
		}
	} else {
		spin_lock(&new_base->lock);
		if (timer->base != old_base) {
			spin_unlock(&new_base->lock);
			goto repeat;
		}
	}

	/*
	 * Delete the previous timeout (if there was any), and install
	 * the new one:
	 */
	if (old_base) {
		list_del(&timer->entry);
		ret = 1;
	}
	timer->expires = expires;
	internal_add_timer(new_base, timer);
	timer->base = new_base;

	if (old_base && (new_base != old_base))
		spin_unlock(&old_base->lock);
	spin_unlock(&new_base->lock);
	spin_unlock_irqrestore(&timer->lock, flags);

	return ret;
}

EXPORT_SYMBOL(__mod_timer);

/***
 * add_timer_on - start a timer on a particular CPU
 * @timer: the timer to be added
 * @cpu: the CPU to start it on
 *
 * This is not very scalable on SMP. Double adds are not possible.
 */
void add_timer_on(struct timer_list *timer, int cpu)
{
	tvec_base_t *base = &per_cpu(tvec_bases, cpu);
  	unsigned long flags;
  
  	BUG_ON(timer_pending(timer) || !timer->function);

	check_timer(timer);

	spin_lock_irqsave(&base->lock, flags);
	internal_add_timer(base, timer);
	timer->base = base;
	spin_unlock_irqrestore(&base->lock, flags);
}


/***
 * mod_timer - modify a timer's timeout
 * @timer: the timer to be modified
 *
 * mod_timer is a more efficient way to update the expire field of an
 * active timer (if the timer is inactive it will be activated)
 *
 * mod_timer(timer, expires) is equivalent to:
 *
 *     del_timer(timer); timer->expires = expires; add_timer(timer);
 *
 * Note that if there are multiple unserialized concurrent users of the
 * same timer, then mod_timer() is the only safe way to modify the timeout,
 * since add_timer() cannot modify an already running timer.
 *
 * The function returns whether it has modified a pending timer or not.
 * (ie. mod_timer() of an inactive timer returns 0, mod_timer() of an
 * active timer returns 1.)
 */
int mod_timer(struct timer_list *timer, unsigned long expires)
{
	BUG_ON(!timer->function);

	check_timer(timer);

	/*
	 * This is a common optimization triggered by the
	 * networking code - if the timer is re-modified
	 * to be the same thing then just return:
	 */
	if (timer->expires == expires && timer_pending(timer))
		return 1;

	return __mod_timer(timer, expires);
}

EXPORT_SYMBOL(mod_timer);

/***
 * del_timer - deactive a timer.
 * @timer: the timer to be deactivated
 *
 * del_timer() deactivates a timer - this works on both active and inactive
 * timers.
 *
 * The function returns whether it has deactivated a pending timer or not.
 * (ie. del_timer() of an inactive timer returns 0, del_timer() of an
 * active timer returns 1.)
 */
int del_timer(struct timer_list *timer)
{
	unsigned long flags;
	tvec_base_t *base;

	check_timer(timer);

repeat:
 	base = timer->base;
	if (!base)
		return 0;
	spin_lock_irqsave(&base->lock, flags);
	if (base != timer->base) {
		spin_unlock_irqrestore(&base->lock, flags);
		goto repeat;
	}
	list_del(&timer->entry);
	/* Need to make sure that anybody who sees a NULL base also sees the list ops */
	smp_wmb();
	timer->base = NULL;
	spin_unlock_irqrestore(&base->lock, flags);

	return 1;
}

EXPORT_SYMBOL(del_timer);

#ifdef CONFIG_SMP
/***
 * del_timer_sync - deactivate a timer and wait for the handler to finish.
 * @timer: the timer to be deactivated
 *
 * This function only differs from del_timer() on SMP: besides deactivating
 * the timer it also makes sure the handler has finished executing on other
 * CPUs.
 *
 * Synchronization rules: callers must prevent restarting of the timer,
 * otherwise this function is meaningless. It must not be called from
 * interrupt contexts. The caller must not hold locks which would prevent
 * completion of the timer's handler.  Upon exit the timer is not queued and
 * the handler is not running on any CPU.
 *
 * The function returns whether it has deactivated a pending timer or not.
 *
 * del_timer_sync() is slow and complicated because it copes with timer
 * handlers which re-arm the timer (periodic timers).  If the timer handler
 * is known to not do this (a single shot timer) then use
 * del_singleshot_timer_sync() instead.
 */
int del_timer_sync(struct timer_list *timer)
{
	tvec_base_t *base;
	int i, ret = 0;

	check_timer(timer);

del_again:
	ret += del_timer(timer);

	for_each_online_cpu(i) {
		base = &per_cpu(tvec_bases, i);
		if (base->running_timer == timer) {
			while (base->running_timer == timer) {
				cpu_relax();
				preempt_check_resched();
			}
			break;
		}
	}
	smp_rmb();
	if (timer_pending(timer))
		goto del_again;

	return ret;
}
EXPORT_SYMBOL(del_timer_sync);

/***
 * del_singleshot_timer_sync - deactivate a non-recursive timer
 * @timer: the timer to be deactivated
 *
 * This function is an optimization of del_timer_sync for the case where the
 * caller can guarantee the timer does not reschedule itself in its timer
 * function.
 *
 * Synchronization rules: callers must prevent restarting of the timer,
 * otherwise this function is meaningless. It must not be called from
 * interrupt contexts. The caller must not hold locks which wold prevent
 * completion of the timer's handler.  Upon exit the timer is not queued and
 * the handler is not running on any CPU.
 *
 * The function returns whether it has deactivated a pending timer or not.
 */
/**
 * 从定时器链表中删除一个定时器
 */
int del_singleshot_timer_sync(struct timer_list *timer)
{
	int ret = del_timer(timer);

	if (!ret) {
		ret = del_timer_sync(timer);
		BUG_ON(ret);
	}

	return ret;
}
EXPORT_SYMBOL(del_singleshot_timer_sync);
#endif

/**
 * 动态定时器分别放在五个链表中。tv1到tv5,分别保存2^8,2^14,2^20,2^26,2^32个tick内到达的定时器
 * 当tv1中为空时，调用cascade来将tv5->tv4->tv3->tv2->tv1
 * 就是把后一个链表中的动态定时器往上移动
 */
static int cascade(tvec_base_t *base, tvec_t *tv, int index)
{
	/* cascade all the timers from tv up one level */
	struct list_head *head, *curr;

	head = tv->vec + index;
	curr = head->next;
	/*
	 * We are removing _all_ timers from the list, so we don't  have to
	 * detach them individually, just clear the list afterwards.
	 */
	while (curr != head) {
		struct timer_list *tmp;

		tmp = list_entry(curr, struct timer_list, entry);
		BUG_ON(tmp->base != base);
		curr = curr->next;
		internal_add_timer(base, tmp);
	}
	INIT_LIST_HEAD(head);

	return index;
}

/***
 * __run_timers - run all expired timers (if any) on this CPU.
 * @base: the timer vector to be processed.
 *
 * This function cascades all vectors and executes all expired timer
 * vectors.
 */
#define INDEX(N) (base->timer_jiffies >> (TVR_BITS + N * TVN_BITS)) & TVN_MASK

/**
 * 处理本CPU的动态定时器。在软中断上下文中执行。
 */
static inline void __run_timers(tvec_base_t *base)
{
	struct timer_list *timer;

	/**
	 * 由于结构可能在add_timer或者mod_timer,del_timer中被访问。
	 * 而这些API是提供给内核开发者用，不知内核开发者会将它们用在什么场景
	 * 所以只能用spin_lock_irq这种方式加锁了。
	 * 因为是在软中断上下文中执行，这里一定是开中断的，所以不必保存中断，强关后强开就行了。
	 * 虽然强开强关中断是一种不道德的行为，在这里却不会有问题。
	 */
	spin_lock_irq(&base->lock);
	/**
	 * 由于jiffies和time_jiffies的值经常是一样的，所以这层循环常常只执行一次。
	 * 但是，如果在执行软中断时，发生了时钟中断，那么，在这个节拍可能会到期新的动态定时器。
	 * 此时就不一定是只执行一次了。
	 */
	while (time_after_eq(jiffies, base->timer_jiffies)) {
		struct list_head work_list = LIST_HEAD_INIT(work_list);
		struct list_head *head = &work_list;
		/**
		 * index是base->tv1链表中索引值。它保存着下一次将要处理的定时器。
		 */
 		int index = base->timer_jiffies & TVR_MASK;
 
		/*
		 * Cascade timers:
		 */
		/**
		 * index == 0说明tv1中所有链表都已经被检查过了。通过cascade来过滤动态定时器。
		 * cascade将tv5->tv4->tv3->tv2->tv1
		 */
		if (!index &&
			(!cascade(base, &base->tv2, INDEX(0))) &&
				(!cascade(base, &base->tv3, INDEX(1))) &&
					!cascade(base, &base->tv4, INDEX(2)))
			cascade(base, &base->tv5, INDEX(3));
		/**
		 * 处理一次循环，就将timer_jiffies递增。
		 */
		++base->timer_jiffies; 
		/**
		 * 将tv1.vec[indx]中的定时器移动到work_list中。
		 * 现在head指向work_list的头。
		 */
		list_splice_init(base->tv1.vec + index, &work_list);
repeat:
		/**
		 * 遍历链表中的每一个定时器。
		 */
		if (!list_empty(head)) {
			void (*fn)(unsigned long);
			unsigned long data;

			/**
			 * 从链表中取出定时器。
			 */
			timer = list_entry(head->next,struct timer_list,entry);
 			fn = timer->function;
 			data = timer->data;

			/**
			 * 从链表中删除定时器。
			 */
			list_del(&timer->entry);
			/**
			 * 设置当前CPU正在处理的定时器。
			 */
			set_running_timer(base, timer);
			smp_wmb();
			timer->base = NULL;
			/**
			 * 注意锁的用法。它保护的定时器链表，但是没有保护回调函数。
			 */
			spin_unlock_irq(&base->lock);
			{
				u32 preempt_count = preempt_count();
				fn(data);
				/**
				 * 在定时器函数中，不能随意修改抢占计数。
				 */
				if (preempt_count != preempt_count()) {
					printk("huh, entered %p with %08x, exited with %08x?\n", fn, preempt_count, preempt_count());
					BUG();
				}
			}
			spin_lock_irq(&base->lock);
			goto repeat;
		}
	}
	set_running_timer(base, NULL);
	spin_unlock_irq(&base->lock);
}

#ifdef CONFIG_NO_IDLE_HZ
/*
 * Find out when the next timer event is due to happen. This
 * is used on S/390 to stop all activity when a cpus is idle.
 * This functions needs to be called disabled.
 */
unsigned long next_timer_interrupt(void)
{
	tvec_base_t *base;
	struct list_head *list;
	struct timer_list *nte;
	unsigned long expires;
	tvec_t *varray[4];
	int i, j;

	base = &__get_cpu_var(tvec_bases);
	spin_lock(&base->lock);
	expires = base->timer_jiffies + (LONG_MAX >> 1);
	list = 0;

	/* Look for timer events in tv1. */
	j = base->timer_jiffies & TVR_MASK;
	do {
		list_for_each_entry(nte, base->tv1.vec + j, entry) {
			expires = nte->expires;
			if (j < (base->timer_jiffies & TVR_MASK))
				list = base->tv2.vec + (INDEX(0));
			goto found;
		}
		j = (j + 1) & TVR_MASK;
	} while (j != (base->timer_jiffies & TVR_MASK));

	/* Check tv2-tv5. */
	varray[0] = &base->tv2;
	varray[1] = &base->tv3;
	varray[2] = &base->tv4;
	varray[3] = &base->tv5;
	for (i = 0; i < 4; i++) {
		j = INDEX(i);
		do {
			if (list_empty(varray[i]->vec + j)) {
				j = (j + 1) & TVN_MASK;
				continue;
			}
			list_for_each_entry(nte, varray[i]->vec + j, entry)
				if (time_before(nte->expires, expires))
					expires = nte->expires;
			if (j < (INDEX(i)) && i < 3)
				list = varray[i + 1]->vec + (INDEX(i + 1));
			goto found;
		} while (j != (INDEX(i)));
	}
found:
	if (list) {
		/*
		 * The search wrapped. We need to look at the next list
		 * from next tv element that would cascade into tv element
		 * where we found the timer element.
		 */
		list_for_each_entry(nte, list, entry) {
			if (time_before(nte->expires, expires))
				expires = nte->expires;
		}
	}
	spin_unlock(&base->lock);
	return expires;
}
#endif

/******************************************************************/

/*
 * Timekeeping variables
 */
unsigned long tick_usec = TICK_USEC; 		/* USER_HZ period (usec) */
/**
 * 每个tick的ns值。如果是1000HZ，这个值就是1000 * 1000
 * 在PC上，它被初始化成9999848，对应的Hz约为1000.15Hz。
 * 如果计算机被外部时钟同步的话，它的值可能被内核自动调整（参见adjtimex调用）
 */
unsigned long tick_nsec = TICK_NSEC;		/* ACTHZ period (nsec) */

/* 
 * The current time 
 * wall_to_monotonic is what we need to add to xtime (or xtime corrected 
 * for sub jiffie times) to get to monotonic time.  Monotonic is pegged
 * at zero at system boot time, so wall_to_monotonic will be negative,
 * however, we will ALWAYS keep the tv_nsec part positive so we can use
 * the usual normalization.
 */
struct timespec xtime __attribute__ ((aligned (16)));
struct timespec wall_to_monotonic __attribute__ ((aligned (16)));

EXPORT_SYMBOL(xtime);

/* Don't completely fail for HZ > 500.  */
int tickadj = 500/HZ ? : 1;		/* microsecs */


/*
 * phase-lock loop variables
 */
/* TIME_ERROR prevents overwriting the CMOS clock */
int time_state = TIME_OK;		/* clock synchronization status	*/
int time_status = STA_UNSYNC;		/* clock status bits		*/
long time_offset;			/* time adjustment (us)		*/
long time_constant = 2;			/* pll time constant		*/
long time_tolerance = MAXFREQ;		/* frequency tolerance (ppm)	*/
long time_precision = 1;		/* clock precision (us)		*/
long time_maxerror = NTP_PHASE_LIMIT;	/* maximum error (us)		*/
long time_esterror = NTP_PHASE_LIMIT;	/* estimated error (us)		*/
long time_phase;			/* phase offset (scaled us)	*/
long time_freq = (((NSEC_PER_SEC + HZ/2) % HZ - HZ/2) << SHIFT_USEC) / NSEC_PER_USEC;
					/* frequency offset (scaled ppm)*/
long time_adj;				/* tick adjust (scaled 1 / HZ)	*/
long time_reftime;			/* time at last adjustment (s)	*/
long time_adjust;
long time_next_adjust;

/*
 * this routine handles the overflow of the microsecond field
 *
 * The tricky bits of code to handle the accurate clock support
 * were provided by Dave Mills (Mills@UDEL.EDU) of NTP fame.
 * They were originally developed for SUN and DEC kernels.
 * All the kudos should go to Dave for this stuff.
 *
 */
static void second_overflow(void)
{
    long ltemp;

    /* Bump the maxerror field */
    time_maxerror += time_tolerance >> SHIFT_USEC;
    if ( time_maxerror > NTP_PHASE_LIMIT ) {
	time_maxerror = NTP_PHASE_LIMIT;
	time_status |= STA_UNSYNC;
    }

    /*
     * Leap second processing. If in leap-insert state at
     * the end of the day, the system clock is set back one
     * second; if in leap-delete state, the system clock is
     * set ahead one second. The microtime() routine or
     * external clock driver will insure that reported time
     * is always monotonic. The ugly divides should be
     * replaced.
     */
    switch (time_state) {

    case TIME_OK:
	if (time_status & STA_INS)
	    time_state = TIME_INS;
	else if (time_status & STA_DEL)
	    time_state = TIME_DEL;
	break;

    case TIME_INS:
	if (xtime.tv_sec % 86400 == 0) {
	    xtime.tv_sec--;
	    wall_to_monotonic.tv_sec++;
	    /* The timer interpolator will make time change gradually instead
	     * of an immediate jump by one second.
	     */
	    time_interpolator_update(-NSEC_PER_SEC);
	    time_state = TIME_OOP;
	    clock_was_set();
	    printk(KERN_NOTICE "Clock: inserting leap second 23:59:60 UTC\n");
	}
	break;

    case TIME_DEL:
	if ((xtime.tv_sec + 1) % 86400 == 0) {
	    xtime.tv_sec++;
	    wall_to_monotonic.tv_sec--;
	    /* Use of time interpolator for a gradual change of time */
	    time_interpolator_update(NSEC_PER_SEC);
	    time_state = TIME_WAIT;
	    clock_was_set();
	    printk(KERN_NOTICE "Clock: deleting leap second 23:59:59 UTC\n");
	}
	break;

    case TIME_OOP:
	time_state = TIME_WAIT;
	break;

    case TIME_WAIT:
	if (!(time_status & (STA_INS | STA_DEL)))
	    time_state = TIME_OK;
    }

    /*
     * Compute the phase adjustment for the next second. In
     * PLL mode, the offset is reduced by a fixed factor
     * times the time constant. In FLL mode the offset is
     * used directly. In either mode, the maximum phase
     * adjustment for each second is clamped so as to spread
     * the adjustment over not more than the number of
     * seconds between updates.
     */
    if (time_offset < 0) {
	ltemp = -time_offset;
	if (!(time_status & STA_FLL))
	    ltemp >>= SHIFT_KG + time_constant;
	if (ltemp > (MAXPHASE / MINSEC) << SHIFT_UPDATE)
	    ltemp = (MAXPHASE / MINSEC) << SHIFT_UPDATE;
	time_offset += ltemp;
	time_adj = -ltemp << (SHIFT_SCALE - SHIFT_HZ - SHIFT_UPDATE);
    } else {
	ltemp = time_offset;
	if (!(time_status & STA_FLL))
	    ltemp >>= SHIFT_KG + time_constant;
	if (ltemp > (MAXPHASE / MINSEC) << SHIFT_UPDATE)
	    ltemp = (MAXPHASE / MINSEC) << SHIFT_UPDATE;
	time_offset -= ltemp;
	time_adj = ltemp << (SHIFT_SCALE - SHIFT_HZ - SHIFT_UPDATE);
    }

    /*
     * Compute the frequency estimate and additional phase
     * adjustment due to frequency error for the next
     * second. When the PPS signal is engaged, gnaw on the
     * watchdog counter and update the frequency computed by
     * the pll and the PPS signal.
     */
    pps_valid++;
    if (pps_valid == PPS_VALID) {	/* PPS signal lost */
	pps_jitter = MAXTIME;
	pps_stabil = MAXFREQ;
	time_status &= ~(STA_PPSSIGNAL | STA_PPSJITTER |
			 STA_PPSWANDER | STA_PPSERROR);
    }
    ltemp = time_freq + pps_freq;
    if (ltemp < 0)
	time_adj -= -ltemp >>
	    (SHIFT_USEC + SHIFT_HZ - SHIFT_SCALE);
    else
	time_adj += ltemp >>
	    (SHIFT_USEC + SHIFT_HZ - SHIFT_SCALE);

#if HZ == 100
    /* Compensate for (HZ==100) != (1 << SHIFT_HZ).
     * Add 25% and 3.125% to get 128.125; => only 0.125% error (p. 14)
     */
    if (time_adj < 0)
	time_adj -= (-time_adj >> 2) + (-time_adj >> 5);
    else
	time_adj += (time_adj >> 2) + (time_adj >> 5);
#endif
#if HZ == 1000
    /* Compensate for (HZ==1000) != (1 << SHIFT_HZ).
     * Add 1.5625% and 0.78125% to get 1023.4375; => only 0.05% error (p. 14)
     */
    if (time_adj < 0)
	time_adj -= (-time_adj >> 6) + (-time_adj >> 7);
    else
	time_adj += (time_adj >> 6) + (time_adj >> 7);
#endif
}

/* in the NTP reference this is called "hardclock()" */
static void update_wall_time_one_tick(void)
{
	long time_adjust_step, delta_nsec;

	if ( (time_adjust_step = time_adjust) != 0 ) {
	    /* We are doing an adjtime thing. 
	     *
	     * Prepare time_adjust_step to be within bounds.
	     * Note that a positive time_adjust means we want the clock
	     * to run faster.
	     *
	     * Limit the amount of the step to be in the range
	     * -tickadj .. +tickadj
	     */
	     if (time_adjust > tickadj)
		time_adjust_step = tickadj;
	     else if (time_adjust < -tickadj)
		time_adjust_step = -tickadj;

	    /* Reduce by this step the amount of time left  */
	    time_adjust -= time_adjust_step;
	}
	delta_nsec = tick_nsec + time_adjust_step * 1000;
	/*
	 * Advance the phase, once it gets to one microsecond, then
	 * advance the tick more.
	 */
	time_phase += time_adj;
	if (time_phase <= -FINENSEC) {
		long ltemp = -time_phase >> (SHIFT_SCALE - 10);
		time_phase += ltemp << (SHIFT_SCALE - 10);
		delta_nsec -= ltemp;
	}
	else if (time_phase >= FINENSEC) {
		long ltemp = time_phase >> (SHIFT_SCALE - 10);
		time_phase -= ltemp << (SHIFT_SCALE - 10);
		delta_nsec += ltemp;
	}
	xtime.tv_nsec += delta_nsec;
	time_interpolator_update(delta_nsec);

	/* Changes by adjtime() do not take effect till next tick. */
	if (time_next_adjust != 0) {
		time_adjust = time_next_adjust;
		time_next_adjust = 0;
	}
}

/*
 * Using a loop looks inefficient, but "ticks" is
 * usually just one (we shouldn't be losing ticks,
 * we're doing this this way mainly for interrupt
 * latency reasons, not because we think we'll
 * have lots of lost timer ticks
 */
static void update_wall_time(unsigned long ticks)
{
	do {
		ticks--;
		update_wall_time_one_tick();
		if (xtime.tv_nsec >= 1000000000) {
			xtime.tv_nsec -= 1000000000;
			xtime.tv_sec++;
			second_overflow();
		}
	} while (ticks);
}

/*
 * Called from the timer interrupt handler to charge one tick to the current 
 * process.  user_tick is 1 if the tick is user time, 0 for system.
 */
/**
 * 更新CPU统计计数
 */
void update_process_times(int user_tick)
{
	struct task_struct *p = current;
	int cpu = smp_processor_id();

	/* Note: this timer irq context must be accounted for as well. */
	/**
	 * 检查进程在用户态或者内核态运行了多长时间。
	 * 如果超过了运行时间，还会杀死进程。
	 */
	if (user_tick)
		account_user_time(p, jiffies_to_cputime(1));
	else
		account_system_time(p, HARDIRQ_OFFSET, jiffies_to_cputime(1));
	/**
	 * 调用raise_softirq(TIMER_SOFTIRQ);激活本地CPU上的TIMER_SOFTIRQ
	 * 通过内核函数add_timer添加的定时器在相应的任务队列上运行。
	 * 而用户的定时器是通过信号来实现的。
	 */
	run_local_timers();
	/**
	 * 如果系统经历了一个静止状态，那么调用tasklet_schedule来激活本地CPU的
	 * rcu_tasklet任务的队列。这个任务负责释放RCU相关的内存。
	 */
	if (rcu_pending(cpu))
		rcu_check_callbacks(cpu, user_tick);
	/**
	 * scheduler_tick使当前进程的时间片计数器减1。
	 */
	scheduler_tick();
}

/*
 * Nr of active tasks - counted in fixed-point numbers
 */
static unsigned long count_active_tasks(void)
{
	return (nr_running() + nr_uninterruptible()) * FIXED_1;
}

/*
 * Hmm.. Changed this, as the GNU make sources (load.c) seems to
 * imply that avenrun[] is the standard name for this kind of thing.
 * Nothing else seems to be standardized: the fractional size etc
 * all seem to differ on different machines.
 *
 * Requires xtime_lock to access.
 */
unsigned long avenrun[3];

/*
 * calc_load - given tick count, update the avenrun load estimates.
 * This is called while holding a write_lock on xtime_lock.
 */
/**
 * 记录系统负载，用户输入uptime命令时，即会看到这些统计值。
 * 平均负载包含有TASK_RUNNING和TASK_UNINTERRUPT状态的进程。由于处于TASK_UNINTERRUPT的进程少，所以高负载一般意味着系统繁忙。
 * 但是这也说明在某些情况下，有反例出现。
 * 被update_times在每个节拍调用。
 */
static inline void calc_load(unsigned long ticks)
{
	unsigned long active_tasks; /* fixed-point */
	static int count = LOAD_FREQ;

	count -= ticks;
	if (count < 0) {
		count += LOAD_FREQ;
		active_tasks = count_active_tasks();
		CALC_LOAD(avenrun[0], EXP_1, active_tasks);
		CALC_LOAD(avenrun[1], EXP_5, active_tasks);
		CALC_LOAD(avenrun[2], EXP_15, active_tasks);
	}
}

/* jiffies at the most recent update of wall time */
/**
 * 墙上时间。即xtime最后被更新的时间。
 */
unsigned long wall_jiffies = INITIAL_JIFFIES;

/*
 * This read-write spinlock protects us from races in SMP while
 * playing with xtime and avenrun.
 */
#ifndef ARCH_HAVE_XTIME_LOCK
seqlock_t xtime_lock __cacheline_aligned_in_smp = SEQLOCK_UNLOCKED;

EXPORT_SYMBOL(xtime_lock);
#endif

/*
 * This function runs timers and the timer-tq in bottom half context.
 */
/**
 * TIMER_SOFTIRQ软中断执行体。
 */
static void run_timer_softirq(struct softirq_action *h)
{
	/**
	 * 将CPU相关的tvec_base_t结构取到base中。
	 */
	tvec_base_t *base = &__get_cpu_var(tvec_bases);

	if (time_after_eq(jiffies, base->timer_jiffies))
		__run_timers(base);
}

/*
 * Called by the local, per-CPU timer interrupt on SMP.
 */
void run_local_timers(void)
{
	raise_softirq(TIMER_SOFTIRQ);
}

/*
 * Called by the timer interrupt. xtime_lock must already be taken
 * by the timer IRQ!
 */
/**
 * 更新xtime的值。并计算系统负载。
 */
static inline void update_times(void)
{
	unsigned long ticks;

	ticks = jiffies - wall_jiffies;
	/**
	 * ticks会==0？？？
	 * 这是有可能的，wall_jiffies还可能会在其他地方被修改，从而与jiffies一致。
	 */
	if (ticks) {
		/**
		 * 一般来说，会运行到这里来，那么就更新wall_jiffies吧。。
		 * 奇怪的是，直接用wall_jiffies = jiffies不就行了。
		 */
		wall_jiffies += ticks;
		/**
		 * 更新xtime
		 */
		update_wall_time(ticks);
	}
	/**
	 * 计算负载平衡。
	 */
	calc_load(ticks);
}
  
/*
 * The 64-bit jiffies value is not atomic - you MUST NOT read it
 * without sampling the sequence number in xtime_lock.
 * jiffies is defined in the linker script...
 */

/**
 * 被do_timer_interrupt调用，时钟中断的一部分。
 */
void do_timer(struct pt_regs *regs)
{
	/**
	 * 直接加jiffies_64是安全的，因为外层函数timer_interrupt此时仍然保持着xtime_lock顺序锁。
	 */
	jiffies_64++;
	/**
	 * 调用update_times更新系统日期和时间,并计算当前系统负载。
	 */
	update_times();
}

#ifdef __ARCH_WANT_SYS_ALARM

/*
 * For backwards compatibility?  This can be done in libc so Alpha
 * and all newer ports shouldn't need it.
 */
asmlinkage unsigned long sys_alarm(unsigned int seconds)
{
	struct itimerval it_new, it_old;
	unsigned int oldalarm;

	it_new.it_interval.tv_sec = it_new.it_interval.tv_usec = 0;
	it_new.it_value.tv_sec = seconds;
	it_new.it_value.tv_usec = 0;
	do_setitimer(ITIMER_REAL, &it_new, &it_old);
	oldalarm = it_old.it_value.tv_sec;
	/* ehhh.. We can't return 0 if we have an alarm pending.. */
	/* And we'd better return too much than too little anyway */
	if ((!oldalarm && it_old.it_value.tv_usec) || it_old.it_value.tv_usec >= 500000)
		oldalarm++;
	return oldalarm;
}

#endif

#ifndef __alpha__

/*
 * The Alpha uses getxpid, getxuid, and getxgid instead.  Maybe this
 * should be moved into arch/i386 instead?
 */

/**
 * sys_getpid - return the thread group id of the current process
 *
 * Note, despite the name, this returns the tgid not the pid.  The tgid and
 * the pid are identical unless CLONE_THREAD was specified on clone() in
 * which case the tgid is the same in all threads of the same group.
 *
 * This is SMP safe as current->tgid does not change.
 */
asmlinkage long sys_getpid(void)
{
	return current->tgid;
}

/*
 * Accessing ->group_leader->real_parent is not SMP-safe, it could
 * change from under us. However, rather than getting any lock
 * we can use an optimistic algorithm: get the parent
 * pid, and go back and check that the parent is still
 * the same. If it has changed (which is extremely unlikely
 * indeed), we just try again..
 *
 * NOTE! This depends on the fact that even if we _do_
 * get an old value of "parent", we can happily dereference
 * the pointer (it was and remains a dereferencable kernel pointer
 * no matter what): we just can't necessarily trust the result
 * until we know that the parent pointer is valid.
 *
 * NOTE2: ->group_leader never changes from under us.
 */
asmlinkage long sys_getppid(void)
{
	int pid;
	struct task_struct *me = current;
	struct task_struct *parent;

	parent = me->group_leader->real_parent;
	for (;;) {
		pid = parent->tgid;
#ifdef CONFIG_SMP
{
		struct task_struct *old = parent;

		/*
		 * Make sure we read the pid before re-reading the
		 * parent pointer:
		 */
		rmb();
		parent = me->group_leader->real_parent;
		if (old != parent)
			continue;
}
#endif
		break;
	}
	return pid;
}

asmlinkage long sys_getuid(void)
{
	/* Only we change this so SMP safe */
	return current->uid;
}

asmlinkage long sys_geteuid(void)
{
	/* Only we change this so SMP safe */
	return current->euid;
}

asmlinkage long sys_getgid(void)
{
	/* Only we change this so SMP safe */
	return current->gid;
}

asmlinkage long sys_getegid(void)
{
	/* Only we change this so SMP safe */
	return  current->egid;
}

#endif

static void process_timeout(unsigned long __data)
{
	wake_up_process((task_t *)__data);
}

/**
 * schedule_timeout - sleep until timeout
 * @timeout: timeout value in jiffies
 *
 * Make the current task sleep until @timeout jiffies have
 * elapsed. The routine will return immediately unless
 * the current task state has been set (see set_current_state()).
 *
 * You can set the task state as follows -
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout jiffies are guaranteed to
 * pass before the routine returns. The routine will return 0
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task. In this case the remaining time
 * in jiffies will be returned, or 0 if the timer expired in time
 *
 * The current task state is guaranteed to be TASK_RUNNING when this
 * routine returns.
 *
 * Specifying a @timeout value of %MAX_SCHEDULE_TIMEOUT will schedule
 * the CPU away without a bound on the timeout. In this case the return
 * value will be %MAX_SCHEDULE_TIMEOUT.
 *
 * In all cases the return value is guaranteed to be non-negative.
 */
fastcall signed long __sched schedule_timeout(signed long timeout)
{
	struct timer_list timer;
	unsigned long expire;

	switch (timeout)
	{
	case MAX_SCHEDULE_TIMEOUT:
		/*
		 * These two special cases are useful to be comfortable
		 * in the caller. Nothing more. We could take
		 * MAX_SCHEDULE_TIMEOUT from one of the negative value
		 * but I' d like to return a valid offset (>=0) to allow
		 * the caller to do everything it want with the retval.
		 */
		schedule();
		goto out;
	default:
		/*
		 * Another bit of PARANOID. Note that the retval will be
		 * 0 since no piece of kernel is supposed to do a check
		 * for a negative retval of schedule_timeout() (since it
		 * should never happens anyway). You just have the printk()
		 * that will tell you if something is gone wrong and where.
		 */
		if (timeout < 0)
		{
			printk(KERN_ERR "schedule_timeout: wrong timeout "
			       "value %lx from %p\n", timeout,
			       __builtin_return_address(0));
			current->state = TASK_RUNNING;
			goto out;
		}
	}

	expire = timeout + jiffies;

	init_timer(&timer);
	timer.expires = expire;
	timer.data = (unsigned long) current;
	timer.function = process_timeout;

	add_timer(&timer);
	schedule();
	del_singleshot_timer_sync(&timer);

	timeout = expire - jiffies;

 out:
	return timeout < 0 ? 0 : timeout;
}

EXPORT_SYMBOL(schedule_timeout);

/* Thread ID - the internal kernel "pid" */
asmlinkage long sys_gettid(void)
{
	return current->pid;
}

static long __sched nanosleep_restart(struct restart_block *restart)
{
	unsigned long expire = restart->arg0, now = jiffies;
	struct timespec __user *rmtp = (struct timespec __user *) restart->arg1;
	long ret;

	/* Did it expire while we handled signals? */
	if (!time_after(expire, now))
		return 0;

	current->state = TASK_INTERRUPTIBLE;
	expire = schedule_timeout(expire - now);

	ret = 0;
	if (expire) {
		struct timespec t;
		jiffies_to_timespec(expire, &t);

		ret = -ERESTART_RESTARTBLOCK;
		if (rmtp && copy_to_user(rmtp, &t, sizeof(t)))
			ret = -EFAULT;
		/* The 'restart' block is already filled in */
	}
	return ret;
}

/**
 * nanosleep系统调用的服务例程。
 * 将进程挂起直到指定的时间间隔用完。
 */
asmlinkage long sys_nanosleep(struct timespec __user *rqtp, struct timespec __user *rmtp)
{
	struct timespec t;
	unsigned long expire;
	long ret;

	/**
	 * 首先调用copy_frome_user将饮食在timerspec结构中的值复制到局部变量t中。
	 */
	if (copy_from_user(&t, rqtp, sizeof(t)))
		return -EFAULT;

	/**
	 * 假定是一个有效的延迟。
	 */
	if ((t.tv_nsec >= 1000000000L) || (t.tv_nsec < 0) || (t.tv_sec < 0))
		return -EINVAL;

	/**
	 * timespec_to_jiffies将t中的时间间隔转换成节拍数。
	 * 再加上t.tv_sec || t.tv_nsec，是为了保险起见。
	 * 即，计算出来的节拍数始终会被加一。
	 */
	expire = timespec_to_jiffies(&t) + (t.tv_sec || t.tv_nsec);
	/**
	 * schedule_timeout会调用动态定时器。实现进程的延时。
	 */
	current->state = TASK_INTERRUPTIBLE;
	/**
	 * 可能会返回一个剩余节拍数。
	 */
	expire = schedule_timeout(expire);

	ret = 0;
	/**
	 * schedule_timeout返回一个剩余节拍数。可能是系统调用被信号打断了。
	 * 在此自动重启系统调用。
	 */
	if (expire) {
		struct restart_block *restart;
		jiffies_to_timespec(expire, &t);
		if (rmtp && copy_to_user(rmtp, &t, sizeof(t)))
			return -EFAULT;

		restart = &current_thread_info()->restart_block;
		restart->fn = nanosleep_restart;
		restart->arg0 = jiffies + expire;
		restart->arg1 = (unsigned long) rmtp;
		/**
		 * ERESTART_RESTARTBLOCK表示系统调用需要重启。但是重启的方式不是简单的重新执行系统调用。
		 * 而是执行一个指定的函数restart_block。让系统调用再睡眠一段时间。
		 */
		ret = -ERESTART_RESTARTBLOCK;
	}
	return ret;
}

/*
 * sys_sysinfo - fill in sysinfo struct
 */ 
asmlinkage long sys_sysinfo(struct sysinfo __user *info)
{
	struct sysinfo val;
	unsigned long mem_total, sav_total;
	unsigned int mem_unit, bitcount;
	unsigned long seq;

	memset((char *)&val, 0, sizeof(struct sysinfo));

	do {
		struct timespec tp;
		seq = read_seqbegin(&xtime_lock);

		/*
		 * This is annoying.  The below is the same thing
		 * posix_get_clock_monotonic() does, but it wants to
		 * take the lock which we want to cover the loads stuff
		 * too.
		 */

		getnstimeofday(&tp);
		tp.tv_sec += wall_to_monotonic.tv_sec;
		tp.tv_nsec += wall_to_monotonic.tv_nsec;
		if (tp.tv_nsec - NSEC_PER_SEC >= 0) {
			tp.tv_nsec = tp.tv_nsec - NSEC_PER_SEC;
			tp.tv_sec++;
		}
		val.uptime = tp.tv_sec + (tp.tv_nsec ? 1 : 0);

		val.loads[0] = avenrun[0] << (SI_LOAD_SHIFT - FSHIFT);
		val.loads[1] = avenrun[1] << (SI_LOAD_SHIFT - FSHIFT);
		val.loads[2] = avenrun[2] << (SI_LOAD_SHIFT - FSHIFT);

		val.procs = nr_threads;
	} while (read_seqretry(&xtime_lock, seq));

	si_meminfo(&val);
	si_swapinfo(&val);

	/*
	 * If the sum of all the available memory (i.e. ram + swap)
	 * is less than can be stored in a 32 bit unsigned long then
	 * we can be binary compatible with 2.2.x kernels.  If not,
	 * well, in that case 2.2.x was broken anyways...
	 *
	 *  -Erik Andersen <andersee@debian.org>
	 */

	mem_total = val.totalram + val.totalswap;
	if (mem_total < val.totalram || mem_total < val.totalswap)
		goto out;
	bitcount = 0;
	mem_unit = val.mem_unit;
	while (mem_unit > 1) {
		bitcount++;
		mem_unit >>= 1;
		sav_total = mem_total;
		mem_total <<= 1;
		if (mem_total < sav_total)
			goto out;
	}

	/*
	 * If mem_total did not overflow, multiply all memory values by
	 * val.mem_unit and set it to 1.  This leaves things compatible
	 * with 2.2.x, and also retains compatibility with earlier 2.4.x
	 * kernels...
	 */

	val.mem_unit = 1;
	val.totalram <<= bitcount;
	val.freeram <<= bitcount;
	val.sharedram <<= bitcount;
	val.bufferram <<= bitcount;
	val.totalswap <<= bitcount;
	val.freeswap <<= bitcount;
	val.totalhigh <<= bitcount;
	val.freehigh <<= bitcount;

 out:
	if (copy_to_user(info, &val, sizeof(struct sysinfo)))
		return -EFAULT;

	return 0;
}

static void __devinit init_timers_cpu(int cpu)
{
	int j;
	tvec_base_t *base;
       
	base = &per_cpu(tvec_bases, cpu);
	spin_lock_init(&base->lock);
	for (j = 0; j < TVN_SIZE; j++) {
		INIT_LIST_HEAD(base->tv5.vec + j);
		INIT_LIST_HEAD(base->tv4.vec + j);
		INIT_LIST_HEAD(base->tv3.vec + j);
		INIT_LIST_HEAD(base->tv2.vec + j);
	}
	for (j = 0; j < TVR_SIZE; j++)
		INIT_LIST_HEAD(base->tv1.vec + j);

	base->timer_jiffies = jiffies;
}

#ifdef CONFIG_HOTPLUG_CPU
static int migrate_timer_list(tvec_base_t *new_base, struct list_head *head)
{
	struct timer_list *timer;

	while (!list_empty(head)) {
		timer = list_entry(head->next, struct timer_list, entry);
		/* We're locking backwards from __mod_timer order here,
		   beware deadlock. */
		if (!spin_trylock(&timer->lock))
			return 0;
		list_del(&timer->entry);
		internal_add_timer(new_base, timer);
		timer->base = new_base;
		spin_unlock(&timer->lock);
	}
	return 1;
}

static void __devinit migrate_timers(int cpu)
{
	tvec_base_t *old_base;
	tvec_base_t *new_base;
	int i;

	BUG_ON(cpu_online(cpu));
	old_base = &per_cpu(tvec_bases, cpu);
	new_base = &get_cpu_var(tvec_bases);

	local_irq_disable();
again:
	/* Prevent deadlocks via ordering by old_base < new_base. */
	if (old_base < new_base) {
		spin_lock(&new_base->lock);
		spin_lock(&old_base->lock);
	} else {
		spin_lock(&old_base->lock);
		spin_lock(&new_base->lock);
	}

	if (old_base->running_timer)
		BUG();
	for (i = 0; i < TVR_SIZE; i++)
		if (!migrate_timer_list(new_base, old_base->tv1.vec + i))
			goto unlock_again;
	for (i = 0; i < TVN_SIZE; i++)
		if (!migrate_timer_list(new_base, old_base->tv2.vec + i)
		    || !migrate_timer_list(new_base, old_base->tv3.vec + i)
		    || !migrate_timer_list(new_base, old_base->tv4.vec + i)
		    || !migrate_timer_list(new_base, old_base->tv5.vec + i))
			goto unlock_again;
	spin_unlock(&old_base->lock);
	spin_unlock(&new_base->lock);
	local_irq_enable();
	put_cpu_var(tvec_bases);
	return;

unlock_again:
	/* Avoid deadlock with __mod_timer, by backing off. */
	spin_unlock(&old_base->lock);
	spin_unlock(&new_base->lock);
	cpu_relax();
	goto again;
}
#endif /* CONFIG_HOTPLUG_CPU */

static int __devinit timer_cpu_notify(struct notifier_block *self, 
				unsigned long action, void *hcpu)
{
	long cpu = (long)hcpu;
	switch(action) {
	case CPU_UP_PREPARE:
		init_timers_cpu(cpu);
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_DEAD:
		migrate_timers(cpu);
		break;
#endif
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block __devinitdata timers_nb = {
	.notifier_call	= timer_cpu_notify,
};


void __init init_timers(void)
{
	timer_cpu_notify(&timers_nb, (unsigned long)CPU_UP_PREPARE,
				(void *)(long)smp_processor_id());
	register_cpu_notifier(&timers_nb);
	open_softirq(TIMER_SOFTIRQ, run_timer_softirq, NULL);
}

#ifdef CONFIG_TIME_INTERPOLATION

struct time_interpolator *time_interpolator;
static struct time_interpolator *time_interpolator_list;
static DEFINE_SPINLOCK(time_interpolator_lock);

static inline u64 time_interpolator_get_cycles(unsigned int src)
{
	unsigned long (*x)(void);

	switch (src)
	{
		case TIME_SOURCE_FUNCTION:
			x = time_interpolator->addr;
			return x();

		case TIME_SOURCE_MMIO64	:
			return readq((void __iomem *) time_interpolator->addr);

		case TIME_SOURCE_MMIO32	:
			return readl((void __iomem *) time_interpolator->addr);

		default: return get_cycles();
	}
}

static inline u64 time_interpolator_get_counter(void)
{
	unsigned int src = time_interpolator->source;

	if (time_interpolator->jitter)
	{
		u64 lcycle;
		u64 now;

		do {
			lcycle = time_interpolator->last_cycle;
			now = time_interpolator_get_cycles(src);
			if (lcycle && time_after(lcycle, now))
				return lcycle;
			/* Keep track of the last timer value returned. The use of cmpxchg here
			 * will cause contention in an SMP environment.
			 */
		} while (unlikely(cmpxchg(&time_interpolator->last_cycle, lcycle, now) != lcycle));
		return now;
	}
	else
		return time_interpolator_get_cycles(src);
}

void time_interpolator_reset(void)
{
	time_interpolator->offset = 0;
	time_interpolator->last_counter = time_interpolator_get_counter();
}

#define GET_TI_NSECS(count,i) (((((count) - i->last_counter) & (i)->mask) * (i)->nsec_per_cyc) >> (i)->shift)

unsigned long time_interpolator_get_offset(void)
{
	/* If we do not have a time interpolator set up then just return zero */
	if (!time_interpolator)
		return 0;

	return time_interpolator->offset +
		GET_TI_NSECS(time_interpolator_get_counter(), time_interpolator);
}

#define INTERPOLATOR_ADJUST 65536
#define INTERPOLATOR_MAX_SKIP 10*INTERPOLATOR_ADJUST

static void time_interpolator_update(long delta_nsec)
{
	u64 counter;
	unsigned long offset;

	/* If there is no time interpolator set up then do nothing */
	if (!time_interpolator)
		return;

	/* The interpolator compensates for late ticks by accumulating
         * the late time in time_interpolator->offset. A tick earlier than
	 * expected will lead to a reset of the offset and a corresponding
	 * jump of the clock forward. Again this only works if the
	 * interpolator clock is running slightly slower than the regular clock
	 * and the tuning logic insures that.
         */

	counter = time_interpolator_get_counter();
	offset = time_interpolator->offset + GET_TI_NSECS(counter, time_interpolator);

	if (delta_nsec < 0 || (unsigned long) delta_nsec < offset)
		time_interpolator->offset = offset - delta_nsec;
	else {
		time_interpolator->skips++;
		time_interpolator->ns_skipped += delta_nsec - offset;
		time_interpolator->offset = 0;
	}
	time_interpolator->last_counter = counter;

	/* Tuning logic for time interpolator invoked every minute or so.
	 * Decrease interpolator clock speed if no skips occurred and an offset is carried.
	 * Increase interpolator clock speed if we skip too much time.
	 */
	if (jiffies % INTERPOLATOR_ADJUST == 0)
	{
		if (time_interpolator->skips == 0 && time_interpolator->offset > TICK_NSEC)
			time_interpolator->nsec_per_cyc--;
		if (time_interpolator->ns_skipped > INTERPOLATOR_MAX_SKIP && time_interpolator->offset == 0)
			time_interpolator->nsec_per_cyc++;
		time_interpolator->skips = 0;
		time_interpolator->ns_skipped = 0;
	}
}

static inline int
is_better_time_interpolator(struct time_interpolator *new)
{
	if (!time_interpolator)
		return 1;
	return new->frequency > 2*time_interpolator->frequency ||
	    (unsigned long)new->drift < (unsigned long)time_interpolator->drift;
}

void
register_time_interpolator(struct time_interpolator *ti)
{
	unsigned long flags;

	/* Sanity check */
	if (ti->frequency == 0 || ti->mask == 0)
		BUG();

	ti->nsec_per_cyc = ((u64)NSEC_PER_SEC << ti->shift) / ti->frequency;
	spin_lock(&time_interpolator_lock);
	write_seqlock_irqsave(&xtime_lock, flags);
	if (is_better_time_interpolator(ti)) {
		time_interpolator = ti;
		time_interpolator_reset();
	}
	write_sequnlock_irqrestore(&xtime_lock, flags);

	ti->next = time_interpolator_list;
	time_interpolator_list = ti;
	spin_unlock(&time_interpolator_lock);
}

void
unregister_time_interpolator(struct time_interpolator *ti)
{
	struct time_interpolator *curr, **prev;
	unsigned long flags;

	spin_lock(&time_interpolator_lock);
	prev = &time_interpolator_list;
	for (curr = *prev; curr; curr = curr->next) {
		if (curr == ti) {
			*prev = curr->next;
			break;
		}
		prev = &curr->next;
	}

	write_seqlock_irqsave(&xtime_lock, flags);
	if (ti == time_interpolator) {
		/* we lost the best time-interpolator: */
		time_interpolator = NULL;
		/* find the next-best interpolator */
		for (curr = time_interpolator_list; curr; curr = curr->next)
			if (is_better_time_interpolator(curr))
				time_interpolator = curr;
		time_interpolator_reset();
	}
	write_sequnlock_irqrestore(&xtime_lock, flags);
	spin_unlock(&time_interpolator_lock);
}
#endif /* CONFIG_TIME_INTERPOLATION */

/**
 * msleep - sleep safely even with waitqueue interruptions
 * @msecs: Time in milliseconds to sleep for
 */
void msleep(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs) + 1;

	while (timeout) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		timeout = schedule_timeout(timeout);
	}
}

EXPORT_SYMBOL(msleep);

/**
 * msleep_interruptible - sleep waiting for waitqueue interruptions
 * @msecs: Time in milliseconds to sleep for
 */
unsigned long msleep_interruptible(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs) + 1;

	while (timeout && !signal_pending(current)) {
		set_current_state(TASK_INTERRUPTIBLE);
		timeout = schedule_timeout(timeout);
	}
	return jiffies_to_msecs(timeout);
}

EXPORT_SYMBOL(msleep_interruptible);
