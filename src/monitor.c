/*
 * monitor.c - monitor library built from POSIX semaphores
 * (Silberschatz 10th ed., Section 6.7, Hoare / signal-and-wait semantics)
 *
 * All objects live in shared memory, so every semaphore is created with
 * pshared = 1. sem_P / sem_V (ipc.c) are sem_wait / sem_post that also
 * count operations (experiment E5) and retry after EINTR.
 */
#include "common.h"

void mon_init(monitor_t *m)
{
    if (sem_init(&m->mutex, 1, 1) == -1) die("sem_init(mon.mutex)");
    if (sem_init(&m->next, 1, 0) == -1)  die("sem_init(mon.next)");
    m->next_count = 0;
}

void mon_destroy(monitor_t *m)
{
    sem_destroy(&m->mutex);
    sem_destroy(&m->next);
}

/* Entry to every monitor procedure. */
void mon_enter(monitor_t *m)
{
    sem_P(&m->mutex);
}

/* Exit from every monitor procedure. If a signaller is suspended on
 * `next`, hand the monitor straight to it; otherwise open the door. */
void mon_leave(monitor_t *m)
{
    if (m->next_count > 0)
        sem_V(&m->next);
    else
        sem_V(&m->mutex);
}

void cond_init(cond_t *c)
{
    if (sem_init(&c->sem, 1, 0) == -1) die("sem_init(cond)");
    c->count = 0;
}

void cond_destroy(cond_t *c)
{
    sem_destroy(&c->sem);
}

/* Block on condition c and give the monitor away while blocked. */
void cond_wait(monitor_t *m, cond_t *c)
{
    c->count++;                    /* one more waiter on c             */
    if (m->next_count > 0)         /* release the monitor: a suspended */
        sem_V(&m->next);           /* signaller has priority,          */
    else
        sem_V(&m->mutex);          /* otherwise let a new process in   */
    sem_P(&c->sem);                /* sleep until someone signals c    */
    c->count--;                    /* we own the monitor again         */
}

/* Wake one waiter on c (if any) and wait on `next` until it leaves
 * the monitor or waits again: the woken process runs immediately,
 * so the condition it waited for is still true (Hoare semantics). */
void cond_signal(monitor_t *m, cond_t *c)
{
    if (c->count > 0) {            /* signal on an empty condition     */
        m->next_count++;           /* is a no-op                       */
        sem_V(&c->sem);            /* pass the monitor to the waiter   */
        sem_P(&m->next);           /* and suspend ourselves            */
        m->next_count--;
    }
}
