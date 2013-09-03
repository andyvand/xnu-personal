/*
 * Copyright (c) 2000-2009 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/* Copyright (c) 1995-2005 Apple Computer, Inc. All Rights Reserved */
/*
 *	pthread_synch.c
 */

#define  _PTHREAD_CONDATTR_T
#define  _PTHREAD_COND_T
#define _PTHREAD_MUTEXATTR_T
#define _PTHREAD_MUTEX_T
#define _PTHREAD_RWLOCKATTR_T
#define _PTHREAD_RWLOCK_T

#undef pthread_mutexattr_t
#undef pthread_mutex_t
#undef pthread_condattr_t
#undef pthread_cond_t
#undef pthread_rwlockattr_t
#undef pthread_rwlock_t

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/resourcevar.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/systm.h>
#include <sys/timeb.h>
#include <sys/times.h>
#include <sys/acct.h>
#include <sys/kernel.h>
#include <sys/wait.h>
#include <sys/signalvar.h>
#include <sys/syslog.h>
#include <sys/stat.h>
#include <sys/lock.h>
#include <sys/kdebug.h>
#include <sys/sysproto.h>
#include <sys/pthread_internal.h>
#include <sys/vm.h>
#include <sys/user.h>		/* for coredump */
#include <sys/proc_info.h>	/* for fill_procworkqueue */


#include <mach/mach_types.h>
#include <mach/vm_prot.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#include <mach/task.h>
#include <kern/kern_types.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <mach/kern_return.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/kalloc.h>
#include <kern/sched_prim.h>	/* for thread_exception_return */
#include <kern/processor.h>
#include <kern/affinity.h>
#include <kern/assert.h>
#include <mach/mach_vm.h>
#include <mach/mach_param.h>
#include <mach/thread_status.h>
#include <mach/thread_policy.h>
#include <mach/message.h>
#include <mach/port.h>
#include <vm/vm_protos.h>
#include <vm/vm_map.h>	/* for current_map() */
#include <vm/vm_fault.h>
#include <mach/thread_act.h> /* for thread_resume */
#include <machine/machine_routines.h>
#if defined(__i386__)
#include <i386/machine_routines.h>
#include <i386/eflags.h>
#include <i386/psl.h>   
#include <i386/seg.h>   
#endif

#include <libkern/OSAtomic.h>

#if 0
#undef KERNEL_DEBUG
#define KERNEL_DEBUG KERNEL_DEBUG_CONSTANT
#undef KERNEL_DEBUG1
#define KERNEL_DEBUG1 KERNEL_DEBUG_CONSTANT1
#endif

lck_grp_attr_t   *pthread_lck_grp_attr;
lck_grp_t    *pthread_lck_grp;
lck_attr_t   *pthread_lck_attr;

extern void thread_set_cthreadself(thread_t thread, uint64_t pself, int isLP64);
extern kern_return_t mach_port_deallocate(ipc_space_t, mach_port_name_t);
extern kern_return_t semaphore_signal_internal_trap(mach_port_name_t);

extern void workqueue_thread_yielded(void);

#if defined(__i386__) || defined(__x86_64__)
extern boolean_t is_useraddr64_canonical(uint64_t addr64);
#endif

static boolean_t workqueue_run_nextreq(proc_t p, struct workqueue *wq, thread_t th, boolean_t force_oc,
					boolean_t  overcommit, int oc_prio, int oc_affinity);

static boolean_t workqueue_run_one(proc_t p, struct workqueue *wq, boolean_t overcommit, int priority);

static void wq_runreq(proc_t p, boolean_t overcommit, uint32_t priority, thread_t th, struct threadlist *tl,
		       int reuse_thread, int wake_thread, int return_directly);

static int setup_wqthread(proc_t p, thread_t th, boolean_t overcommit, uint32_t priority, int reuse_thread, struct threadlist *tl);

static void wq_unpark_continue(void);
static void wq_unsuspend_continue(void);

static boolean_t workqueue_addnewthread(struct workqueue *wq, boolean_t oc_thread);
static void workqueue_removethread(struct threadlist *tl, int fromexit);
static void workqueue_lock_spin(proc_t);
static void workqueue_unlock(proc_t);

int proc_settargetconc(pid_t pid, int queuenum, int32_t targetconc);
int proc_setalltargetconc(pid_t pid, int32_t * targetconcp);

#define WQ_MAXPRI_MIN	0	/* low prio queue num */
#define WQ_MAXPRI_MAX	2	/* max  prio queuenum */
#define WQ_PRI_NUM	3	/* number of prio work queues */

#define C_32_STK_ALIGN          16
#define C_64_STK_ALIGN          16
#define C_64_REDZONE_LEN        128
#define TRUNC_DOWN32(a,c)       ((((uint32_t)a)-(c)) & ((uint32_t)(-(c))))
#define TRUNC_DOWN64(a,c)       ((((uint64_t)a)-(c)) & ((uint64_t)(-(c))))


/* flag values for reuse field in the libc side _pthread_wqthread */
#define	WQ_FLAG_THREAD_PRIOMASK		0x0000ffff
#define	WQ_FLAG_THREAD_OVERCOMMIT	0x00010000	/* thread is with overcommit prio */
#define	WQ_FLAG_THREAD_REUSE		0x00020000	/* thread is being reused */
#define	WQ_FLAG_THREAD_NEWSPI		0x00040000	/* the call is with new SPIs */

/*
 * Flags filed passed to bsdthread_create and back in pthread_start 
31  <---------------------------------> 0
_________________________________________
| flags(8) | policy(8) | importance(16) |
-----------------------------------------
*/
void _pthread_start(pthread_t self, mach_port_t kport, void *(*fun)(void *), void * funarg, size_t stacksize, unsigned int flags);

#define PTHREAD_START_CUSTOM	0x01000000
#define PTHREAD_START_SETSCHED	0x02000000
#define PTHREAD_START_DETACHED	0x04000000
#define PTHREAD_START_POLICY_BITSHIFT 16
#define PTHREAD_START_POLICY_MASK 0xff
#define PTHREAD_START_IMPORTANCE_MASK 0xffff

#define SCHED_OTHER      POLICY_TIMESHARE
#define SCHED_FIFO       POLICY_FIFO
#define SCHED_RR         POLICY_RR



int
bsdthread_create(__unused struct proc *p, struct bsdthread_create_args  *uap, user_addr_t *retval)
{
	kern_return_t kret;
	void * sright;
	int error = 0;
	int allocated = 0;
	mach_vm_offset_t stackaddr;
        mach_vm_size_t th_allocsize = 0;
        mach_vm_size_t user_stacksize;
        mach_vm_size_t th_stacksize;
        mach_vm_offset_t th_stackaddr;
        mach_vm_offset_t th_stack;
        mach_vm_offset_t th_pthread;
        mach_port_name_t th_thport;
	thread_t th;
	user_addr_t user_func = uap->func;
	user_addr_t user_funcarg = uap->func_arg;
	user_addr_t user_stack = uap->stack;
	user_addr_t user_pthread = uap->pthread;
	unsigned int  flags = (unsigned int)uap->flags;
	vm_map_t vmap = current_map();
	task_t ctask = current_task();
	unsigned int policy, importance;
	
	int isLP64 = 0;


	if ((p->p_lflag & P_LREGISTER) == 0)
		return(EINVAL);
#if 0
	KERNEL_DEBUG_CONSTANT(0x9000080 | DBG_FUNC_START, flags, 0, 0, 0, 0);
#endif

	isLP64 = IS_64BIT_PROCESS(p);


#if defined(__i386__) || defined(__x86_64__)
	stackaddr = 0xB0000000;
#else
#error Need to define a stack address hint for this architecture
#endif
	kret = thread_create(ctask, &th);
	if (kret != KERN_SUCCESS)
		return(ENOMEM);
	thread_reference(th);

	sright = (void *) convert_thread_to_port(th);
	th_thport = ipc_port_copyout_send(sright, get_task_ipcspace(ctask));

	if ((flags & PTHREAD_START_CUSTOM) == 0) {
		th_stacksize = (mach_vm_size_t)user_stack;		/* if it is custom them it is stacksize */
		th_allocsize = th_stacksize + PTH_DEFAULT_GUARDSIZE + p->p_pthsize;

		kret = mach_vm_map(vmap, &stackaddr,
    				th_allocsize,
    				page_size-1,
    				VM_MAKE_TAG(VM_MEMORY_STACK)| VM_FLAGS_ANYWHERE , NULL,
    				0, FALSE, VM_PROT_DEFAULT, VM_PROT_ALL,
    				VM_INHERIT_DEFAULT);
    		if (kret != KERN_SUCCESS)
    			kret = mach_vm_allocate(vmap,
    					&stackaddr, th_allocsize,
    					VM_MAKE_TAG(VM_MEMORY_STACK)| VM_FLAGS_ANYWHERE);
    		if (kret != KERN_SUCCESS) {
			error = ENOMEM;
			goto out;
    		}
#if 0
		KERNEL_DEBUG_CONSTANT(0x9000080 |DBG_FUNC_NONE, th_allocsize, stackaddr, 0, 2, 0);
#endif
		th_stackaddr = stackaddr;
		allocated = 1;
     		/*
		 * The guard page is at the lowest address
     		 * The stack base is the highest address
		 */
		kret = mach_vm_protect(vmap,  stackaddr, PTH_DEFAULT_GUARDSIZE, FALSE, VM_PROT_NONE);

    		if (kret != KERN_SUCCESS) { 
			error = ENOMEM;
			goto out1;
    		}
		th_stack = (stackaddr + th_stacksize + PTH_DEFAULT_GUARDSIZE);
		th_pthread = (stackaddr + th_stacksize + PTH_DEFAULT_GUARDSIZE);
		user_stacksize = th_stacksize;
		
	       /*
		* Pre-fault the first page of the new thread's stack and the page that will
		* contain the pthread_t structure.
		*/	
		vm_fault( vmap,
		  vm_map_trunc_page(th_stack - PAGE_SIZE_64),
		  VM_PROT_READ | VM_PROT_WRITE,
		  FALSE, 
		  THREAD_UNINT, NULL, 0);
		
		vm_fault( vmap,
		  vm_map_trunc_page(th_pthread),
		  VM_PROT_READ | VM_PROT_WRITE,
		  FALSE, 
		  THREAD_UNINT, NULL, 0);
	} else {
		th_stack = user_stack;
		user_stacksize = user_stack;
		th_pthread = user_pthread;
#if 0
		KERNEL_DEBUG_CONSTANT(0x9000080 |DBG_FUNC_NONE, 0, 0, 0, 3, 0);
#endif
	}
	
#if defined(__i386__) || defined(__x86_64__)
	{
        /*
         * Set up i386 registers & function call.
         */
	if (isLP64 == 0) {
		x86_thread_state32_t state;
		x86_thread_state32_t *ts = &state;

        	ts->eip = (int)p->p_threadstart;
		ts->eax = (unsigned int)th_pthread;
		ts->ebx = (unsigned int)th_thport;
		ts->ecx = (unsigned int)user_func;
		ts->edx = (unsigned int)user_funcarg;
		ts->edi = (unsigned int)user_stacksize;
		ts->esi = (unsigned int)uap->flags;
		/*
		 * set stack pointer
		 */
        	ts->esp = (int)((vm_offset_t)(th_stack-C_32_STK_ALIGN));

		thread_set_wq_state32(th, (thread_state_t)ts);

	} else {
	        x86_thread_state64_t state64;
		x86_thread_state64_t *ts64 = &state64;

        	ts64->rip = (uint64_t)p->p_threadstart;
		ts64->rdi = (uint64_t)th_pthread;
		ts64->rsi = (uint64_t)(th_thport);
		ts64->rdx = (uint64_t)user_func;
		ts64->rcx = (uint64_t)user_funcarg;
		ts64->r8 = (uint64_t)user_stacksize;
		ts64->r9 = (uint64_t)uap->flags;
		/*
		 * set stack pointer aligned to 16 byte boundary
		 */
		ts64->rsp = (uint64_t)(th_stack - C_64_REDZONE_LEN);

		/* Disallow setting non-canonical PC or stack */
		if (!is_useraddr64_canonical(ts64->rsp) ||
		    !is_useraddr64_canonical(ts64->rip)) {
			error = EINVAL;
			goto out;
		}

		thread_set_wq_state64(th, (thread_state_t)ts64);
	}
	}
#else
#error bsdthread_create  not defined for this architecture
#endif
	/* Set scheduling parameters if needed */
	if ((flags & PTHREAD_START_SETSCHED) != 0) {
		thread_extended_policy_data_t    extinfo;
		thread_precedence_policy_data_t   precedinfo;
#if CONFIG_EMBEDDED
		int ret = 0;
#endif /* CONFIG_EMBEDDED */

		importance = (flags & PTHREAD_START_IMPORTANCE_MASK);
#if CONFIG_EMBEDDED
		/* sets the saved importance for apple ios daemon if backgrounded. else returns 0 */
		ret = proc_setthread_saved_importance(th, importance);
		if (ret == 0) {
#endif /* CONFIG_EMBEDDED */
		policy = (flags >> PTHREAD_START_POLICY_BITSHIFT) & PTHREAD_START_POLICY_MASK;

		if (policy == SCHED_OTHER)
			extinfo.timeshare = 1;
		else
			extinfo.timeshare = 0;
		thread_policy_set(th, THREAD_EXTENDED_POLICY, (thread_policy_t)&extinfo, THREAD_EXTENDED_POLICY_COUNT);

#define BASEPRI_DEFAULT 31
		precedinfo.importance = (importance - BASEPRI_DEFAULT);
		thread_policy_set(th, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&precedinfo, THREAD_PRECEDENCE_POLICY_COUNT);
#if CONFIG_EMBEDDED
		}
#endif /* CONFIG_EMBEDDED */
	}

	kret = thread_resume(th);
	if (kret != KERN_SUCCESS) {
		error = EINVAL;
		goto out1;
	}
	thread_deallocate(th);	/* drop the creator reference */
#if 0
	KERNEL_DEBUG_CONSTANT(0x9000080 |DBG_FUNC_END, error, th_pthread, 0, 0, 0);
#endif
	*retval = th_pthread;

	return(0);

out1:
	if (allocated != 0)
		(void)mach_vm_deallocate(vmap,  stackaddr, th_allocsize);
out:
	(void)mach_port_deallocate(get_task_ipcspace(ctask), th_thport);
	(void)thread_terminate(th);
	(void)thread_deallocate(th);
	return(error);
}

int       
bsdthread_terminate(__unused struct proc *p, struct bsdthread_terminate_args  *uap, __unused int32_t *retval)
{
	mach_vm_offset_t  freeaddr;
	mach_vm_size_t freesize;
	kern_return_t kret;
	mach_port_name_t kthport = (mach_port_name_t)uap->port;
	mach_port_name_t sem = (mach_port_name_t)uap->sem;

	freeaddr = (mach_vm_offset_t)uap->stackaddr;
	freesize = uap->freesize;
	
#if 0
	KERNEL_DEBUG_CONSTANT(0x9000084 |DBG_FUNC_START, freeaddr, freesize, kthport, 0xff, 0);
#endif
	if ((freesize != (mach_vm_size_t)0) && (freeaddr != (mach_vm_offset_t)0)) {
		kret = mach_vm_deallocate(current_map(), freeaddr, freesize);
		if (kret != KERN_SUCCESS) {
			return(EINVAL);
		}
	}
	
	(void) thread_terminate(current_thread());
	if (sem != MACH_PORT_NULL) {
		 kret = semaphore_signal_internal_trap(sem);
		if (kret != KERN_SUCCESS) {
			return(EINVAL);
		}
	}
	
	if (kthport != MACH_PORT_NULL)
			mach_port_deallocate(get_task_ipcspace(current_task()), kthport);
	thread_exception_return();
	panic("bsdthread_terminate: still running\n");
#if 0
	KERNEL_DEBUG_CONSTANT(0x9000084 |DBG_FUNC_END, 0, 0, 0, 0xff, 0);
#endif
	return(0);
}


int
bsdthread_register(struct proc *p, struct bsdthread_register_args  *uap, __unused int32_t *retval)
{
	/* prevent multiple registrations */
	if ((p->p_lflag & P_LREGISTER) != 0)
		return(EINVAL);
	/* syscall randomizer test can pass bogus values */
	if (uap->pthsize > MAX_PTHREAD_SIZE) {
		return(EINVAL);
	}
	p->p_threadstart = uap->threadstart;
	p->p_wqthread = uap->wqthread;
	p->p_pthsize = uap->pthsize;
	p->p_targconc = uap->targetconc_ptr;
	p->p_dispatchqueue_offset = uap->dispatchqueue_offset;
	proc_setregister(p);

	return(0);
}

uint32_t wq_yielded_threshold		= WQ_YIELDED_THRESHOLD;
uint32_t wq_yielded_window_usecs	= WQ_YIELDED_WINDOW_USECS;
uint32_t wq_stalled_window_usecs	= WQ_STALLED_WINDOW_USECS;
uint32_t wq_reduce_pool_window_usecs	= WQ_REDUCE_POOL_WINDOW_USECS;
uint32_t wq_max_timer_interval_usecs	= WQ_MAX_TIMER_INTERVAL_USECS;
uint32_t wq_max_threads			= WORKQUEUE_MAXTHREADS;
uint32_t wq_max_constrained_threads	= WORKQUEUE_MAXTHREADS / 8;


SYSCTL_INT(_kern, OID_AUTO, wq_yielded_threshold, CTLFLAG_RW | CTLFLAG_LOCKED,
	   &wq_yielded_threshold, 0, "");

SYSCTL_INT(_kern, OID_AUTO, wq_yielded_window_usecs, CTLFLAG_RW | CTLFLAG_LOCKED,
	   &wq_yielded_window_usecs, 0, "");

SYSCTL_INT(_kern, OID_AUTO, wq_stalled_window_usecs, CTLFLAG_RW | CTLFLAG_LOCKED,
	   &wq_stalled_window_usecs, 0, "");

SYSCTL_INT(_kern, OID_AUTO, wq_reduce_pool_window_usecs, CTLFLAG_RW | CTLFLAG_LOCKED,
	   &wq_reduce_pool_window_usecs, 0, "");

SYSCTL_INT(_kern, OID_AUTO, wq_max_timer_interval_usecs, CTLFLAG_RW | CTLFLAG_LOCKED,
	   &wq_max_timer_interval_usecs, 0, "");

SYSCTL_INT(_kern, OID_AUTO, wq_max_threads, CTLFLAG_RW | CTLFLAG_LOCKED,
	   &wq_max_threads, 0, "");

SYSCTL_INT(_kern, OID_AUTO, wq_max_constrained_threads, CTLFLAG_RW | CTLFLAG_LOCKED,
	   &wq_max_constrained_threads, 0, "");


static uint32_t wq_init_constrained_limit = 1;


void
workqueue_init_lock(proc_t p)
{
        lck_spin_init(&p->p_wqlock, pthread_lck_grp, pthread_lck_attr);

	p->p_wqiniting = FALSE;
}

void
workqueue_destroy_lock(proc_t p)
{
	lck_spin_destroy(&p->p_wqlock, pthread_lck_grp);
}


static void
workqueue_lock_spin(proc_t p)
{
	lck_spin_lock(&p->p_wqlock);
}

static void
workqueue_unlock(proc_t p)
{
	lck_spin_unlock(&p->p_wqlock);
}


static void
workqueue_interval_timer_start(struct workqueue *wq)
{
        uint64_t deadline;

	if (wq->wq_timer_interval == 0)
		wq->wq_timer_interval = wq_stalled_window_usecs;
	else {
		wq->wq_timer_interval = wq->wq_timer_interval * 2;

		if (wq->wq_timer_interval > wq_max_timer_interval_usecs)
			wq->wq_timer_interval = wq_max_timer_interval_usecs;
	}
	clock_interval_to_deadline(wq->wq_timer_interval, 1000, &deadline);

	thread_call_enter_delayed(wq->wq_atimer_call, deadline);

	KERNEL_DEBUG(0xefffd110, wq, wq->wq_reqcount, wq->wq_flags, wq->wq_timer_interval, 0);
}


static boolean_t
wq_thread_is_busy(uint64_t cur_ts, uint64_t *lastblocked_tsp)
{	clock_sec_t	secs;
	clock_usec_t	usecs;
	uint64_t lastblocked_ts;
	uint64_t elapsed;

	/*
	 * the timestamp is updated atomically w/o holding the workqueue lock
	 * so we need to do an atomic read of the 64 bits so that we don't see
	 * a mismatched pair of 32 bit reads... we accomplish this in an architecturally
	 * independent fashion by using OSCompareAndSwap64 to write back the
	 * value we grabbed... if it succeeds, then we have a good timestamp to
	 * evaluate... if it fails, we straddled grabbing the timestamp while it
	 * was being updated... treat a failed update as a busy thread since
	 * it implies we are about to see a really fresh timestamp anyway
	 */
	lastblocked_ts = *lastblocked_tsp;

	if ( !OSCompareAndSwap64((UInt64)lastblocked_ts, (UInt64)lastblocked_ts, lastblocked_tsp))
		return (TRUE);

	if (lastblocked_ts >= cur_ts) {
		/*
		 * because the update of the timestamp when a thread blocks isn't
		 * serialized against us looking at it (i.e. we don't hold the workq lock)
		 * it's possible to have a timestamp that matches the current time or
		 * that even looks to be in the future relative to when we grabbed the current
		 * time... just treat this as a busy thread since it must have just blocked.
		 */
		return (TRUE);
	}
	elapsed = cur_ts - lastblocked_ts;

	absolutetime_to_microtime(elapsed, &secs, &usecs);

	if (secs == 0 && usecs < wq_stalled_window_usecs)
		return (TRUE);
	return (FALSE);
}


#define WQ_TIMER_NEEDED(wq, start_timer) do {		\
	int oldflags = wq->wq_flags;			\
							\
	if ( !(oldflags & (WQ_EXITING | WQ_ATIMER_RUNNING))) {	\
		if (OSCompareAndSwap(oldflags, oldflags | WQ_ATIMER_RUNNING, (UInt32 *)&wq->wq_flags)) \
			start_timer = TRUE;			\
	}							\
} while (0)



static void
workqueue_add_timer(struct workqueue *wq, __unused int param1)
{
	proc_t		p;
	boolean_t	start_timer = FALSE;
	boolean_t	retval;
	boolean_t	add_thread;
	uint32_t	busycount;
		
        KERNEL_DEBUG(0xefffd108 | DBG_FUNC_START, wq, wq->wq_flags, wq->wq_nthreads, wq->wq_thidlecount, 0);

	p = wq->wq_proc;

	workqueue_lock_spin(p);

	/*
	 * because workqueue_callback now runs w/o taking the workqueue lock
	 * we are unsynchronized w/r to a change in state of the running threads...
	 * to make sure we always evaluate that change, we allow it to start up 
	 * a new timer if the current one is actively evalutating the state
	 * however, we do not need more than 2 timers fired up (1 active and 1 pending)
	 * and we certainly do not want 2 active timers evaluating the state
	 * simultaneously... so use WQL_ATIMER_BUSY to serialize the timers...
	 * note that WQL_ATIMER_BUSY is in a different flag word from WQ_ATIMER_RUNNING since
	 * it is always protected by the workq lock... WQ_ATIMER_RUNNING is evaluated
	 * and set atomimcally since the callback function needs to manipulate it
	 * w/o holding the workq lock...
	 *
	 * !WQ_ATIMER_RUNNING && !WQL_ATIMER_BUSY   ==   no pending timer, no active timer
	 * !WQ_ATIMER_RUNNING && WQL_ATIMER_BUSY    ==   no pending timer, 1 active timer
	 * WQ_ATIMER_RUNNING && !WQL_ATIMER_BUSY    ==   1 pending timer, no active timer
	 * WQ_ATIMER_RUNNING && WQL_ATIMER_BUSY     ==   1 pending timer, 1 active timer
	 */
	while (wq->wq_lflags & WQL_ATIMER_BUSY) {
		wq->wq_lflags |= WQL_ATIMER_WAITING;

		assert_wait((caddr_t)wq, (THREAD_UNINT));
		workqueue_unlock(p);

		thread_block(THREAD_CONTINUE_NULL);

		workqueue_lock_spin(p);
	}
	wq->wq_lflags |= WQL_ATIMER_BUSY;

	/*
	 * the workq lock will protect us from seeing WQ_EXITING change state, but we
	 * still need to update this atomically in case someone else tries to start
	 * the timer just as we're releasing it
	 */
	while ( !(OSCompareAndSwap(wq->wq_flags, (wq->wq_flags & ~WQ_ATIMER_RUNNING), (UInt32 *)&wq->wq_flags)));

again:
	retval = TRUE;
	add_thread = FALSE;

	if ( !(wq->wq_flags & WQ_EXITING)) {
		/*
		 * check to see if the stall frequency was beyond our tolerance
		 * or we have work on the queue, but haven't scheduled any 
		 * new work within our acceptable time interval because
		 * there were no idle threads left to schedule
		 */
		if (wq->wq_reqcount) {
			uint32_t	priority;
			uint32_t	affinity_tag;
			uint32_t	i;
			uint64_t	curtime;

			for (priority = 0; priority < WORKQUEUE_NUMPRIOS; priority++) {
				if (wq->wq_requests[priority])
					break;
			}
			assert(priority < WORKQUEUE_NUMPRIOS);

			curtime = mach_absolute_time();
			busycount = 0;

			for (affinity_tag = 0; affinity_tag < wq->wq_reqconc[priority]; affinity_tag++) {
				/*
				 * if we have no idle threads, we can try to add them if needed
				 */
				if (wq->wq_thidlecount == 0)
					add_thread = TRUE;

				/*
				 * look for first affinity group that is currently not active
				 * i.e. no active threads at this priority level or higher
				 * and has not been active recently at this priority level or higher
				 */
				for (i = 0; i <= priority; i++) {
					if (wq->wq_thactive_count[i][affinity_tag]) {
						add_thread = FALSE;
						break;
					}
					if (wq->wq_thscheduled_count[i][affinity_tag]) {
						if (wq_thread_is_busy(curtime, &wq->wq_lastblocked_ts[i][affinity_tag])) {
							add_thread = FALSE;
							busycount++;
							break;
						}
					}
				}
				if (add_thread == TRUE) {
					retval = workqueue_addnewthread(wq, FALSE);
					break;
				}
			}
			if (wq->wq_reqcount) {
				/*
				 * as long as we have threads to schedule, and we successfully
				 * scheduled new work, keep trying
				 */
				while (wq->wq_thidlecount && !(wq->wq_flags & WQ_EXITING)) {
					/*
					 * workqueue_run_nextreq is responsible for
					 * dropping the workqueue lock in all cases
					 */
					retval = workqueue_run_nextreq(p, wq, THREAD_NULL, FALSE, FALSE, 0, 0);
					workqueue_lock_spin(p);

					if (retval == FALSE)
						break;
				}
				if ( !(wq->wq_flags & WQ_EXITING) && wq->wq_reqcount) {

					if (wq->wq_thidlecount == 0 && retval == TRUE && add_thread == TRUE)
						goto again;

					if (wq->wq_thidlecount == 0 || busycount)
						WQ_TIMER_NEEDED(wq, start_timer);

					KERNEL_DEBUG(0xefffd108 | DBG_FUNC_NONE, wq, wq->wq_reqcount, wq->wq_thidlecount, busycount, 0);
				}
			}
		}
	}
	if ( !(wq->wq_flags & WQ_ATIMER_RUNNING))
		wq->wq_timer_interval = 0;

	wq->wq_lflags &= ~WQL_ATIMER_BUSY;

	if ((wq->wq_flags & WQ_EXITING) || (wq->wq_lflags & WQL_ATIMER_WAITING)) {
		/*
		 * wakeup the thread hung up in workqueue_exit or workqueue_add_timer waiting for this timer
		 * to finish getting out of the way
		 */
		wq->wq_lflags &= ~WQL_ATIMER_WAITING;
		wakeup(wq);
	}
        KERNEL_DEBUG(0xefffd108 | DBG_FUNC_END, wq, start_timer, wq->wq_nthreads, wq->wq_thidlecount, 0);

	workqueue_unlock(p);

        if (start_timer == TRUE)
	        workqueue_interval_timer_start(wq);
}


void
workqueue_thread_yielded(void)
{
	struct workqueue *wq;
	proc_t		p;

	p = current_proc();

	if ((wq = p->p_wqptr) == NULL || wq->wq_reqcount == 0)
		return;
	
	workqueue_lock_spin(p);

	if (wq->wq_reqcount) {
		uint64_t	curtime;
		uint64_t	elapsed;
		clock_sec_t	secs;
		clock_usec_t	usecs;

		if (wq->wq_thread_yielded_count++ == 0)
			wq->wq_thread_yielded_timestamp = mach_absolute_time();

		if (wq->wq_thread_yielded_count < wq_yielded_threshold) {
			workqueue_unlock(p);
			return;
		}
		KERNEL_DEBUG(0xefffd138 | DBG_FUNC_START, wq, wq->wq_thread_yielded_count, wq->wq_reqcount, 0, 0);

		wq->wq_thread_yielded_count = 0;

		curtime = mach_absolute_time();
		elapsed = curtime - wq->wq_thread_yielded_timestamp;
		absolutetime_to_microtime(elapsed, &secs, &usecs);

		if (secs == 0 && usecs < wq_yielded_window_usecs) {

			if (wq->wq_thidlecount == 0) {
				workqueue_addnewthread(wq, TRUE);
				/*
				 * 'workqueue_addnewthread' drops the workqueue lock
				 * when creating the new thread and then retakes it before
				 * returning... this window allows other threads to process
				 * requests, so we need to recheck for available work
				 * if none found, we just return...  the newly created thread
				 * will eventually get used (if it hasn't already)...
				 */
				if (wq->wq_reqcount == 0) {
					workqueue_unlock(p);
					return;
				}
			}
			if (wq->wq_thidlecount) {
				uint32_t	priority;
				uint32_t	affinity = -1;
				boolean_t	overcommit = FALSE;
				boolean_t	force_oc = FALSE;
				struct uthread    *uth;
				struct threadlist *tl;

				uth = get_bsdthread_info(current_thread());
				if ((tl = uth->uu_threadlist))
					affinity = tl->th_affinity_tag;

				for (priority = 0; priority < WORKQUEUE_NUMPRIOS; priority++) {
					if (wq->wq_requests[priority])
						break;
				}
				assert(priority < WORKQUEUE_NUMPRIOS);

				wq->wq_reqcount--;
				wq->wq_requests[priority]--;

				if (wq->wq_ocrequests[priority]) {
					wq->wq_ocrequests[priority]--;
					overcommit = TRUE;
				} else
					force_oc = TRUE;

				(void)workqueue_run_nextreq(p, wq, THREAD_NULL, force_oc, overcommit, priority, affinity);
				/*
				 * workqueue_run_nextreq is responsible for
				 * dropping the workqueue lock in all cases
				 */
				KERNEL_DEBUG(0xefffd138 | DBG_FUNC_END, wq, wq->wq_thread_yielded_count, wq->wq_reqcount, 1, 0);

				return;
			}
		}
		KERNEL_DEBUG(0xefffd138 | DBG_FUNC_END, wq, wq->wq_thread_yielded_count, wq->wq_reqcount, 2, 0);
	}
	workqueue_unlock(p);
}



static void
workqueue_callback(int type, thread_t thread)
{
	struct uthread    *uth;
	struct threadlist *tl;
	struct workqueue  *wq;

	uth = get_bsdthread_info(thread);
	tl = uth->uu_threadlist;
	wq = tl->th_workq;

        switch (type) {

	      case SCHED_CALL_BLOCK:
	        {
		uint32_t	old_activecount;

		old_activecount = OSAddAtomic(-1, &wq->wq_thactive_count[tl->th_priority][tl->th_affinity_tag]);

		if (old_activecount == 1) {
			boolean_t	start_timer = FALSE;
			uint64_t	curtime;
			UInt64		*lastblocked_ptr;

		        /*
			 * we were the last active thread on this affinity set
			 * and we've got work to do
			 */
			lastblocked_ptr = (UInt64 *)&wq->wq_lastblocked_ts[tl->th_priority][tl->th_affinity_tag];
			curtime = mach_absolute_time();

			/*
			 * if we collide with another thread trying to update the last_blocked (really unlikely
			 * since another thread would have to get scheduled and then block after we start down 
			 * this path), it's not a problem.  Either timestamp is adequate, so no need to retry
			 */

			OSCompareAndSwap64(*lastblocked_ptr, (UInt64)curtime, lastblocked_ptr);

			if (wq->wq_reqcount)
				WQ_TIMER_NEEDED(wq, start_timer);

			if (start_timer == TRUE)
				workqueue_interval_timer_start(wq);
		}
	        KERNEL_DEBUG1(0xefffd020 | DBG_FUNC_START, wq, old_activecount, tl->th_priority, tl->th_affinity_tag, thread_tid(thread));
	        }
		break;

	      case SCHED_CALL_UNBLOCK:
		/*
		 * we cannot take the workqueue_lock here...
		 * an UNBLOCK can occur from a timer event which
		 * is run from an interrupt context... if the workqueue_lock
		 * is already held by this processor, we'll deadlock...
		 * the thread lock for the thread being UNBLOCKED
		 * is also held
		 */
		 OSAddAtomic(1, &wq->wq_thactive_count[tl->th_priority][tl->th_affinity_tag]);

		 KERNEL_DEBUG1(0xefffd020 | DBG_FUNC_END, wq, wq->wq_threads_scheduled, tl->th_priority, tl->th_affinity_tag, thread_tid(thread));

		 break;
	}
}


static void
workqueue_removethread(struct threadlist *tl, int fromexit)
{
	struct workqueue *wq;
	struct uthread * uth;

	/* 
	 * If fromexit is set, the call is from workqueue_exit(,
	 * so some cleanups are to be avoided.
	 */
	wq = tl->th_workq;

	TAILQ_REMOVE(&wq->wq_thidlelist, tl, th_entry);

	if (fromexit == 0) {
		wq->wq_nthreads--;
		wq->wq_thidlecount--;
	}

	/*
	 * Clear the threadlist pointer in uthread so 
	 * blocked thread on wakeup for termination will
	 * not access the thread list as it is going to be
	 * freed.
	 */
	thread_sched_call(tl->th_thread, NULL);

	uth = get_bsdthread_info(tl->th_thread);
	if (uth != (struct uthread *)0) {
		uth->uu_threadlist = NULL;
	}
	if (fromexit == 0) {
		/* during exit the lock is not held */
		workqueue_unlock(wq->wq_proc);
	}

	if ( (tl->th_flags & TH_LIST_SUSPENDED) ) {
		/*
		 * thread was created, but never used... 
		 * need to clean up the stack and port ourselves
		 * since we're not going to spin up through the
		 * normal exit path triggered from Libc
		 */
		if (fromexit == 0) {
			/* vm map is already deallocated when this is called from exit */
			(void)mach_vm_deallocate(wq->wq_map, tl->th_stackaddr, tl->th_allocsize);
		}
		(void)mach_port_deallocate(get_task_ipcspace(wq->wq_task), tl->th_thport);

	        KERNEL_DEBUG1(0xefffd014 | DBG_FUNC_END, wq, (uintptr_t)thread_tid(current_thread()), wq->wq_nthreads, 0xdead, thread_tid(tl->th_thread));
	} else {

		KERNEL_DEBUG1(0xefffd018 | DBG_FUNC_END, wq, (uintptr_t)thread_tid(current_thread()), wq->wq_nthreads, 0xdead, thread_tid(tl->th_thread));
	}
	/*
	 * drop our ref on the thread
	 */
	thread_deallocate(tl->th_thread);

	kfree(tl, sizeof(struct threadlist));
}


/*
 * called with workq lock held
 * dropped and retaken around thread creation
 * return with workq lock held
 */
static boolean_t
workqueue_addnewthread(struct workqueue *wq, boolean_t oc_thread)
{
	struct threadlist *tl;
	struct uthread	*uth;
	kern_return_t	kret;
	thread_t	th;
	proc_t		p;
	void 	 	*sright;
	mach_vm_offset_t stackaddr;

	if ((wq->wq_flags & WQ_EXITING) == WQ_EXITING)
		return (FALSE);

	if (wq->wq_nthreads >= wq_max_threads || wq->wq_nthreads >= (CONFIG_THREAD_MAX - 20)) {
		wq->wq_lflags |= WQL_EXCEEDED_TOTAL_THREAD_LIMIT;
		return (FALSE);
	}
	wq->wq_lflags &= ~WQL_EXCEEDED_TOTAL_THREAD_LIMIT;

	if (oc_thread == FALSE && wq->wq_constrained_threads_scheduled >= wq_max_constrained_threads) {
		/*
		 * if we're not creating this thread to service an overcommit request,
		 * then check the size of the constrained thread pool...  if we've already
		 * reached our max for threads scheduled from this pool, don't create a new
		 * one... the callers of this function are prepared for failure.
		 */
		wq->wq_lflags |= WQL_EXCEEDED_CONSTRAINED_THREAD_LIMIT;
		return (FALSE);
	}
	if (wq->wq_constrained_threads_scheduled < wq_max_constrained_threads)
		wq->wq_lflags &= ~WQL_EXCEEDED_CONSTRAINED_THREAD_LIMIT;

	wq->wq_nthreads++;

	p = wq->wq_proc;
	workqueue_unlock(p);

	kret = thread_create_workq(wq->wq_task, (thread_continue_t)wq_unsuspend_continue, &th);

 	if (kret != KERN_SUCCESS)
		goto failed;

	tl = kalloc(sizeof(struct threadlist));
	bzero(tl, sizeof(struct threadlist));

#if defined(__i386__) || defined(__x86_64__)
	stackaddr = 0xB0000000;
#else
#error Need to define a stack address hint for this architecture
#endif
	tl->th_allocsize = PTH_DEFAULT_STACKSIZE + PTH_DEFAULT_GUARDSIZE + p->p_pthsize;

	kret = mach_vm_map(wq->wq_map, &stackaddr,
    			tl->th_allocsize,
    			page_size-1,
    			VM_MAKE_TAG(VM_MEMORY_STACK)| VM_FLAGS_ANYWHERE , NULL,
    			0, FALSE, VM_PROT_DEFAULT, VM_PROT_ALL,
    			VM_INHERIT_DEFAULT);

	if (kret != KERN_SUCCESS) {
	        kret = mach_vm_allocate(wq->wq_map,
    					&stackaddr, tl->th_allocsize,
    					VM_MAKE_TAG(VM_MEMORY_STACK) | VM_FLAGS_ANYWHERE);
	}
	if (kret == KERN_SUCCESS) {
	        /*
		 * The guard page is at the lowest address
		 * The stack base is the highest address
		 */
	        kret = mach_vm_protect(wq->wq_map, stackaddr, PTH_DEFAULT_GUARDSIZE, FALSE, VM_PROT_NONE);

		if (kret != KERN_SUCCESS)
		        (void) mach_vm_deallocate(wq->wq_map, stackaddr, tl->th_allocsize);
	}
	if (kret != KERN_SUCCESS) {
		(void) thread_terminate(th);
		thread_deallocate(th);

		kfree(tl, sizeof(struct threadlist));
		goto failed;
	}
	thread_reference(th);

	sright = (void *) convert_thread_to_port(th);
	tl->th_thport = ipc_port_copyout_send(sright, get_task_ipcspace(wq->wq_task));

	thread_static_param(th, TRUE);

	tl->th_flags = TH_LIST_INITED | TH_LIST_SUSPENDED;

	tl->th_thread = th;
	tl->th_workq = wq;
	tl->th_stackaddr = stackaddr;
	tl->th_affinity_tag = -1;
	tl->th_priority = WORKQUEUE_NUMPRIOS;
	tl->th_policy = -1;

	uth = get_bsdthread_info(tl->th_thread);

        workqueue_lock_spin(p);
	
	uth->uu_threadlist = (void *)tl;
	TAILQ_INSERT_TAIL(&wq->wq_thidlelist, tl, th_entry);

	wq->wq_thidlecount++;

	KERNEL_DEBUG1(0xefffd014 | DBG_FUNC_START, wq, wq->wq_nthreads, 0, thread_tid(current_thread()), thread_tid(tl->th_thread));

	return (TRUE);

failed:
	workqueue_lock_spin(p);
	wq->wq_nthreads--;

	return (FALSE);
}


int
workq_open(struct proc *p, __unused struct workq_open_args  *uap, __unused int32_t *retval)
{
	struct workqueue * wq;
	int wq_size;
	char * ptr;
	char * nptr;
	uint32_t i;
	uint32_t num_cpus;
	int error = 0;
	boolean_t need_wakeup = FALSE;


	if ((p->p_lflag & P_LREGISTER) == 0)
		return(EINVAL);

	num_cpus = ml_get_max_cpus();

	if (wq_init_constrained_limit) {
		uint32_t limit;
		/*
		 * set up the limit for the constrained pool
		 * this is a virtual pool in that we don't
		 * maintain it on a separate idle and run list
		 */
		limit = num_cpus * (WORKQUEUE_NUMPRIOS + 1);

		if (limit > wq_max_constrained_threads)
			wq_max_constrained_threads = limit;

		wq_init_constrained_limit = 0;
	}
	workqueue_lock_spin(p);

	if (p->p_wqptr == NULL) {

		while (p->p_wqiniting == TRUE) {

			assert_wait((caddr_t)&p->p_wqiniting, THREAD_UNINT);
			workqueue_unlock(p);

			thread_block(THREAD_CONTINUE_NULL);

			workqueue_lock_spin(p);
		}
		if (p->p_wqptr != NULL)
			goto out;

		p->p_wqiniting = TRUE;

		workqueue_unlock(p);

		wq_size = sizeof(struct workqueue) +
			(num_cpus * WORKQUEUE_NUMPRIOS * sizeof(uint16_t)) +
			(num_cpus * WORKQUEUE_NUMPRIOS * sizeof(uint32_t)) +
			(num_cpus * WORKQUEUE_NUMPRIOS * sizeof(uint64_t)) +
			sizeof(uint32_t) + sizeof(uint64_t);

		ptr = (char *)kalloc(wq_size);
		bzero(ptr, wq_size);

		wq = (struct workqueue *)ptr;
		wq->wq_flags = WQ_LIST_INITED;
		wq->wq_proc = p;
		wq->wq_affinity_max = num_cpus;
		wq->wq_task = current_task();
		wq->wq_map  = current_map();

		for (i = 0; i < WORKQUEUE_NUMPRIOS; i++)
			wq->wq_reqconc[i] = wq->wq_affinity_max;

		nptr = ptr + sizeof(struct workqueue);

		for (i = 0; i < WORKQUEUE_NUMPRIOS; i++) {
			wq->wq_thscheduled_count[i] = (uint16_t *)nptr;
			nptr += (num_cpus * sizeof(uint16_t));
		}
		nptr += (sizeof(uint32_t) - 1);
		nptr = (char *)((uintptr_t)nptr & ~(sizeof(uint32_t) - 1));

		for (i = 0; i < WORKQUEUE_NUMPRIOS; i++) {
			wq->wq_thactive_count[i] = (uint32_t *)nptr;
			nptr += (num_cpus * sizeof(uint32_t));
		}
		/*
		 * align nptr on a 64 bit boundary so that we can do nice
		 * atomic64 operations on the timestamps...
		 * note that we requested an extra uint64_t when calcuating
		 * the size for the allocation of the workqueue struct
		 */
		nptr += (sizeof(uint64_t) - 1);
		nptr = (char *)((uintptr_t)nptr & ~(sizeof(uint64_t) - 1));

		for (i = 0; i < WORKQUEUE_NUMPRIOS; i++) {
			wq->wq_lastblocked_ts[i] = (uint64_t *)nptr;
			nptr += (num_cpus * sizeof(uint64_t));
		}
		TAILQ_INIT(&wq->wq_thrunlist);
		TAILQ_INIT(&wq->wq_thidlelist);

		wq->wq_atimer_call = thread_call_allocate((thread_call_func_t)workqueue_add_timer, (thread_call_param_t)wq);

		workqueue_lock_spin(p);

		p->p_wqptr = (void *)wq;
		p->p_wqsize = wq_size;

		p->p_wqiniting = FALSE;
		need_wakeup = TRUE;
	}
out:
	workqueue_unlock(p);

	if (need_wakeup == TRUE)
		wakeup(&p->p_wqiniting);
	return(error);
}


int
workq_kernreturn(struct proc *p, struct workq_kernreturn_args  *uap, __unused int32_t *retval)
{
        struct workqueue *wq;
	int error	= 0;

	if ((p->p_lflag & P_LREGISTER) == 0)
		return(EINVAL);

	switch (uap->options) {

		case WQOPS_QUEUE_NEWSPISUPP:
			break;

		case WQOPS_QUEUE_REQTHREADS: {
			/*
			 * for this operation, we re-purpose the affinity
			 * argument as the number of threads to start
			 */
			boolean_t overcommit = FALSE;
			int priority	     = uap->prio;
			int reqcount	     = uap->affinity;

			if (priority & WORKQUEUE_OVERCOMMIT) {
				priority &= ~WORKQUEUE_OVERCOMMIT;
				overcommit = TRUE;
			}
			if ((reqcount <= 0) || (priority < 0) || (priority >= WORKQUEUE_NUMPRIOS)) {
				error = EINVAL;
				break;
			}
                        workqueue_lock_spin(p);

                        if ((wq = (struct workqueue *)p->p_wqptr) == NULL) {
                                workqueue_unlock(p);

				error = EINVAL;
				break;
                        }
			if (overcommit == FALSE) {
				wq->wq_reqcount += reqcount;
				wq->wq_requests[priority] += reqcount;
				
				KERNEL_DEBUG(0xefffd008 | DBG_FUNC_NONE, wq, priority, wq->wq_requests[priority], reqcount, 0);

				while (wq->wq_reqcount) {
					if (workqueue_run_one(p, wq, overcommit, priority) == FALSE)
						break;
				}
			} else {
				KERNEL_DEBUG(0xefffd13c | DBG_FUNC_NONE, wq, priority, wq->wq_requests[priority], reqcount, 0);

				while (reqcount) {
					if (workqueue_run_one(p, wq, overcommit, priority) == FALSE)
						break;
					reqcount--;
				}
				if (reqcount) {
					/*
					 * we need to delay starting some of the overcommit requests...
					 * we should only fail to create the overcommit threads if
					 * we're at the max thread limit... as existing threads
					 * return to the kernel, we'll notice the ocrequests
					 * and spin them back to user space as the overcommit variety
					 */
					wq->wq_reqcount += reqcount;
					wq->wq_requests[priority] += reqcount;
					wq->wq_ocrequests[priority] += reqcount;

					KERNEL_DEBUG(0xefffd140 | DBG_FUNC_NONE, wq, priority, wq->wq_requests[priority], reqcount, 0);
				}
			}
			workqueue_unlock(p);

		        }
			break;

		case WQOPS_THREAD_RETURN: {
		        thread_t th = current_thread();
		        struct uthread *uth = get_bsdthread_info(th);

			/* reset signal mask on the workqueue thread to default state */
			if (uth->uu_sigmask != (sigset_t)(~workq_threadmask)) {
				proc_lock(p);
				uth->uu_sigmask = ~workq_threadmask;
				proc_unlock(p);
			}
			workqueue_lock_spin(p);

			if ((wq = (struct workqueue *)p->p_wqptr) == NULL || (uth->uu_threadlist == NULL)) {
			        workqueue_unlock(p);

				error = EINVAL;
				break;
			}
		        KERNEL_DEBUG(0xefffd004 | DBG_FUNC_END, wq, 0, 0, 0, 0);

			(void)workqueue_run_nextreq(p, wq, th, FALSE, FALSE, 0, -1);
			/*
			 * workqueue_run_nextreq is responsible for
			 * dropping the workqueue lock in all cases
			 */
		        }
			break;
		
		default:
			error = EINVAL;
			break;
	}
	return (error);
}

/*
 * Routine:	workqueue_mark_exiting
 *
 * Function:	Mark the work queue such that new threads will not be added to the
 *		work queue after we return.  
 *
 * Conditions:	Called against the current process.
 */
void
workqueue_mark_exiting(struct proc *p)
{
	struct workqueue  * wq;

	wq = p->p_wqptr;
	if (wq != NULL) {

		KERNEL_DEBUG(0x9008088 | DBG_FUNC_START, p->p_wqptr, 0, 0, 0, 0);

	        workqueue_lock_spin(p);

		/*
		 * we now arm the timer in the callback function w/o holding the workq lock...
		 * we do this by setting  WQ_ATIMER_RUNNING via OSCompareAndSwap in order to 
		 * insure only a single timer if running and to notice that WQ_EXITING has
		 * been set (we don't want to start a timer once WQ_EXITING is posted)
		 *
		 * so once we have successfully set WQ_EXITING, we cannot fire up a new timer...
		 * therefor no need to clear the timer state atomically from the flags
		 *
		 * since we always hold the workq lock when dropping WQ_ATIMER_RUNNING
		 * the check for and sleep until clear is protected
		 */
		while ( !(OSCompareAndSwap(wq->wq_flags, (wq->wq_flags | WQ_EXITING), (UInt32 *)&wq->wq_flags)));

		if (wq->wq_flags & WQ_ATIMER_RUNNING) {
			if (thread_call_cancel(wq->wq_atimer_call) == TRUE)
				wq->wq_flags &= ~WQ_ATIMER_RUNNING;
		}
		while ((wq->wq_flags & WQ_ATIMER_RUNNING) || (wq->wq_lflags & WQL_ATIMER_BUSY)) {

			assert_wait((caddr_t)wq, (THREAD_UNINT));
			workqueue_unlock(p);

			thread_block(THREAD_CONTINUE_NULL);

			workqueue_lock_spin(p);
		}
		workqueue_unlock(p);

		KERNEL_DEBUG(0x9008088 | DBG_FUNC_END, 0, 0, 0, 0, 0);
	}
}

/*
 * Routine:	workqueue_exit
 *
 * Function:	clean up the work queue structure(s) now that there are no threads
 *		left running inside the work queue (except possibly current_thread).
 *
 * Conditions:	Called by the last thread in the process.
 *		Called against current process.
 */
void
workqueue_exit(struct proc *p)
{
	struct workqueue  * wq;
	struct threadlist  * tl, *tlist;
	struct uthread	*uth;
	int wq_size = 0;

	wq = (struct workqueue *)p->p_wqptr;
	if (wq != NULL) {

		KERNEL_DEBUG(0x900808c | DBG_FUNC_START, p->p_wqptr, 0, 0, 0, 0);

		wq_size = p->p_wqsize;
		p->p_wqptr = NULL;
		p->p_wqsize = 0;

		/*
		 * Clean up workqueue data structures for threads that exited and
		 * didn't get a chance to clean up after themselves.
		 */
		TAILQ_FOREACH_SAFE(tl, &wq->wq_thrunlist, th_entry, tlist) {

		        thread_sched_call(tl->th_thread, NULL);

			uth = get_bsdthread_info(tl->th_thread);
			if (uth != (struct uthread *)0) {
				uth->uu_threadlist = NULL;
			}
			TAILQ_REMOVE(&wq->wq_thrunlist, tl, th_entry);

		        /*
			 * drop our last ref on the thread
			 */
		        thread_deallocate(tl->th_thread);

			kfree(tl, sizeof(struct threadlist));
		}
		TAILQ_FOREACH_SAFE(tl, &wq->wq_thidlelist, th_entry, tlist) {
			workqueue_removethread(tl, 1);
		}
		thread_call_free(wq->wq_atimer_call);

		kfree(wq, wq_size);

		KERNEL_DEBUG(0x900808c | DBG_FUNC_END, 0, 0, 0, 0, 0);
	}
}


static int workqueue_importance[WORKQUEUE_NUMPRIOS] = 
{
	2, 0, -2, INT_MIN,
};

#define WORKQ_POLICY_TIMESHARE 1

static int workqueue_policy[WORKQUEUE_NUMPRIOS] = 
{
	WORKQ_POLICY_TIMESHARE, WORKQ_POLICY_TIMESHARE, WORKQ_POLICY_TIMESHARE, WORKQ_POLICY_TIMESHARE
};



static boolean_t
workqueue_run_one(proc_t p, struct workqueue *wq, boolean_t overcommit, int priority)
{
	boolean_t	ran_one;

	if (wq->wq_thidlecount == 0) {
		if (overcommit == FALSE) {
			if (wq->wq_constrained_threads_scheduled < wq->wq_affinity_max)
				workqueue_addnewthread(wq, overcommit);
		} else {
			workqueue_addnewthread(wq, overcommit);

			if (wq->wq_thidlecount == 0)
				return (FALSE);
		}
	}
	ran_one = workqueue_run_nextreq(p, wq, THREAD_NULL, FALSE, overcommit, priority, -1);
	/*
	 * workqueue_run_nextreq is responsible for
	 * dropping the workqueue lock in all cases
	 */
	workqueue_lock_spin(p);

	return (ran_one);
}



/*
 * workqueue_run_nextreq:
 *   called with the workqueue lock held...
 *   responsible for dropping it in all cases
 */
static boolean_t
workqueue_run_nextreq(proc_t p, struct workqueue *wq, thread_t thread,
		      boolean_t force_oc, boolean_t overcommit, int oc_prio, int oc_affinity)
{
	thread_t th_to_run = THREAD_NULL;
	thread_t th_to_park = THREAD_NULL;
	int wake_thread = 0;
	int reuse_thread = WQ_FLAG_THREAD_REUSE;
	uint32_t priority, orig_priority;
	uint32_t affinity_tag, orig_affinity_tag;
	uint32_t i, n;
	uint32_t busycount;
	uint32_t us_to_wait;
	struct threadlist *tl = NULL;
	struct threadlist *ttl = NULL;
	struct uthread *uth = NULL;
	boolean_t start_timer = FALSE;
	boolean_t adjust_counters = TRUE;
	uint64_t  curtime;


	KERNEL_DEBUG(0xefffd000 | DBG_FUNC_START, wq, thread, wq->wq_thidlecount, wq->wq_reqcount, 0);

	if (thread != THREAD_NULL) {
		uth = get_bsdthread_info(thread);

		if ( (tl = uth->uu_threadlist) == NULL)
			panic("wq thread with no threadlist ");
	}
	/*
	 * from here until we drop the workq lock
	 * we can't be pre-empted since we hold 
	 * the lock in spin mode... this is important
	 * since we have to independently update the priority
	 * and affinity that the thread is associated with
	 * and these values are used to index the multi-dimensional
	 * counter arrays in 'workqueue_callback'
	 */
dispatch_overcommit:

	if (overcommit == TRUE || force_oc == TRUE) {
		uint32_t min_scheduled = 0;
		uint32_t scheduled_count;
		uint32_t active_count;
		uint32_t t_affinity = 0;

		priority = oc_prio;

		if ((affinity_tag = oc_affinity) == (uint32_t)-1) {
			for (affinity_tag = 0; affinity_tag < wq->wq_reqconc[priority]; affinity_tag++) {
				/*
				 * look for the affinity group with the least number of threads
				 */
				scheduled_count = 0;
				active_count = 0;

				for (i = 0; i <= priority; i++) {
					scheduled_count += wq->wq_thscheduled_count[i][affinity_tag];
					active_count += wq->wq_thactive_count[i][affinity_tag];
				}
				if (active_count == 0) {
					t_affinity = affinity_tag;
					break;
				}
				if (affinity_tag == 0 || scheduled_count < min_scheduled) {
					min_scheduled = scheduled_count;
					t_affinity = affinity_tag;
				}
			}
			affinity_tag = t_affinity;
		}
		if (thread != THREAD_NULL) {
			th_to_run = thread;
			goto pick_up_work;
		}
		goto grab_idle_thread;
	}
	if (wq->wq_reqcount) {
		for (priority = 0; priority < WORKQUEUE_NUMPRIOS; priority++) {
			if (wq->wq_requests[priority])
				break;
		}
		assert(priority < WORKQUEUE_NUMPRIOS);

		if (wq->wq_ocrequests[priority] && (thread != THREAD_NULL || wq->wq_thidlecount)) {
			/*
			 * handle delayed overcommit request...
			 * they have priority over normal requests
			 * within a given priority level
			 */
			wq->wq_reqcount--;
			wq->wq_requests[priority]--;
			wq->wq_ocrequests[priority]--;

			oc_prio = priority;
			overcommit = TRUE;

			goto dispatch_overcommit;
		}
	}
	/*
	 * if we get here, the work should be handled by a constrained thread
	 */
	if (wq->wq_reqcount == 0 || wq->wq_constrained_threads_scheduled >= wq_max_constrained_threads) {
		/*
		 * no work to do, or we're already at or over the scheduling limit for
		 * constrained threads...  just return or park the thread...
		 * do not start the timer for this condition... if we don't have any work,
		 * we'll check again when new work arrives... if we're over the limit, we need 1 or more
		 * constrained threads to return to the kernel before we can dispatch additional work
		 */
	        if ((th_to_park = thread) == THREAD_NULL)
		        goto out_of_work;
		goto parkit;
	}

	curtime = mach_absolute_time();

	if (thread != THREAD_NULL) {

		affinity_tag = tl->th_affinity_tag;

		/*
		 * check to see if the affinity group this thread is
		 * associated with is still within the bounds of the
		 * specified concurrency for the priority level
		 * we're considering running work for
		 */
		if (affinity_tag < wq->wq_reqconc[priority]) {
			uint32_t  bcount = 0;
			uint32_t  acount = 0;
			uint32_t  tcount = 0;

			/*
			 * we're a worker thread from the pool... currently we
			 * are considered 'active' which means we're counted
			 * in "wq_thactive_count"
			 * add up the active counts of all the priority levels
			 * up to and including the one we want to schedule
			 */
			for (i = 0; i <= priority; i++) {

				tcount = wq->wq_thactive_count[i][affinity_tag];
				acount += tcount;

				if (tcount == 0 && wq->wq_thscheduled_count[i][affinity_tag]) {
					if (wq_thread_is_busy(curtime, &wq->wq_lastblocked_ts[i][affinity_tag]))
						bcount++;
				}
			}
			if ((acount + bcount) == 1) {
				/*
				 * we're the only active thread associated with our
				 * affinity group at this priority level and higher,
				 * and there are no threads considered 'busy',
				 * so pick up some work and keep going
				 */
				th_to_run = thread;
				goto pick_up_work;
			}
			if (wq->wq_reqconc[priority] == 1) {
				/*
				 * we have at least one other active or busy thread running at this
				 * priority level or higher and since we only have 
				 * 1 affinity group to schedule against, no need
				 * to try and find another... we can't start up another thread to
				 * service the request and we already have the info
				 * needed to determine if we need to start a timer or not
				 */
				if (acount == 1) {
					/*
					 * we're the only active thread, but we must have found
					 * at least 1 busy thread, so indicate that we need
					 * to start a timer
					 */
					busycount = 1;
				} else
					busycount = 0;

				affinity_tag = 1;
				goto cant_schedule;
			}
		}
		/*
		 * there's more than 1 thread running in this affinity group
		 * or the concurrency level has been cut back for this priority...
		 * let's continue on and look for an 'empty' group to run this
		 * work request in
		 */
	}
	busycount = 0;

	for (affinity_tag = 0; affinity_tag < wq->wq_reqconc[priority]; affinity_tag++) {
		boolean_t	can_schedule;

		/*
		 * look for first affinity group that is currently not active
		 * i.e. no active threads at this priority level or higher
		 * and no threads that have run recently
		 */
		for (i = 0; i <= priority; i++) {
			can_schedule = FALSE;

			if (wq->wq_thactive_count[i][affinity_tag])
				break;

			if (wq->wq_thscheduled_count[i][affinity_tag] &&
			    wq_thread_is_busy(curtime, &wq->wq_lastblocked_ts[i][affinity_tag])) {
				busycount++;
				break;
			}
			can_schedule = TRUE;
		}
		if (can_schedule == TRUE)
			break;
	}
cant_schedule:

	if (affinity_tag >= wq->wq_reqconc[priority]) {
		/*
		 * we've already got at least 1 thread per
		 * affinity group in the active state...
		 */
		if (busycount) {
			/*
			 * we found at least 1 thread in the
			 * 'busy' state... make sure we start
			 * the timer because if they are the only
			 * threads keeping us from scheduling
			 * this work request, we won't get a callback
			 * to kick off the timer... we need to
			 * start it now...
			 */
			WQ_TIMER_NEEDED(wq, start_timer);
		}
		KERNEL_DEBUG(0xefffd000 | DBG_FUNC_NONE, wq, busycount, start_timer, 0, 0);

		if (thread != THREAD_NULL) {
			/*
			 * go park this one for later
			 */
			th_to_park = thread;
		        goto parkit;
		}
		goto out_of_work;
	}
	if (thread != THREAD_NULL) {
		/*
		 * we're overbooked on the affinity group this thread is
		 * currently associated with, but we have work to do
		 * and at least 1 idle processor, so we'll just retarget
		 * this thread to a new affinity group
		 */
		th_to_run = thread;
		goto pick_up_work;
	}

grab_idle_thread:
	if (wq->wq_thidlecount == 0) {
		/*
 		 * we don't have a thread to schedule, but we have
		 * work to do and at least 1 affinity group that 
		 * doesn't currently have an active thread... 
		 */
		WQ_TIMER_NEEDED(wq, start_timer);

		KERNEL_DEBUG(0xefffd118, wq, wq->wq_nthreads, start_timer, 0, 0);

		goto no_thread_to_run;
	}
	/*
	 * we've got a candidate (affinity group with no currently
	 * active threads) to start a new thread on...
	 * we already know there is both work available
	 * and an idle thread, so activate a thread and then
	 * fall into the code that pulls a new work request...
	 */
	TAILQ_FOREACH(ttl, &wq->wq_thidlelist, th_entry) {
		if (ttl->th_affinity_tag == affinity_tag || ttl->th_affinity_tag == (uint16_t)-1) {

			TAILQ_REMOVE(&wq->wq_thidlelist, ttl, th_entry);
			tl = ttl;

			break;
		}
	}
	if (tl == NULL) {
		tl = TAILQ_FIRST(&wq->wq_thidlelist);
		TAILQ_REMOVE(&wq->wq_thidlelist, tl, th_entry);
	}
	wq->wq_thidlecount--;
	
	TAILQ_INSERT_TAIL(&wq->wq_thrunlist, tl, th_entry);

	if ((tl->th_flags & TH_LIST_SUSPENDED) == TH_LIST_SUSPENDED) {
		tl->th_flags &= ~TH_LIST_SUSPENDED;
		reuse_thread = 0;

	} else if ((tl->th_flags & TH_LIST_BLOCKED) == TH_LIST_BLOCKED) {
		tl->th_flags &= ~TH_LIST_BLOCKED;
		wake_thread = 1;
	}
	tl->th_flags |= TH_LIST_RUNNING | TH_LIST_BUSY;

	wq->wq_threads_scheduled++;
	wq->wq_thscheduled_count[priority][affinity_tag]++;
	OSAddAtomic(1, &wq->wq_thactive_count[priority][affinity_tag]);

	adjust_counters = FALSE;
	th_to_run = tl->th_thread;

pick_up_work:
	if (overcommit == FALSE && force_oc == FALSE) {
		wq->wq_reqcount--;
		wq->wq_requests[priority]--;

		if ( !(tl->th_flags & TH_LIST_CONSTRAINED)) {
			wq->wq_constrained_threads_scheduled++;
			tl->th_flags |= TH_LIST_CONSTRAINED;
		}
	} else {
		if (tl->th_flags & TH_LIST_CONSTRAINED) {
			wq->wq_constrained_threads_scheduled--;
			tl->th_flags &= ~TH_LIST_CONSTRAINED;
		}
	}
	orig_priority = tl->th_priority;
	orig_affinity_tag = tl->th_affinity_tag;

	tl->th_priority = priority;
	tl->th_affinity_tag = affinity_tag;

	if (adjust_counters == TRUE && (orig_priority != priority || orig_affinity_tag != affinity_tag)) {
		/*
		 * we need to adjust these counters based on this
		 * thread's new disposition w/r to affinity and priority
		 */
	        OSAddAtomic(-1, &wq->wq_thactive_count[orig_priority][orig_affinity_tag]);
	        OSAddAtomic(1, &wq->wq_thactive_count[priority][affinity_tag]);

		wq->wq_thscheduled_count[orig_priority][orig_affinity_tag]--;
		wq->wq_thscheduled_count[priority][affinity_tag]++;
	}
	wq->wq_thread_yielded_count = 0;

        workqueue_unlock(p);

	if (orig_affinity_tag != affinity_tag) {
		/*
		 * this thread's affinity does not match the affinity group
		 * its being placed on (it's either a brand new thread or
		 * we're retargeting an existing thread to a new group)...
		 * affinity tag of 0 means no affinity...
		 * but we want our tags to be 0 based because they
		 * are used to index arrays, so...
		 * keep it 0 based internally and bump by 1 when
		 * calling out to set it
		 */
		KERNEL_DEBUG(0xefffd114 | DBG_FUNC_START, wq, orig_affinity_tag, 0, 0, 0);

		(void)thread_affinity_set(th_to_run, affinity_tag + 1);

		KERNEL_DEBUG(0xefffd114 | DBG_FUNC_END, wq, affinity_tag, 0, 0, 0);
	}
	if (orig_priority != priority) {
		thread_precedence_policy_data_t	precedinfo;
		thread_extended_policy_data_t	extinfo;
		uint32_t	policy;
#if CONFIG_EMBEDDED
		int retval = 0;

		/* sets the saved importance for apple ios daemon if backgrounded. else returns 0 */
		retval = proc_setthread_saved_importance(th_to_run, workqueue_importance[priority]);
		if (retval == 0) {
#endif /* CONFIG_EMBEDDED */
		policy = workqueue_policy[priority];
		
		KERNEL_DEBUG(0xefffd120 | DBG_FUNC_START, wq, orig_priority, tl->th_policy, 0, 0);

		if ((orig_priority == WORKQUEUE_BG_PRIOQUEUE) || (priority == WORKQUEUE_BG_PRIOQUEUE)) {
			if (orig_priority == WORKQUEUE_BG_PRIOQUEUE) {
				/* remove the disk throttle, importance will be reset in anycase */
				proc_restore_workq_bgthreadpolicy(th_to_run);
			} 

			if (priority == WORKQUEUE_BG_PRIOQUEUE) {
				proc_apply_workq_bgthreadpolicy(th_to_run);
			}
		}

		if (tl->th_policy != policy) {
			extinfo.timeshare = policy;
			(void)thread_policy_set_internal(th_to_run, THREAD_EXTENDED_POLICY, (thread_policy_t)&extinfo, THREAD_EXTENDED_POLICY_COUNT);

			tl->th_policy = policy;
		}

                precedinfo.importance = workqueue_importance[priority];
                (void)thread_policy_set_internal(th_to_run, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&precedinfo, THREAD_PRECEDENCE_POLICY_COUNT);


		KERNEL_DEBUG(0xefffd120 | DBG_FUNC_END, wq,  priority, policy, 0, 0);
#if CONFIG_EMBEDDED
		}
#endif /* CONFIG_EMBEDDED */
	}
	if (kdebug_enable) {
		int	lpri = -1;
		int	laffinity = -1;
		int	first = -1;
		uint32_t  code = 0xefffd02c | DBG_FUNC_START;

		for (n = 0; n < WORKQUEUE_NUMPRIOS; n++) {
			for (i = 0; i < wq->wq_affinity_max; i++) {
				if (wq->wq_thactive_count[n][i]) {
					if (lpri != -1) {
						KERNEL_DEBUG(code, lpri, laffinity, wq->wq_thactive_count[lpri][laffinity], first, 0);
						code = 0xefffd02c;
						first = 0;
					}
					lpri = n;
					laffinity = i;
				}
			}
		}
		if (lpri != -1) {
			if (first == -1)
				first = 0xeeeeeeee;
			KERNEL_DEBUG(0xefffd02c | DBG_FUNC_END, lpri, laffinity, wq->wq_thactive_count[lpri][laffinity], first, 0);
		}
	}
	/*
	 * if current thread is reused for work request, does not return via unix_syscall
	 */
	wq_runreq(p, overcommit, priority, th_to_run, tl, reuse_thread, wake_thread, (thread == th_to_run));
	
	KERNEL_DEBUG(0xefffd000 | DBG_FUNC_END, wq, thread_tid(th_to_run), overcommit, 1, 0);

	return (TRUE);

out_of_work:
	/*
	 * we have no work to do or we are fully booked
	 * w/r to running threads...
	 */
no_thread_to_run:
	workqueue_unlock(p);

	if (start_timer)
		workqueue_interval_timer_start(wq);

	KERNEL_DEBUG(0xefffd000 | DBG_FUNC_END, wq, thread_tid(thread), 0, 2, 0);

	return (FALSE);

parkit:
	/*
	 * this is a workqueue thread with no more
	 * work to do... park it for now
	 */
	TAILQ_REMOVE(&wq->wq_thrunlist, tl, th_entry);
	tl->th_flags &= ~TH_LIST_RUNNING;

	tl->th_flags |= TH_LIST_BLOCKED;
	TAILQ_INSERT_HEAD(&wq->wq_thidlelist, tl, th_entry);

	thread_sched_call(th_to_park, NULL);

	OSAddAtomic(-1, &wq->wq_thactive_count[tl->th_priority][tl->th_affinity_tag]);
	wq->wq_thscheduled_count[tl->th_priority][tl->th_affinity_tag]--;
	wq->wq_threads_scheduled--;

	if (tl->th_flags & TH_LIST_CONSTRAINED) {
		wq->wq_constrained_threads_scheduled--;
		wq->wq_lflags &= ~WQL_EXCEEDED_CONSTRAINED_THREAD_LIMIT;
		tl->th_flags &= ~TH_LIST_CONSTRAINED;
	}
	if (wq->wq_thidlecount < 100)
		us_to_wait = wq_reduce_pool_window_usecs - (wq->wq_thidlecount * (wq_reduce_pool_window_usecs / 100));
	else
		us_to_wait = wq_reduce_pool_window_usecs / 100;

	wq->wq_thidlecount++;
	wq->wq_lflags &= ~WQL_EXCEEDED_TOTAL_THREAD_LIMIT;

	assert_wait_timeout((caddr_t)tl, (THREAD_INTERRUPTIBLE), us_to_wait, NSEC_PER_USEC);

	workqueue_unlock(p);

	if (start_timer)
		workqueue_interval_timer_start(wq);

	KERNEL_DEBUG1(0xefffd018 | DBG_FUNC_START, wq, wq->wq_threads_scheduled, wq->wq_thidlecount, us_to_wait, thread_tid(th_to_park));
	KERNEL_DEBUG(0xefffd000 | DBG_FUNC_END, wq, thread_tid(thread), 0, 3, 0);

	thread_block((thread_continue_t)wq_unpark_continue);
	/* NOT REACHED */

	return (FALSE);
}


static void
wq_unsuspend_continue(void)
{
	struct uthread *uth = NULL;
	thread_t th_to_unsuspend;
	struct threadlist *tl;
	proc_t	p;

	th_to_unsuspend = current_thread();
	uth = get_bsdthread_info(th_to_unsuspend);

	if (uth != NULL && (tl = uth->uu_threadlist) != NULL) {
		
		if ((tl->th_flags & (TH_LIST_RUNNING | TH_LIST_BUSY)) == TH_LIST_RUNNING) {
			/*
			 * most likely a normal resume of this thread occurred...
			 * it's also possible that the thread was aborted after we
			 * finished setting it up so that it could be dispatched... if
			 * so, thread_bootstrap_return will notice the abort and put
			 * the thread on the path to self-destruction
			 */
normal_resume_to_user:
			thread_sched_call(th_to_unsuspend, workqueue_callback);

			thread_bootstrap_return();
		}
		/*
		 * if we get here, it's because we've been resumed due to
		 * an abort of this thread (process is crashing)
		 */
		p = current_proc();

		workqueue_lock_spin(p);

		if (tl->th_flags & TH_LIST_SUSPENDED) {
			/*
			 * thread has been aborted while still on our idle
			 * queue... remove it from our domain...
			 * workqueue_removethread consumes the lock
			 */
			workqueue_removethread(tl, 0);

			thread_bootstrap_return();
		}
		while ((tl->th_flags & TH_LIST_BUSY)) {
			/*
			 * this thread was aborted after we started making
			 * it runnable, but before we finished dispatching it...
			 * we need to wait for that process to finish,
			 * and we need to ask for a wakeup instead of a
			 * thread_resume since the abort has already resumed us
			 */
			tl->th_flags |= TH_LIST_NEED_WAKEUP;

			assert_wait((caddr_t)tl, (THREAD_UNINT));

			workqueue_unlock(p);

			thread_block(THREAD_CONTINUE_NULL);

			workqueue_lock_spin(p);
		}
		workqueue_unlock(p);
		/*
		 * we have finished setting up the thread's context...
		 * thread_bootstrap_return will take us through the abort path
		 * where the thread will self destruct
		 */
		goto normal_resume_to_user;
	}
	thread_bootstrap_return();
}


static void
wq_unpark_continue(void)
{
	struct uthread *uth = NULL;
	struct threadlist *tl;
	thread_t th_to_unpark;
	proc_t 	p;
				
	th_to_unpark = current_thread();
	uth = get_bsdthread_info(th_to_unpark);

	if (uth != NULL) {
		if ((tl = uth->uu_threadlist) != NULL) {

			if ((tl->th_flags & (TH_LIST_RUNNING | TH_LIST_BUSY)) == TH_LIST_RUNNING) {
				/*
				 * a normal wakeup of this thread occurred... no need 
				 * for any synchronization with the timer and wq_runreq
				 */
normal_return_to_user:			
				thread_sched_call(th_to_unpark, workqueue_callback);

				KERNEL_DEBUG(0xefffd018 | DBG_FUNC_END, tl->th_workq, 0, 0, 0, 0);
	
				thread_exception_return();
			}
			p = current_proc();

			workqueue_lock_spin(p);

			if ( !(tl->th_flags & TH_LIST_RUNNING)) {
				/*
				 * the timer popped us out and we've not
				 * been moved off of the idle list
				 * so we should now self-destruct
				 *
				 * workqueue_removethread consumes the lock
				 */
				workqueue_removethread(tl, 0);
					
				thread_exception_return();
			}
			/*
			 * the timer woke us up, but we have already
			 * started to make this a runnable thread,
			 * but have not yet finished that process...
			 * so wait for the normal wakeup
			 */
			while ((tl->th_flags & TH_LIST_BUSY)) {

				assert_wait((caddr_t)tl, (THREAD_UNINT));

				workqueue_unlock(p);

				thread_block(THREAD_CONTINUE_NULL);

				workqueue_lock_spin(p);
			}
			/*
			 * we have finished setting up the thread's context
			 * now we can return as if we got a normal wakeup
			 */
			workqueue_unlock(p);

			goto normal_return_to_user;
		}
	}
	thread_exception_return();
}



static void 
wq_runreq(proc_t p, boolean_t overcommit, uint32_t priority, thread_t th, struct threadlist *tl,
	   int reuse_thread, int wake_thread, int return_directly)
{
	int ret = 0;
	boolean_t need_resume = FALSE;

	KERNEL_DEBUG1(0xefffd004 | DBG_FUNC_START, tl->th_workq, tl->th_priority, tl->th_affinity_tag, thread_tid(current_thread()), thread_tid(th));

	ret = setup_wqthread(p, th, overcommit, priority, reuse_thread, tl);

	if (ret != 0)
		panic("setup_wqthread failed  %x\n", ret);

	if (return_directly) {
		KERNEL_DEBUG(0xefffd000 | DBG_FUNC_END, tl->th_workq, 0, 0, 4, 0);

		thread_exception_return();

		panic("wq_runreq: thread_exception_return returned ...\n");
	}
	if (wake_thread) {
		workqueue_lock_spin(p);
		
		tl->th_flags &= ~TH_LIST_BUSY;
		wakeup(tl);

		workqueue_unlock(p);
	} else {
	        KERNEL_DEBUG1(0xefffd014 | DBG_FUNC_END, tl->th_workq, 0, 0, thread_tid(current_thread()), thread_tid(th));

		workqueue_lock_spin(p);

		if (tl->th_flags & TH_LIST_NEED_WAKEUP)
			wakeup(tl);
		else
			need_resume = TRUE;

		tl->th_flags &= ~(TH_LIST_BUSY | TH_LIST_NEED_WAKEUP);
		
		workqueue_unlock(p);

		if (need_resume) {
			/*
			 * need to do this outside of the workqueue spin lock
			 * since thread_resume locks the thread via a full mutex
			 */
			thread_resume(th);
		}
	}
}


int
setup_wqthread(proc_t p, thread_t th, boolean_t overcommit, uint32_t priority, int reuse_thread, struct threadlist *tl)
{
	uint32_t flags = reuse_thread | WQ_FLAG_THREAD_NEWSPI;

	if (overcommit == TRUE)
		flags |= WQ_FLAG_THREAD_OVERCOMMIT;

	flags |= priority;

#if defined(__i386__) || defined(__x86_64__)
	int isLP64 = 0;

	isLP64 = IS_64BIT_PROCESS(p);
        /*
         * Set up i386 registers & function call.
         */
	if (isLP64 == 0) {
		x86_thread_state32_t state;
		x86_thread_state32_t *ts = &state;

        	ts->eip = (int)p->p_wqthread;
		ts->eax = (unsigned int)(tl->th_stackaddr + PTH_DEFAULT_STACKSIZE + PTH_DEFAULT_GUARDSIZE);
		ts->ebx = (unsigned int)tl->th_thport;
		ts->ecx = (unsigned int)(tl->th_stackaddr + PTH_DEFAULT_GUARDSIZE);
		ts->edx = (unsigned int)0;
		ts->edi = (unsigned int)flags;
		ts->esi = (unsigned int)0;
		/*
		 * set stack pointer
		 */
        	ts->esp = (int)((vm_offset_t)((tl->th_stackaddr + PTH_DEFAULT_STACKSIZE + PTH_DEFAULT_GUARDSIZE) - C_32_STK_ALIGN));

		thread_set_wq_state32(th, (thread_state_t)ts);

	} else {
	        x86_thread_state64_t state64;
		x86_thread_state64_t *ts64 = &state64;

        	ts64->rip = (uint64_t)p->p_wqthread;
		ts64->rdi = (uint64_t)(tl->th_stackaddr + PTH_DEFAULT_STACKSIZE + PTH_DEFAULT_GUARDSIZE);
		ts64->rsi = (uint64_t)(tl->th_thport);
		ts64->rdx = (uint64_t)(tl->th_stackaddr + PTH_DEFAULT_GUARDSIZE);
		ts64->rcx = (uint64_t)0;
		ts64->r8 = (uint64_t)flags;
		ts64->r9 = (uint64_t)0;

		/*
		 * set stack pointer aligned to 16 byte boundary
		 */
		ts64->rsp = (uint64_t)((tl->th_stackaddr + PTH_DEFAULT_STACKSIZE + PTH_DEFAULT_GUARDSIZE) - C_64_REDZONE_LEN);

		thread_set_wq_state64(th, (thread_state_t)ts64);
	}
#else
#error setup_wqthread  not defined for this architecture
#endif
	return(0);
}

int 
fill_procworkqueue(proc_t p, struct proc_workqueueinfo * pwqinfo)
{
	struct workqueue * wq;
	int error = 0;
	int	activecount;
	uint32_t pri, affinity;

	workqueue_lock_spin(p);
	if ((wq = p->p_wqptr) == NULL) {
		error = EINVAL;
		goto out;
	}
	activecount = 0;

	for (pri = 0; pri < WORKQUEUE_NUMPRIOS; pri++) {
		for (affinity = 0; affinity < wq->wq_affinity_max; affinity++)
			activecount += wq->wq_thactive_count[pri][affinity];
	}
	pwqinfo->pwq_nthreads = wq->wq_nthreads;
	pwqinfo->pwq_runthreads = activecount;
	pwqinfo->pwq_blockedthreads = wq->wq_threads_scheduled - activecount;
	pwqinfo->pwq_state = 0;

	if (wq->wq_lflags & WQL_EXCEEDED_CONSTRAINED_THREAD_LIMIT)
		pwqinfo->pwq_state |= WQ_EXCEEDED_CONSTRAINED_THREAD_LIMIT;

	if (wq->wq_lflags & WQL_EXCEEDED_TOTAL_THREAD_LIMIT)
		pwqinfo->pwq_state |= WQ_EXCEEDED_TOTAL_THREAD_LIMIT;

out:
	workqueue_unlock(p);
	return(error);
}

/* Set target concurrency of one of the  queue(0,1,2) with specified value */
int
proc_settargetconc(pid_t pid, int queuenum, int32_t targetconc)
{
	proc_t p, self;
	uint64_t addr;
	int32_t conc = targetconc;
	int error = 0;
	vm_map_t oldmap = VM_MAP_NULL;
	int gotref = 0;

	self = current_proc();
	if (self->p_pid != pid) {
		/* if not on self, hold a refernce on the process */
		
		if (pid == 0)
			return(EINVAL);

		p = proc_find(pid);

		if (p == PROC_NULL)
			return(ESRCH);
		gotref = 1;

	} else
		p = self;

	if ((addr = p->p_targconc) == (uint64_t)0) {
		error = EINVAL;
		goto out;
	}


	if ((queuenum >= WQ_MAXPRI_MIN) && (queuenum <= WQ_MAXPRI_MAX)) {
		addr += (queuenum * sizeof(int32_t));
		if (gotref == 1)
			oldmap = vm_map_switch(get_task_map(p->task));
		error = copyout(&conc, addr, sizeof(int32_t));
		if (gotref == 1)
			(void)vm_map_switch(oldmap);

	} else  {
		error = EINVAL;
	}
out:
	if (gotref == 1)
		proc_rele(p);
	return(error);
}


/* Set target concurrency on all the prio queues with specified value */
int 
proc_setalltargetconc(pid_t pid, int32_t * targetconcp)
{
	proc_t p, self;
	uint64_t addr;
	int error = 0;
	vm_map_t oldmap = VM_MAP_NULL;
	int gotref = 0;

	self = current_proc();
	if (self->p_pid != pid) {
		/* if not on self, hold a refernce on the process */
		
		if (pid == 0)
			return(EINVAL);

		p = proc_find(pid);

		if (p == PROC_NULL)
			return(ESRCH);
		gotref = 1;

	} else
		p = self;

	if ((addr = (uint64_t)p->p_targconc) == (uint64_t)0) {
		error = EINVAL;
		goto out;
	}


	if (gotref == 1)
		oldmap = vm_map_switch(get_task_map(p->task));

	error = copyout(targetconcp, addr, WQ_PRI_NUM * sizeof(int32_t));
	if (gotref == 1)
		(void)vm_map_switch(oldmap);

out:
	if (gotref == 1)
		proc_rele(p);
	return(error);
}

int thread_selfid(__unused struct proc *p, __unused struct thread_selfid_args *uap, uint64_t *retval)
{
	thread_t thread = current_thread();
	*retval = thread_tid(thread);
	return KERN_SUCCESS;
}

void
pthread_init(void)
{
	pthread_lck_grp_attr = lck_grp_attr_alloc_init();
	pthread_lck_grp = lck_grp_alloc_init("pthread", pthread_lck_grp_attr);
	
	/*
	 * allocate the lock attribute for pthread synchronizers
	 */
	pthread_lck_attr = lck_attr_alloc_init();

	workqueue_init_lock((proc_t) get_bsdtask_info(kernel_task));
#if PSYNCH
	pthread_list_mlock = lck_mtx_alloc_init(pthread_lck_grp, pthread_lck_attr);
	
	pth_global_hashinit();
	psynch_thcall = thread_call_allocate(psynch_wq_cleanup, NULL);
	psynch_zoneinit();
#endif /* PSYNCH */
}
