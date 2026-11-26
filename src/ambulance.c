/*
 * ambulance.c - producer process: generates patients and admits them
 *               into the waiting area.
 */
#include "common.h"

#include <stdlib.h>

/* Draw a severity using the mix percentages (default 20/30/50). */
static int pick_severity(const config_t *cfg, unsigned *rs)
{
    int r = (int)(rand_r(rs) % 100u);        /* 0..99                  */
    if (r < cfg->mix[0]) return 1;
    if (r < cfg->mix[0] + cfg->mix[1]) return 2;
    return 3;
}

void ambulance_main(er_shared_t *sh, int a, int logfd)
{
    const config_t *cfg = &sh->cfg;
    amb_stats_t *st = &sh->amb[a];           /* only we write here     */

    /* --seed fixes every random choice; each ambulance gets its own
     * reproducible stream. */
    unsigned rs = cfg->seed * 1000003u + (unsigned)a * 7919u + 1u;
    long long deadline = 0;
    if (cfg->seconds > 0)
        deadline = sh->t0_ns + (long long)(cfg->seconds * 1e9);

    for (int k = 0; k < cfg->quota[a]; k++) {
        if (deadline && now_ns() >= deadline) break;   /* --seconds    */

        if (cfg->gap_ms > 0)                 /* random gap 0..gap ms   */
            sleep_us((long)(rand_r(&rs) % (unsigned)(cfg->gap_ms * 1000 + 1)));

        patient_t p = { 0 };                 /* F1: id, severity,      */
        p.id         = a * cfg->id_stride + k + 1;  /* ambulance,      */
        p.severity   = pick_severity(cfg, &rs);     /* arrival time    */
        p.ambulance  = a;
        p.arrival_ns = now_ns();
        st->generated[p.severity - 1]++;

        long long t1 = now_ns();
        int waiting = 0;
        wa_admit(sh, &p, &st->c, &waiting);  /* may block at the door  */
        long long t2 = now_ns();
        st->blocked_ns += t2 - t1;

        log_event_t ev = { EV_ADMIT, p.id, p.severity, a, -1, waiting,
                           t2 - sh->t0_ns, 0 };
        send_event(logfd, &ev);              /* F5: one pipe record    */

        if (cfg->verbose)
            vlog("[%9.3f ms] AMB %d admit id=%-5d sev=%d waiting=%d/%d"
                 " (door %.3f ms)\n", (t2 - sh->t0_ns) / 1e6, a, p.id,
                 p.severity, waiting, cfg->beds, (t2 - t1) / 1e6);
    }
    st->sem_ops = g_sem_ops;                 /* for experiment E5      */
}
