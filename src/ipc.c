/*
 * ipc.c - create, attach and destroy every IPC object
 *         (POSIX shared memory + unnamed POSIX semaphores inside it)
 */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

long g_sem_ops = 0;                   /* counted in every process      */
volatile sig_atomic_t g_stop = 0;     /* SIGINT seen by the coordinator */
void (*g_on_stop)(void) = NULL;       /* set only in the coordinator    */

void die(const char *what)
{
    perror(what);
    if (g_on_stop) g_on_stop();       /* coordinator: kill + clean up  */
    _exit(1);                         /* child: leave, no atexit/flush */
}

/* wait(): block until the semaphore is > 0, then decrement it.
 * Children simply block in sem_wait (Ctrl+C kills them).
 * The coordinator (g_on_stop set) blocks in sem_timedwait instead, so
 * that a Ctrl+C arriving just BEFORE it blocks is still noticed within
 * 200 ms; otherwise it could sleep forever on a semaphore that the
 * killed children will never post. */
void sem_P(sem_t *s)
{
    for (;;) {
        if (g_stop && g_on_stop) g_on_stop();   /* Ctrl+C: tear down */
        int r;
        if (g_on_stop) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts); /* timedwait wants an */
            ts.tv_nsec += 200000000L;           /* absolute time      */
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            r = sem_timedwait(s, &ts);
        } else {
            r = sem_wait(s);
        }
        if (r == 0) break;
        if (errno != EINTR && errno != ETIMEDOUT) die("sem_wait");
        /* interrupted or timed out: check g_stop and wait again */
    }
    g_sem_ops++;
}

/* signal(): increment the semaphore, waking one blocked process. */
void sem_V(sem_t *s)
{
    if (sem_post(s) == -1) die("sem_post");
    g_sem_ops++;
}

/* Create the shared segment, map it, initialise every semaphore with
 * pshared = 1. Must be called BEFORE fork() so children inherit it. */
er_shared_t *ipc_create(const config_t *cfg, char *name, size_t namelen,
                        size_t *size_out)
{
    size_t ntrack = (size_t)cfg->ambulances * (size_t)cfg->id_stride;
    size_t size   = sizeof(er_shared_t) + (size_t)cfg->doctors * ntrack;

    snprintf(name, namelen, "%s%d", SHM_PREFIX, (int)getpid());

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) die("shm_open");
    if (ftruncate(fd, (off_t)size) == -1) {       /* new bytes are 0  */
        shm_unlink(name);
        die("ftruncate");
    }
    er_shared_t *sh = mmap(NULL, size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
    if (sh == MAP_FAILED) {
        shm_unlink(name);
        die("mmap");
    }
    close(fd);                         /* the mapping stays valid      */

    memset(sh, 0, sizeof(er_shared_t)); /* seen[] is already zero      */
    sh->cfg    = *cfg;
    sh->ntrack = ntrack;
    for (int s = 0; s < NSEV; s++) sh->last_qseq[s] = -1;

    /* semaphore solution: empty = Q free beds, full = 0, mutex = 1 */
    if (sem_init(&sh->empty, 1, (unsigned)cfg->beds) == -1) die("sem_init(empty)");
    if (sem_init(&sh->full,  1, 0) == -1)                   die("sem_init(full)");
    if (sem_init(&sh->mutex, 1, 1) == -1)                   die("sem_init(mutex)");

    /* monitor solution */
    mon_init(&sh->mon);
    cond_init(&sh->not_full);
    cond_init(&sh->not_empty);

    *size_out = size;
    return sh;
}

/* Destroy semaphores, unmap and remove the name from /dev/shm. */
void ipc_destroy(er_shared_t *sh, const char *name, size_t size)
{
    if (sh == NULL) return;
    sem_destroy(&sh->empty);
    sem_destroy(&sh->full);
    sem_destroy(&sh->mutex);
    cond_destroy(&sh->not_full);
    cond_destroy(&sh->not_empty);
    mon_destroy(&sh->mon);
    munmap(sh, size);
    shm_unlink(name);
}
