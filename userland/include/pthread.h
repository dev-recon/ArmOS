/*
 * ArmOS pthread feature wrapper.
 *
 * Newlib ships the portable pthread declarations but only enables them for
 * operating systems that publish their feature set.  ArmOS supplies the
 * implementation in newlib-port/pthread.c.
 */

#ifndef _ARMOS_PTHREAD_WRAPPER_H
#define _ARMOS_PTHREAD_WRAPPER_H

#ifndef _POSIX_THREADS
#define _POSIX_THREADS 200809L
#endif
#ifndef _POSIX_TIMEOUTS
#define _POSIX_TIMEOUTS 200809L
#endif
#ifndef _POSIX_THREAD_PROCESS_SHARED
#define _POSIX_THREAD_PROCESS_SHARED -1
#endif
#ifndef _POSIX_BARRIERS
#define _POSIX_BARRIERS 200809L
#endif
#ifndef _POSIX_READER_WRITER_LOCKS
#define _POSIX_READER_WRITER_LOCKS 200809L
#endif
#ifndef _POSIX_SPIN_LOCKS
#define _POSIX_SPIN_LOCKS 200809L
#endif
#ifndef _UNIX98_THREAD_MUTEX_ATTRIBUTES
#define _UNIX98_THREAD_MUTEX_ATTRIBUTES 1
#endif

#include_next <pthread.h>

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 16384
#endif

#define PTHREAD_MUTEX_STALLED 0
#define PTHREAD_MUTEX_ROBUST  1

int pthread_mutexattr_getrobust(const pthread_mutexattr_t *attr,
                                int *robustness);
int pthread_mutexattr_setrobust(pthread_mutexattr_t *attr, int robustness);
int pthread_mutex_consistent(pthread_mutex_t *mutex);

#endif /* _ARMOS_PTHREAD_WRAPPER_H */
