# Native Threads and pthread Roadmap

ArmOS uses a one-to-one model: every user thread is a scheduler-visible
`task_t`, while all members of a thread group share one `process_t`.

## Foundation implemented

The initial native ABI provides:

- `armos_clone()` with a versionable argument block;
- one PID shared by the group and one TID per task;
- a private kernel stack and user stack for every thread;
- shared VM, file table, current directory, credentials, and signal actions;
- child-TID publication and clear-on-exit;
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
and single-thread exit without depending on a pthread implementation.

## Next milestones

1. Add per-task TLS context and newlib `_reent` selection.
2. Add `futex` wait/wake and clear-child-TID wakeup.
3. Implement `pthread_create`, join/detach, mutexes, condition variables,
   `pthread_once`, and thread-specific keys.
4. Split process-wide signal dispositions from per-thread masks and pending
   signals.
5. Serialize VM mutations and define multithreaded `fork`, `exec`, and
   process-wide exit semantics.

Until those milestones land, native threads must not make concurrent libc,
VM-management, or signal-management calls.
