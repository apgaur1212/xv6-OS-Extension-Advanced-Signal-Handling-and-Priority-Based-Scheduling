// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE, SUSPENDED };

#define SIGINT  1  // Interrupt signal
#define SIGSTP  2  // Stop signal
#define SIGFG   3  // Foreground signal
#define SIGCUSTOM 4  // Your custom signal
typedef void (*sighandler_t)(void);   // Define the signal handler type:

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  // My Code
  // For custom fork
  int start_later;         // Flag to indicate delayed scheduling
  int exec_time;           // Maximum execution time

  // For scheduler profiling
  int creation_time;       // When the process was created
  int first_scheduled;     // When first scheduled (for response time)
  int cpu_ticks_used;      // CPU time consumed
  int wait_time;           // Time spent waiting
  int context_switches;    // Number of context switches
  
  // For priority scheduling
  int initial_priority;    // Initial priority value
  int dynamic_priority;    // Current priority value

  // Signal Handling
  int pending_signals;     // Bitmap of pending signals
  int is_suspended;        // Flag for suspended state
  sighandler_t custom_handler;  // Pointer to the registered SIGCUSTOM handler.
  uint saved_eip;            // Save original EIP before invoking custom_handler (optional).
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap

void update_waiting_time(void);
void broadcast_signal(int signum);
void handle_signals(struct proc *p);
void send_signal(struct proc *p, int signum);