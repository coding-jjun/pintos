#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "lib/user/syscall.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/**
 * @brief `timer_sleep()`에 의해 추가될 스레드들을 담은 연결리스트
 */
static struct list g_sleep_list;
/**
 * @brief 전체 스레드를 관리하는 리스트, mlfqs에서만 쓰이기 때문에 `d_elem`으로
 * 연결가능.
 */
static struct list g_thread_pool;

/**
 * @brief ready list에 있는 스레드의 개수. 초기값은 1
 * unblock(create 포함)시 +1
 * exit, block시 -1
 */
static int g_ready_threads = 1;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */
static int64_t g_min_tick; // NOTE - sleep_list 스레드들의 최소 local_tick

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;
/// @brief system-wide load average. check **EWMA** on wikipedia
static fixed_point g_load_avg = 0;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
  ASSERT (intr_get_level () == INTR_OFF);

  /* Reload the temporal gdt for the kernel
   * This gdt does not include the user context.
   * The kernel will rebuild the gdt with user context, in gdt_init (). */
  struct desc_ptr gdt_ds = {
    .size = sizeof (gdt) - 1,
    .address = (uint64_t) gdt
  };
  lgdt (&gdt_ds);

  /* Init the global thread context */
  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&destruction_req);
  list_init (&g_sleep_list);
  list_init (&g_thread_pool);
  
  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  initial_thread->recent_cpu = 0;
  init_thread (initial_thread, "main", PRI_DEFAULT);

  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

  
  if (thread_mlfqs) {
    g_ready_threads = 1;
  }
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pml4 != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
      idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
    thread_func *function, void *aux) {
  struct thread *t, *cur = thread_current();
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Call the kernel_thread if it scheduled.
   * Note) rdi is 1st argument, and rsi is 2nd argument. */
  t->tf.rip = (uintptr_t) kernel_thread;
  t->tf.R.rdi = (uint64_t) function;
  t->tf.R.rsi = (uint64_t) aux;
  t->tf.ds = SEL_KDSEG;
  t->tf.es = SEL_KDSEG;
  t->tf.ss = SEL_KDSEG;
  t->tf.cs = SEL_KCSEG;
  t->tf.eflags = FLAG_IF;

#ifdef USERPROG
  struct child_info *ch_info = (struct child_info *) malloc(sizeof(struct child_info));   // 자식의 유서 새로 할당
  ch_info->pid = tid;  // 자식의 주민 등록 번호
  ch_info->th = t;  // 자식 thread의 포인터 등록
  ch_info->exited = false;  // 자식의 사망 여부

  t->fd_table = palloc_get_multiple(PAL_ZERO, FDT_PAGES);
  if (t->fd_table == NULL) {
    return TID_ERROR;
  }
  t->fd_idx = 2;
  t->fd_table[0] = 1;   // stdin 자리 : 1 배정
  t->fd_table[1] = 2;   // stdout 자리 : 2 배정
  t->parent = thread_current();
  list_push_front(&cur->child_list, &ch_info->c_elem);
#endif  /* USERPROG */

  /* Add to run queue. */
  thread_unblock(t);
  /* 
  * 새로 생성한 priority와 현재 실행중인 priority를 비교해서 새로 생성한 priority가 더 크다면 yield해서 선점  
  * unblock에서 ready_list에 insert_order하기 때문에 (!list_empty(&ready_list))예외 처리는 생략
  */
  if (t->priority >= thread_get_priority())
    thread_yield();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  if (thread_mlfqs && thread_current() != idle_thread) {
    g_ready_threads -= 1;
  }

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) {
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  // list_insert_ordered(&ready_list, &t->elem, priority_dsc, NULL);
  list_push_back(&ready_list, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
  
  if (thread_mlfqs) {
    g_ready_threads += 1;
  }
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
  struct thread *t = running_thread ();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
  return thread_current ()->tid;
}

/** @brief Deschedules the current thread and destroys it.  Never
 * returns to the caller.
 * 
 */
void thread_exit(void) {
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  if (thread_mlfqs) {
    list_remove(&thread_current()->d_elem);
    g_ready_threads -= 1;
  }

  /* Just set our status to dying and schedule another process.
     We will be destroyed during the call to schedule_tail(). */
  intr_disable ();
  do_schedule (THREAD_DYING);
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {
  struct thread *curr = thread_current ();
  enum intr_level old_level;

  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (curr != idle_thread) {
    // list_insert_ordered(&ready_list, &curr->elem, priority_dsc, NULL);
    list_push_back(&ready_list, &curr->elem);
  }
  do_schedule (THREAD_READY);
  intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
  if (thread_mlfqs) {
    return;
  }
  set_priority(thread_current(), new_priority);
}

void
set_priority (struct thread *target, int new_priority) {
  target->priority = new_priority;
  /* ready_list가 비어 있지 않고, 새로 생성한 priority와 현재 실행중인 priority를 비교해서 새로 생성한 priority가 더 크다면 yield해서 선점  */
  if (!list_empty(&ready_list) && target->priority < elem_to_thread(list_front(&ready_list))->priority)
  thread_yield();
}

/**
 * @brief donation list가 비어있지 않으면 `max(donation_list)`를, 비어있다면 original priority
 */
int
thread_get_priority (void) {
  if (thread_mlfqs) {
    return 0;
  }
  return get_priority(thread_current());
}

/**
 * @brief Sets the current thread's nice value to NICE.
 * @note If the running thread no longer has the highest priority, yields.
 * @note Calculating Priority 섹션에 가서 계산공식을 확인하기 바람.
 */
void
thread_set_nice (int nice) {
  set_nice(thread_current(), nice);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
  return get_nice(thread_current());
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
  return to_int32_t_rnd(mul_int(g_load_avg, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
  return get_recent_cpu(thread_current());
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
  struct semaphore *idle_started = idle_started_;

  thread_set_priority(PRI_MIN);
  idle_thread = thread_current ();

  if (thread_mlfqs) {
    g_ready_threads -= 1;
    list_remove(&idle_thread->d_elem);
  }
#ifdef USERPROG

#endif // USERPROG
  sema_up (idle_started);

  for (;;) {
    /* Let someone else run. */
    intr_disable ();
    thread_block ();

    /* Re-enable interrupts and wait for the next one.

       The `sti' instruction disables interrupts until the
       completion of the next instruction, so these two
       instructions are executed atomically.  This atomicity is
       important; otherwise, an interrupt could be handled
       between re-enabling interrupts and waiting for the next
       one to occur, wasting as much as one clock tick worth of
       time.

       See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
       7.11.1 "HLT Instruction". */
    asm volatile ("sti; hlt" : : : "memory");
  }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);

  if (t == initial_thread) {
    t->nice = 0;
  } else {
    t->nice = thread_current()->nice;
    t->recent_cpu = thread_current()->recent_cpu;
  }
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
  list_init(&t->donation_list);
  t->magic = THREAD_MAGIC;

  if (!thread_mlfqs) {
    t->priority = priority;
  } else {
    set_priority_mlfqs(t);
    list_push_back(&g_thread_pool, &t->d_elem);
  }

#ifdef USERPROG
  t->exit_status = 0;
  list_init(&t->child_list);
  sema_init(&t->wait_sema, 0);
  sema_init(&t->fork_sema, 0);
#endif //USERPROG
#ifdef VM
  list_init(&t->head_list);
#endif //VM
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
  if (list_empty (&ready_list)) {
    return idle_thread;
  } else {
    struct list_elem *max_elem = list_max(&ready_list, priority_asc, NULL);
    list_remove(max_elem);
    return elem_to_thread(max_elem);
  }
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
  __asm __volatile(
      "movq %0, %%rsp\n"
      "movq 0(%%rsp),%%r15\n"
      "movq 8(%%rsp),%%r14\n"
      "movq 16(%%rsp),%%r13\n"
      "movq 24(%%rsp),%%r12\n"
      "movq 32(%%rsp),%%r11\n"
      "movq 40(%%rsp),%%r10\n"
      "movq 48(%%rsp),%%r9\n"
      "movq 56(%%rsp),%%r8\n"
      "movq 64(%%rsp),%%rsi\n"
      "movq 72(%%rsp),%%rdi\n"
      "movq 80(%%rsp),%%rbp\n"
      "movq 88(%%rsp),%%rdx\n"
      "movq 96(%%rsp),%%rcx\n"
      "movq 104(%%rsp),%%rbx\n"
      "movq 112(%%rsp),%%rax\n"
      "addq $120,%%rsp\n"
      "movw 8(%%rsp),%%ds\n"
      "movw (%%rsp),%%es\n"
      "addq $32, %%rsp\n"
      "iretq"
      : : "g" ((uint64_t) tf) : "memory");
}

bool tick_ascend(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *a_th = list_entry(a, struct thread, elem);
  struct thread *b_th = list_entry(b, struct thread, elem);
  
  return a_th->local_tick < b_th->local_tick;
}

bool less(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *a_th = list_entry(a, struct thread, elem);
  struct thread *b_th = list_entry(b, struct thread, elem);

  return a_th->local_tick < b_th->local_tick;
}

/**
 * @brief put current thread into sleep_list and block it until elapsed tick
 * exceeds given `ticks`
 */
void thread_sleep(int64_t ticks) {
  struct thread *cur = thread_current();
  cur->local_tick = timer_ticks() + ticks;

  // NOTE - 처음 인터럽트를 끈 놈이 인터럽트를 켜기 위해서 `old_level`
  // 변수를 사용한다.
  {
    enum intr_level old_level = intr_disable();

    list_insert_ordered(&g_sleep_list, &cur->elem, tick_ascend, NULL);
    thread_block();

    intr_set_level(old_level);
  }
}

/**
 * @brief sleep_list의 최소값을 가진 스레드를 깨울지 말지 결정한다.
 * 
 * @note g_sleep_list는 항상 정렬된 상태임을 보장해야 한다.
 */
void thread_wakeup() {
	int64_t current_ticks = timer_ticks();

	while (!list_empty(&g_sleep_list)) {
		struct list_elem *front = list_front(&g_sleep_list);
		if (current_ticks >= list_entry(front, struct thread, elem)->local_tick) {
			list_remove(front);
			thread_unblock(list_entry(front, struct thread, elem));
		} else
			break;
	}
	// 인터럽트에 의해 멱살잡고 끌려나온 스레드를 다시 준비상태로 만들어주어야 함.
}

/**
 * @brief Recalcuate `load_avg`, `recent_cpu` of all threads every 1 sec,
 * Recalculate priority of all threads every 4th tick
 */
void update_priority() {
  ASSERT(intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  int64_t cur_tick = timer_ticks();
  struct thread *cur = thread_current();
  
  if (thread_mlfqs) {
    cur->recent_cpu = FXP_ADD_INT(cur->recent_cpu, 1);
  }
  
  if (cur_tick % 4 == 0) { // 4 ticks
    // recalculate priority of all threads
    if (cur_tick % TIMER_FREQ == 0) { // 1 seconds
      // recalculate `load_avg`, `recent_cpu` of all thread
      update_load_avg();

      for (struct list_elem *d_elem = list_begin(&g_thread_pool);
           d_elem != list_end(&g_thread_pool); d_elem = list_next(d_elem)) {
        update_recent_cpu(d_elem_to_thread(d_elem));
      }
    }
    
    // recalculate priority of all threads
    for (struct list_elem *d_elem = list_begin(&g_thread_pool);
         d_elem != list_end(&g_thread_pool); d_elem = list_next(d_elem)) {
      set_priority_mlfqs(d_elem_to_thread(d_elem));
    }
    list_sort(&ready_list, origin_priority_dsc, NULL);
  }
}

struct thread *elem_to_thread(const struct list_elem *e) {
  return list_entry(e, struct thread, elem);
}

struct thread *d_elem_to_thread(const struct list_elem *e) {
  return list_entry(e, struct thread, d_elem);
}

/**
 * @brief Get the donated priority RECURSIVELY
 */
int get_priority(struct thread *target) {
  if (list_empty(&target->donation_list)) {
    // original priority
    return target->priority;
  }
  // not empty list
  struct list_elem *max_elem = list_max(&target->donation_list, priority_asc_d, NULL);

  return get_priority(d_elem_to_thread(max_elem));
}

int get_nice(struct thread *target) {
  return target->nice;
}

void set_nice(struct thread *target, int val) { 
  target->nice = val;
}

/* Returns 100 times the thread's recent_cpu value. */
inline int get_recent_cpu(struct thread *target) {
  return to_int32_t_rnd(mul_int(target->recent_cpu, 100));
}

/**
 * @brief 과제페이지가 제공한 공식에 따라 스레드의 `recent_cpu`를 수정한다.
 * @note `recent_cpu = (2 * load_avg) / (2 * load_avg + 1) * recent_cpu + nice`
 */
void update_recent_cpu(struct thread *target) {
  // fixed_point에 100을 곱한 뒤 int형으로 변환했기 때문에 이를 다시 fixed_point로 변환
  const fixed_point load_avg = g_load_avg;
  const fixed_point recent_cpu = target->recent_cpu;
  // `(2 * avg) / ((2 * avg) + 1)`
  const fixed_point decay_rate =
      FXP_DIV(
        (FXP_MUL_INT(load_avg, 2)),
        (FXP_ADD_INT(FXP_MUL_INT(load_avg, 2), 1))
      );

  // decay_rate * recent_cpu + nice
  target->recent_cpu = FXP_ADD_INT(FXP_MUL(decay_rate, recent_cpu), target->nice);
}

/**
 * @brief `load_avg = (59/60) * load_avg + (1/60) * ready_threads`
 * where `ready_threads` is the number of threads that are either running or
 * ready to run at time of update (not including the idle thread)
 * @note 1초에 한 번씩 인터럽트 핸들러에 의해 실행되어야 한다.
 */
void update_load_avg() {
  ASSERT(intr_context());
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(g_ready_threads >= 0);

  const static fixed_point coefficient1 = FXP_DIV_INT(FIXED_POINT(59), 60);
  const static fixed_point coefficient2 = FXP_DIV_INT(FIXED_POINT(1), 60);
  const int ready_threads = g_ready_threads;
      // list_size(&ready_list) + (thread_current() == idle_thread ? 0 : 1);
  const fixed_point term1 = mul(coefficient1, g_load_avg);
  const fixed_point term2 = mul_int(coefficient2, ready_threads);
  
  // do update
  g_load_avg = add(term1, term2);
}

/**
 * @brief `PRI_MAX - (recent_cpu / 4) - (nice * 2)`
 */
void set_priority_mlfqs(struct thread *target) {
  const int term2 = INT32_T(FXP_DIV_INT(target->recent_cpu, 4));
  const int term3 = target->nice * 2;
  int new_priority = PRI_MAX - term2 - term3;
  
  if (new_priority > PRI_MAX) {
    new_priority = PRI_MAX;
  } else if (new_priority < PRI_MIN) {
    new_priority = PRI_MIN;
  }

  target->priority = new_priority;
}

/**
 * @brief elem으로 ready list에 priority 내림차순 정렬
 */
bool priority_dsc(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *a_th = list_entry(a, struct thread, elem);
  struct thread *b_th = list_entry(b, struct thread, elem);
  
    return get_priority(a_th) > get_priority(b_th);
}

/**
 * @brief elem으로 ready_list에 priority 오름차순 정렬
 */
bool priority_asc(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *a_th = list_entry(a, struct thread, elem);
  struct thread *b_th = list_entry(b, struct thread, elem);
  
    return get_priority(a_th) < get_priority(b_th);
}

/**
 * @brief d_elem으로 donation_list에 priority 내림차순 정렬
 */
bool priority_dsc_d(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *a_th = d_elem_to_thread(a);
  struct thread *b_th = d_elem_to_thread(b);
  
  return get_priority(a_th) > get_priority(b_th);
}

/**
 * @brief d_elem으로 donation_list에 priority 오름차순 정렬
 */
bool priority_asc_d(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct thread *a_th = d_elem_to_thread(a);
  struct thread *b_th = d_elem_to_thread(b);
  
  return get_priority(a_th) < get_priority(b_th);
}

/**
 * @brief elem으로 origin priority 내림차순 정렬
 */
bool origin_priority_dsc(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
  struct thread *a_th = list_entry(a, struct thread, elem);
  struct thread *b_th = list_entry(b, struct thread, elem);

  return a_th->priority > b_th->priority;
}

/**
 * @brief elem으로 origin priority 오름차순 정렬
 */
bool origin_priority_asc(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
  struct thread *a_th = list_entry(a, struct thread, elem);
  struct thread *b_th = list_entry(b, struct thread, elem);

  return a_th->priority < b_th->priority;
}

/**
 * @brief d_elem으로 origin priority 내림차순 정렬
 */
bool origin_priority_dsc_d(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
  struct thread *a_th = d_elem_to_thread(a);
  struct thread *b_th = d_elem_to_thread(b);

  return a_th->priority > b_th->priority;
}

/**
 * @brief d_elem으로 origin priority 오름차순 정렬
 */
bool origin_priority_asc_d(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
  struct thread *a_th = d_elem_to_thread(a);
  struct thread *b_th = d_elem_to_thread(b);

  return a_th->priority < b_th->priority;
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
  uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
  uint64_t tf = (uint64_t) &th->tf;
  ASSERT (intr_get_level () == INTR_OFF);

  /* The main switching logic.
   * We first restore the whole execution context into the intr_frame
   * and then switching to the next thread by calling do_iret.
   * Note that, we SHOULD NOT use any stack from here
   * until switching is done. */
  __asm __volatile (
      /* Store registers that will be used. */
      "push %%rax\n"
      "push %%rbx\n"
      "push %%rcx\n"
      /* Fetch input once */
      "movq %0, %%rax\n"
      "movq %1, %%rcx\n"
      "movq %%r15, 0(%%rax)\n"
      "movq %%r14, 8(%%rax)\n"
      "movq %%r13, 16(%%rax)\n"
      "movq %%r12, 24(%%rax)\n"
      "movq %%r11, 32(%%rax)\n"
      "movq %%r10, 40(%%rax)\n"
      "movq %%r9, 48(%%rax)\n"
      "movq %%r8, 56(%%rax)\n"
      "movq %%rsi, 64(%%rax)\n"
      "movq %%rdi, 72(%%rax)\n"
      "movq %%rbp, 80(%%rax)\n"
      "movq %%rdx, 88(%%rax)\n"
      "pop %%rbx\n"              // Saved rcx
      "movq %%rbx, 96(%%rax)\n"
      "pop %%rbx\n"              // Saved rbx
      "movq %%rbx, 104(%%rax)\n"
      "pop %%rbx\n"              // Saved rax
      "movq %%rbx, 112(%%rax)\n"
      "addq $120, %%rax\n"
      "movw %%es, (%%rax)\n"
      "movw %%ds, 8(%%rax)\n"
      "addq $32, %%rax\n"
      "call __next\n"         // read the current rip.
      "__next:\n"
      "pop %%rbx\n"
      "addq $(out_iret -  __next), %%rbx\n"
      "movq %%rbx, 0(%%rax)\n" // rip
      "movw %%cs, 8(%%rax)\n"  // cs
      "pushfq\n"
      "popq %%rbx\n"
      "mov %%rbx, 16(%%rax)\n" // eflags
      "mov %%rsp, 24(%%rax)\n" // rsp
      "movw %%ss, 32(%%rax)\n"
      "mov %%rcx, %%rdi\n"
      "call do_iret\n"
      "out_iret:\n"
      : : "g"(tf_cur), "g" (tf) : "memory"
      );
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (thread_current()->status == THREAD_RUNNING);
  
  // dying 상태인 스레드들을 free 한다.
  while (!list_empty (&destruction_req)) {
    struct thread *victim =
      list_entry (list_pop_front (&destruction_req), struct thread, elem);
    palloc_free_page(victim);
  }
  thread_current ()->status = status;
  schedule ();
}

static void
schedule (void) {
  struct thread *curr = running_thread ();
  struct thread *next = next_thread_to_run ();

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (curr->status != THREAD_RUNNING);
  ASSERT (is_thread (next));
  /* Mark us as running. */
  next->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;
  g_min_tick = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate (next);
#endif

  if (curr != next) {
    /* If the thread we switched from is dying, destroy its struct
       thread. This must happen late so that thread_exit() doesn't
       pull out the rug under itself.
       We just queuing the page free reqeust here because the page is
       currently used by the stack.
       The real destruction logic will be called at the beginning of the
       schedule(). */
    if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
      ASSERT (curr != next);
      list_push_back (&destruction_req, &curr->elem);
    }

    /* Before switching the thread, we first save the information
     * of current running. */
    thread_launch (next);
  }
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/*SECTION - Fixed Point Arithmetic Definition*/
fixed_point to_fixed_point(int32_t n) { return FIXED_POINT(n); }
int32_t to_int32_t(fixed_point x) { return INT32_T(x); }
int32_t to_int32_t_rnd(fixed_point x) { return INT32_T_RND(x); }
fixed_point add(fixed_point x, fixed_point y) { return FXP_ADD(x, y); }
fixed_point add_int(fixed_point x, int32_t n) { return FXP_ADD_INT(x, n); }
fixed_point sub(fixed_point x, fixed_point y) { return FXP_SUB(x, y); }
fixed_point sub_int(fixed_point x, int32_t n) { return FXP_SUB_INT(x, n); }
fixed_point mul(fixed_point x, fixed_point y) { return FXP_MUL(x, y); }
fixed_point mul_int(fixed_point x, int32_t n) { return FXP_MUL_INT(x, n); }
fixed_point div(fixed_point x, fixed_point y) { return FXP_DIV(x, y); }
fixed_point div_int(fixed_point x, int32_t n) { return FXP_DIV_INT(x, n); }
/*!SECTION - Fixed Point Arithmetic Definition*/