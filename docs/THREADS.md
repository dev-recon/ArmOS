# Native Threads and pthread Roadmap

ArmOS uses a one-to-one model: every user thread is a scheduler-visible
`task_t`, while all members of a thread group share one `process_t`.

## Lots 1 to 5 implemented

The native ABI and userspace runtime provide:

- `armos_clone()` with a versionable argument block;
- one PID shared by the group and one TID per task;
- a private kernel stack and user stack for every thread;
- shared VM, file table, current directory, credentials, and signal actions;
- child-TID publication and clear-on-exit;
- a native per-task TLS base and one newlib `_reent` context per thread;
- `futex` wait/wake, including relative timeouts and clear-child-TID wakeup;
- futex-backed locking for newlib's allocator and global stdio, environment,
  and timezone state;
- `pthread_create`, join/detach, attributes, cleanup handlers, deferred
  cancellation, `pthread_once`, and thread-specific keys with destructors;
- process-private mutexes, condition variables, read/write locks, barriers,
  spin locks, and unnamed semaphores, including timed waits;
- recursive, error-checking, and robust mutexes with owner-death recovery;
- ELF `PT_TLS` loading and compiler-managed `__thread` variables through
  TPIDRURO on ARM32 and TPIDR_EL0 on ARM64;
- SMP-safe deferred reclamation after the exiting thread releases its kernel
  stack.

The currently supported clone flags are:

```text
ARMOS_CLONE_VM
ARMOS_CLONE_FS
ARMOS_CLONE_FILES
ARMOS_CLONE_SIGHAND
ARMOS_CLONE_THREAD
ARMOS_CLONE_PARENT_SETTID
ARMOS_CLONE_CHILD_SETTID
ARMOS_CLONE_CHILD_CLEARTID
```

`threadtest` validates shared memory, PID/TID identity, distinct user stacks,
per-thread `errno`, concurrent newlib allocation, blocking futex wake and
timeout paths, futex-based join, and single-thread exit without depending on a
pthread implementation.

`pthreadtest` validates the pthread lifecycle, mutexes, condition variables,
once control, TSD destructors, read/write locks, barriers, semaphores,
cancellation cleanup, robust owner death, timed waits, and initialized
compiler TLS on ARM32 and ARM64.

## Remaining milestones

1. Split process-wide signal dispositions from per-thread masks and pending
   signals.
2. Serialize VM mutations and define multithreaded `fork`, `exec`, and
   process-wide exit semantics.
3. Add priority inheritance/protection and process-shared synchronization.
4. Add named semaphores and complete less common scheduling and GNU pthread
   extensions as ports require them.

Until those milestones land, applications must not combine active threads with
concurrent VM mutation, `fork`, `exec`, or process-wide signal-management
changes. Asynchronous cancellation is intentionally unsupported.
