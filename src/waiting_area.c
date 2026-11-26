/*
 * waiting_area.c - the bounded buffer (Q beds, three severity queues)
 *                  and its four synchronization modes:
 *
 *   nosync   : no synchronization, bounded retry loop (baseline)
 *   sem_fifo : semaphores empty/full/mutex, one FIFO queue
 *   sem      : semaphores empty/full/mutex, three severity queues
 *   monitor  : monitor ERoom { admit(), next_patient() }
 *
 * Public entry points: wa_admit() (ambulance side, producer) and
 * wa_next() (doctor side, consumer).
 */
#include "common.h"

#include <sched.h>

/* ================================================================== */
/* Queue helpers. In the correct modes they are called ONLY inside   */
/* the critical section (mutex held, or inside the monitor).         */
/* ================================================================== */

/* Which queue holds a patient of severity sev? sem_fifo uses one queue. */
static int qidx(const er_shared_t *sh, int sev)
{
    return sh->cfg.mode == MODE_SEM_FIFO ? 0 : sev - 1;
}

/* Append p at the tail of queue s. Returns waiting after the insert. */
static int put_q(er_shared_t *sh, int s, patient_t p, wa_counters_t *c)
{
    int Q = sh->cfg.beds;
    p.qseq = sh->next_qseq++;              /* remember entry order       */
    sh->q[s][sh->tail[s]] = p;
    sh->tail[s] = (sh->tail[s] + 1) % Q;   /* circular buffer            */
    sh->cnt[s]++;
    sh->waiting++;
    if (sh->waiting < 0 || sh->waiting > Q) c->cap++;   /* CHECK capacity */
    return sh->waiting;
}

/* Remove the patient at the head of queue s. */
static patient_t take_q(er_shared_t *sh, int s, wa_counters_t *c)
{
    int Q = sh->cfg.beds;
    patient_t p = sh->q[s][sh->head[s]];
    sh->q[s][sh->head[s]].id = EMPTY_ID;   /* slot is free again         */
    sh->head[s] = (sh->head[s] + 1) % Q;
    sh->cnt[s]--;
    sh->waiting--;
    if (sh->waiting < 0 || sh->waiting > Q) c->cap++;   /* CHECK capacity */
    return p;
}

/* Index of the queue a doctor must serve next, or -1 if all are empty.
 * Priority rule (F2): severity 1 first, then 2, then 3. */
static int best_queue(const er_shared_t *sh)
{
    if (sh->cfg.mode == MODE_SEM_FIFO)
        return sh->cnt[0] > 0 ? 0 : -1;
    for (int s = 0; s < NSEV; s++)
        if (sh->cnt[s] > 0) return s;
    return -1;
}

/* CHECK priority (S4) and FIFO-inside-a-severity, for a patient p that
 * was just removed from queue s. */
static void check_order(er_shared_t *sh, int s, const patient_t *p,
                        wa_counters_t *c)
{
    if (p->id == PILL_ID) return;
    if (p->qseq < sh->last_qseq[s]) c->fifo++;      /* overtook someone  */
    sh->last_qseq[s] = p->qseq;
    if (sh->cfg.mode != MODE_SEM_FIFO)
        for (int k = 0; k < s; k++)                 /* a smaller severity */
            if (sh->cnt[k] > 0) c->prio++;          /* number was waiting */
}

/* Bonus (aging): a minor patient who has waited longer than aging_ms
 * moves from queue 3 to the tail of queue 2. Pills are never promoted
 * (they must stay behind every real patient). */
static void age_minor(er_shared_t *sh, long long now, wa_counters_t *c)
{
    if (sh->cfg.aging_ms <= 0) return;
    if (sh->cfg.mode != MODE_SEM && sh->cfg.mode != MODE_MONITOR) return;
    long long limit = sh->cfg.aging_ms * 1000000LL;
    while (sh->cnt[2] > 0) {
        patient_t *h = &sh->q[2][sh->head[2]];      /* oldest minor      */
        if (h->id == PILL_ID || now - h->arrival_ns <= limit) break;
        patient_t p = take_q(sh, 2, c);
        put_q(sh, 1, p, c);                         /* waiting unchanged */
        c->promoted++;
    }
}

/* Common body of "remove the best patient" (used by sem and monitor). */
static patient_t remove_best(er_shared_t *sh, long long now,
                             wa_counters_t *c)
{
    age_minor(sh, now, c);
    int s = best_queue(sh);
    if (s < 0) {                       /* impossible when synchronized  */
        c->invalid++;
        patient_t none = { .id = EMPTY_ID };
        return none;
    }
    patient_t p = take_q(sh, s, c);
    check_order(sh, s, &p, c);
    return p;
}

/* ==== BEGIN SEM ==== (semaphore solution: modes sem_fifo and sem) */

/* Producer: wait for a free bed, then insert under the mutex.
 * Order matters: empty BEFORE mutex, otherwise a full room deadlocks. */
static void sem_admit(er_shared_t *sh, const patient_t *p,
                      wa_counters_t *c, int *waiting_after)
{
    sem_P(&sh->empty);                         /* a bed must be free    */
    sem_P(&sh->mutex);                         /* enter critical section */
    *waiting_after = put_q(sh, qidx(sh, p->severity), *p, c);
    sem_V(&sh->mutex);                         /* leave critical section */
    sem_V(&sh->full);                          /* one more patient waits */
}

/* Consumer: wait for a patient, then pick the best one under the mutex. */
static void sem_next(er_shared_t *sh, patient_t *out, long long *start,
                     wa_counters_t *c)
{
    sem_P(&sh->full);                          /* someone must be waiting */
    sem_P(&sh->mutex);
    *start = now_ns();
    *out = remove_best(sh, *start, c);         /* priority choice inside */
    sem_V(&sh->mutex);
    sem_V(&sh->empty);                         /* one bed became free    */
}

/* ==== END SEM ==== */

/* ==== BEGIN MONITOR ==== (monitor ERoom: admit / next_patient) */

/* monitor procedure ERoom.admit(p) */
static void mon_admit(er_shared_t *sh, const patient_t *p,
                      wa_counters_t *c, int *waiting_after)
{
    mon_enter(&sh->mon);
    while (sh->waiting == sh->cfg.beds)        /* room full: wait       */
        cond_wait(&sh->mon, &sh->not_full);
    *waiting_after = put_q(sh, qidx(sh, p->severity), *p, c);
    cond_signal(&sh->mon, &sh->not_empty);     /* wake a resting doctor */
    mon_leave(&sh->mon);
}

/* monitor procedure ERoom.next_patient() */
static void mon_next(er_shared_t *sh, patient_t *out, long long *start,
                     wa_counters_t *c)
{
    mon_enter(&sh->mon);
    while (sh->waiting == 0)                   /* nobody waiting: rest  */
        cond_wait(&sh->mon, &sh->not_empty);
    *start = now_ns();
    *out = remove_best(sh, *start, c);         /* the priority "if"     */
    cond_signal(&sh->mon, &sh->not_full);      /* wake a waiting ambulance */
    mon_leave(&sh->mon);
}

/* ==== END MONITOR ==== */

/* ==== BEGIN NOSYNC ==== (baseline: same work, no synchronization) */

/* Every read-modify-write is written out as read; yield; write so that
 * the race window is wide enough to show up in a short run. The race is
 * there without the yield too - it is only rarer. */
static void nosync_admit(er_shared_t *sh, const patient_t *p,
                         wa_counters_t *c, int *waiting_after)
{
    int Q = sh->cfg.beds;
    for (int r = 0; r < NOSYNC_RETRIES && sh->waiting >= Q; r++)
        sleep_us(NOSYNC_POLL_US);              /* bounded retry, no block */

    int s = qidx(sh, p->severity);
    int t = sh->tail[s];                       /* read tail ...         */
    sched_yield();                             /* ... someone else runs */
    patient_t np = *p;
    np.qseq = sh->next_qseq++;
    sh->q[s][t] = np;                          /* may overwrite a patient */
    sh->tail[s] = (t + 1) % Q;
    sh->cnt[s]++;
    int w = sh->waiting;                       /* lost update possible  */
    sched_yield();
    sh->waiting = w + 1;
    if (w + 1 < 0 || w + 1 > Q) c->cap++;
    *waiting_after = w + 1;
}

/* Returns 1 with a patient, or 0 if nothing (valid) was found. */
static int nosync_next(er_shared_t *sh, patient_t *out, long long *start,
                       wa_counters_t *c)
{
    int Q = sh->cfg.beds;
    for (int r = 0; r < NOSYNC_RETRIES && sh->waiting <= 0; r++)
        sleep_us(NOSYNC_POLL_US);
    if (sh->waiting <= 0) return 0;            /* gave up for now       */

    *start = now_ns();
    int s = best_queue(sh);
    if (s < 0) {                               /* counters disagree     */
        sh->waiting--;
        c->invalid++;
        return 0;
    }
    int h = sh->head[s];                       /* two doctors may read  */
    patient_t p = sh->q[s][h];                 /* the same patient ...  */
    sched_yield();                             /* ... before either one */
    sh->q[s][h].id = EMPTY_ID;                 /* marks it as taken     */
    sh->head[s] = (h + 1) % Q;
    sh->cnt[s]--;
    int w = sh->waiting;
    sched_yield();
    sh->waiting = w - 1;
    if (w - 1 < 0 || w - 1 > Q) c->cap++;
    if (p.id == EMPTY_ID) {                    /* slot empty / taken    */
        c->invalid++;
        return 0;
    }
    check_order(sh, s, &p, c);
    *out = p;
    return 1;
}

/* ==== END NOSYNC ==== */

/* ================================================================== */
/* Public interface: dispatch on --mode                               */
/* ================================================================== */

void wa_admit(er_shared_t *sh, const patient_t *p, wa_counters_t *c,
              int *waiting_after)
{
    switch (sh->cfg.mode) {
    case MODE_NOSYNC:  nosync_admit(sh, p, c, waiting_after); break;
    case MODE_MONITOR: mon_admit(sh, p, c, waiting_after);    break;
    default:           sem_admit(sh, p, c, waiting_after);    break;
    }
}

/* Returns 1 if *out holds a patient (or a pill), 0 if nothing was
 * found (only possible in nosync). */
int wa_next(er_shared_t *sh, patient_t *out, long long *start_ns,
            wa_counters_t *c)
{
    switch (sh->cfg.mode) {
    case MODE_NOSYNC:
        return nosync_next(sh, out, start_ns, c);
    case MODE_MONITOR:
        mon_next(sh, out, start_ns, c);
        break;
    default:
        sem_next(sh, out, start_ns, c);
        break;
    }
    return out->id != EMPTY_ID;
}
