/*
 * doctor.c - consumer process: takes the highest-priority waiting
 *            patient, "treats" it, records the waiting time.
 */
#include "common.h"

void doctor_main(er_shared_t *sh, int d, int logfd)
{
    const config_t *cfg = &sh->cfg;
    doc_stats_t *st = &sh->doc[d];           /* only we write here     */
    uint8_t *seen = sh->seen + (size_t)d * sh->ntrack;   /* our row   */
    int idle = 0;

    for (;;) {
        patient_t p;
        long long start = 0;

        if (!wa_next(sh, &p, &start, &st->c)) {
            /* Only nosync gets here: nothing found after the bounded
             * retries. Leave once the ambulances are done and the room
             * has looked empty for a few rounds. */
            if (sh->producers_done && ++idle >= NOSYNC_IDLE_EXIT) break;
            continue;
        }
        idle = 0;

        if (p.id == PILL_ID) {               /* F4: poison pill -> exit */
            st->pills++;
            break;
        }
        if (p.id < 1 || (size_t)p.id > sh->ntrack ||
            p.severity < 1 || p.severity > NSEV) {
            st->c.invalid++;                 /* garbage (nosync only)  */
            continue;
        }

        if (seen[p.id - 1] < 255) seen[p.id - 1]++;   /* exactly-once */

        int s = p.severity - 1;
        long long wait = start - p.arrival_ns;        /* start-arrival */
        st->treated[s]++;
        st->wait_sum_ns[s] += wait;
        if (wait > st->wait_max_ns[s]) st->wait_max_ns[s] = wait;

        log_event_t ev = { EV_TREAT, p.id, p.severity, p.ambulance, d, 0,
                           start - sh->t0_ns, wait };
        send_event(logfd, &ev);              /* F5: treatment starts   */

        if (cfg->verbose)
            vlog("[%9.3f ms] DOC %d treat id=%-5d sev=%d wait=%.3f ms\n",
                 (start - sh->t0_ns) / 1e6, d, p.id, p.severity, wait / 1e6);

        sleep_us(cfg->treat_ms[s] * 1000L);  /* "treat": 2/4/6 ms      */
        st->busy_ns += now_ns() - start;     /* for utilisation        */
    }
    st->sem_ops = g_sem_ops;
}
