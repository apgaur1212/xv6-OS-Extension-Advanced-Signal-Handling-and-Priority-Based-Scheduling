#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->start_later = 0;
  p->exec_time = -1;
  p->creation_time = ticks;
  p->first_scheduled = -1;
  p->cpu_ticks_used = 0;
  p->wait_time = 0;
  p->context_switches = 0;
  p->initial_priority = INIT_PRIORITY;
  p->dynamic_priority = INIT_PRIORITY;
  p->pending_signals = 0;
  p->is_suspended = 0;
  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  // cprintf("yeah\n");
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));
  np->start_later = 0;
  np->exec_time = -1;
  np->creation_time = ticks;
  np->first_scheduled = -1;
  np->cpu_ticks_used = 0;
  np->wait_time = 0;
  np->context_switches = 0;
  np->initial_priority = INIT_PRIORITY;
  np->dynamic_priority = INIT_PRIORITY;
  np->pending_signals = 0;
  np->is_suspended = 0;

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // My Code
  // Print metrics
  p = curproc;
  int turnaround_time = ticks - p->creation_time;
  int response_time = p->first_scheduled - p->creation_time;
  
  cprintf("PID: %d\n", p->pid);
  cprintf("TAT: %d\n", turnaround_time);
  cprintf("WT: %d\n", p->wait_time);
  cprintf("RT: %d\n", response_time);
  cprintf("#CS: %d\n", p->context_switches);

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");

}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
  int all_suspended = 1;
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      
      if(p->state != SUSPENDED)
        all_suspended = 0;
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // if(curproc->pid == 2)
    //   cprintf("hii shell  ");
    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed || all_suspended){
      // cprintf("hii there  ");
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
  release(&ptable.lock);
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  for (;;)
  {
    // Enable interrupts on this processor.
    sti();
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    // Loop over all processes and update their DYNAMIC PRIORITY
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state == RUNNABLE)
      {
        // Update dynamic priority using formula
        int dynamic = p->initial_priority - 
                            (ALPHA * p->cpu_ticks_used) + 
                            (BETA * p->wait_time);
        if (dynamic > MAX_PRIORITY)
          p->dynamic_priority = MAX_PRIORITY;
        else if (dynamic < 0)
          p->dynamic_priority = 0;
        else
          p->dynamic_priority = dynamic;

        // cprintf("CPU time %d, Waiting time %d, Priority %d\n", p->cpu_ticks_used, p->wait_time, p->dynamic_priority);
      }
    }

    // Select highest priority process
    struct proc *highest = ptable.proc;
    int max_priority = -1; 
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if(p->state == RUNNING || p->state == RUNNABLE){
          // cprintf(" %d   ", p->pid);
        handle_signals(p);
        // cprintf("pid %d before  ", p->pid);
        if (p->state == SUSPENDED){
          // cprintf("pid %d after  ", p->pid);
          // if(ptable.proc[1].state == SLEEPING){
          //   cprintf("  pid %d is set to RUNNABLE\n",  ptable.proc[1].pid);
          //   wakeup1(p);
          // }
          continue;
        }
        if (((p->cpu_ticks_used - p->creation_time) >= p->exec_time) && (p->exec_time > 0))
        {
          // Terminate the process due to overrun
          // You might call kill(p->pid) or set a flag that exit() checks.
          // Here, we assume a function kill_proc(p) exists.
        // cprintf("run pid %d, ticks %d, CT %d, EXET %d, CPU time consumed %d ", p->pid, ticks, p->creation_time, p->exec_time, p->cpu_ticks_used);
          kill(p->pid);
          // Optionally print a message:
          cprintf("Process %d exceeded execution time. Sending SIGINT.\n", p->pid);
        }
      }
      if(p->state == RUNNABLE) {
        // If this process has higher priority or same priority but lower PID
        if(highest == 0 || 
           p->dynamic_priority > max_priority || 
           (p->dynamic_priority == max_priority && p->pid < highest->pid)) {
          if(highest)
          highest = p;
          max_priority = p->dynamic_priority;
          continue;
        }
      }
    }
    if (highest)
    {
      // cprintf("pid %d state %d  ", ptable.proc[1].pid, ptable.proc[1].state);
      // If first time scheduled, record time
      if (highest->first_scheduled == -1)
      {
        highest->first_scheduled = ticks;
      }

      highest->context_switches++;
      
      // Switch to chosen process
      c->proc = highest;
      switchuvm(highest);
      highest->state = RUNNING;
      swtch(&c->scheduler, highest->context);
      switchkvm();
      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  struct proc *np;
  int all_suspended = 1;
  int havechild = 0;

  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");
  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  // cprintf(" pid %d  ", p->pid);
  for (np = ptable.proc; np < &ptable.proc[NPROC]; np++)
  {
    if(np->parent != p)
      continue;
    havechild = 1;
    // cprintf(" pid   %d    ", np->pid);
    if(np->state != SUSPENDED)
      all_suspended = 0;

  }
  if(all_suspended && havechild){
    // cprintf("hello\n");
    p->chan = 0;
    if (lk != &ptable.lock)
    { // DOC: sleeplock2
      release(&ptable.lock);
      acquire(lk);
      return;
    }
  }

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie",
  [SUSPENDED] "suspended"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// My Code
int
sys_custom_fork(void)
{
  int start_later, exec_time;
  struct proc *np;
  struct proc *p = myproc();
  
  // Get arguments from user space
  if(argint(0, &start_later) < 0 || argint(1, &exec_time) < 0)
    return -1;
  
  // Allocate process
  if((np = allocproc()) == 0){
    return -1;
  }
  // Copy user memory from parent to child
  if((np->pgdir = copyuvm(p->pgdir, p->sz)) == 0){
      kfree(np->kstack);
      np->kstack = 0;
      np->state = UNUSED;
      return -1;
    }
  np->sz = p->sz;
  np->parent = p;
  *np->tf = *p->tf;
  
  // Set return value of 0 in child process
  np->tf->eax = 0;
  
  // Increment reference counts on open file descriptors
  for(int i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);
  
  // Copy name
  safestrcpy(np->name, p->name, sizeof(p->name));
  
  // Set custom fork parameters
  np->start_later = start_later;
  np->exec_time = exec_time;
  np->creation_time = ticks;
  np->first_scheduled = -1;
  np->cpu_ticks_used = 0;
  np->wait_time = 0;
  np->context_switches = 0;
  np->initial_priority = INIT_PRIORITY;
  np->dynamic_priority = INIT_PRIORITY;
  
  // Increment process ID
  int pid = np->pid;

  acquire(&ptable.lock);

  // Set process state based on start_later flag
  if(start_later){
    np->state = SLEEPING;  // Don't schedule yet
  } else {
    np->state = RUNNABLE;
  }
  
  release(&ptable.lock);
  
  return pid;
}

int
sys_scheduler_start(void)
{
  struct proc *p;
  
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->start_later) {
      p->state = RUNNABLE;
    }
  }
  release(&ptable.lock);
  
  return 0;
}


void
update_waiting_time(void)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == RUNNABLE){
      p->wait_time++;  // Increment waiting time by one tick
    }
  }
  release(&ptable.lock);
}

void
send_signal(struct proc *p, int signum)
{
//   cprintf("pid %d updated\n", p->pid);
  p->pending_signals |= (1 << signum);
}

void
handle_signals(struct proc *p)
{
  if(p->pid <= 2)
    return;
  if(p->pending_signals & (1 << SIGINT)) {
    p->pending_signals &= ~(1 << SIGINT);
    p->killed = 1;  // Terminate the process
    p->state = RUNNABLE;
  }

  if(p->pending_signals & (1 << SIGSTP)) {
    p->pending_signals &= ~(1 << SIGSTP);
    p->is_suspended = 1;  // Suspend the process
    p->state = SUSPENDED;  // Change state to prevent scheduling
  }

  if(p->pending_signals & (1 << SIGFG)) {
    p->pending_signals &= ~(1 << SIGFG);
    if(p->is_suspended && p->state == SUSPENDED){
      p->is_suspended = 0;
      p->state = RUNNABLE;
    }
  }

  if(p->pending_signals & (1 << SIGCUSTOM)) {
    p->pending_signals &= ~(1 << SIGCUSTOM);
    if(p->custom_handler != 0){
       // Save the current instruction pointer (optional, if you implement sigreturn)
      p->saved_eip = p->tf->eip;
      p->tf->eip = (uint)p->custom_handler;
    }
    // If no handler is registered, ignore SIGCUSTOM.
  }
}


void broadcast_signal(int signum){
  struct proc *p;
  int x = 0;
  acquire(&ptable.lock);
  if(signum == 3 ) {
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->is_suspended){
        x++;
        send_signal(p, SIGFG);
        handle_signals(p);
        // cprintf("pid %d unsuspended", p->pid);
      }
    }
    if(!x)
      cprintf("No background Process\n");
    release(&ptable.lock);
    return;
  }
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid > 2 && p->state != UNUSED){
      send_signal(p, signum);
    }
  }
  release(&ptable.lock);
}
