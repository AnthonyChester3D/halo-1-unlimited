/*
PTHREAD.H (Xbox)

The few POSIX thread calls internet play uses (port/linux/src/p2p*.c: a lock its threads share, and its own thread),
for the Xbox: kernel semaphores and XAPI's CreateThread (oxbox_p2p_posix.c). Only in the builds with internet play
(OXBOX_NET_P2P).
*/

#ifndef OXBOX_PTHREAD_H
#define OXBOX_PTHREAD_H

/* (names of their own: the C library's sys/types.h, which lwIP's headers and stdlib.h bring in, has its own types;
brought in first, so that they keep theirs) */
#include <sys/types.h>
#define pthread_mutex_t oxbox_pthread_mutex_t
#define pthread_t oxbox_pthread_t
#define pthread_attr_t oxbox_pthread_attr_t
#define pthread_mutex_lock oxbox_pthread_mutex_lock
#define pthread_mutex_unlock oxbox_pthread_mutex_unlock
#define pthread_create oxbox_pthread_create
#define pthread_detach oxbox_pthread_detach

/* a kernel semaphore (KSEMAPHORE: 20 bytes), made the first time the lock is taken */
typedef struct
{
	volatile long state;
	long storage[7];
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }

typedef unsigned long pthread_t;
typedef struct { int unused; } pthread_attr_t;

int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument);
int pthread_detach(pthread_t thread);

#endif
