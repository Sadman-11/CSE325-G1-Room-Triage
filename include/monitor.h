/*
 * monitor.h - a Hoare-style monitor built only from POSIX semaphores.
 *
 * Construction from Silberschatz, Operating System Concepts, Section 6.7
 * (signal-and-wait semantics). Every monitor_t and cond_t must live inside
 * shared memory and be initialised once, before fork(), with pshared = 1.
 */
#ifndef MONITOR_H
#define MONITOR_H

#include <semaphore.h>

typedef struct {
    sem_t mutex;      /* 1: at most one process is inside the monitor      */
    sem_t next;       /* 0: signallers suspend here (signal-and-wait)      */
    int   next_count; /* how many signallers are suspended on next         */
} monitor_t;

typedef struct {
    sem_t sem;        /* 0: processes waiting on this condition block here */
    int   count;      /* how many processes are waiting on this condition  */
} cond_t;

void mon_init(monitor_t *m);                  /* mutex = 1, next = 0        */
void mon_destroy(monitor_t *m);
void mon_enter(monitor_t *m);                 /* wait(mutex)                */
void mon_leave(monitor_t *m);                 /* next_count>0 ? post(next)
                                                              : post(mutex) */
void cond_init(cond_t *c);                    /* sem = 0, count = 0         */
void cond_destroy(cond_t *c);
void cond_wait(monitor_t *m, cond_t *c);      /* count++; release monitor;
                                                 wait(sem); count--         */
void cond_signal(monitor_t *m, cond_t *c);    /* if count>0: next_count++;
                                                 post(sem); wait(next);
                                                 next_count--               */

/* cond_timedwait() is not provided: the ER scenario has no timeouts
 * (a doctor rests until a patient arrives, an ambulance waits until a
 * bed is free), so Section 7 does not require it. */

#endif /* MONITOR_H */
