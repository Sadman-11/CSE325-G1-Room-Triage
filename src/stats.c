/*
 * stats.c - timing helpers, summary, CHECK lines, CSV row
 */
#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);     /* not affected by NTP/date */
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void sleep_us(long us)
{
    if (us <= 0) return;
    struct timespec req = { us / 1000000L, (us % 1000000L) * 1000L };
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
        ;                                    /* resume after a signal  */
}

/* ------------------------------------------------------------------ */
/* Output helpers                                                      */
/* ------------------------------------------------------------------ */
/* One write() per line, so lines from different processes never mix
 * and never come out in the wrong order because of stdio buffering. */
void vlog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    if (write(STDOUT_FILENO, buf, (size_t)n) < 0) { /* ignore */ }
}

/* Send one fixed-size record to the logger (atomic: <= PIPE_BUF). */
void send_event(int fd, const log_event_t *ev)
{
    ssize_t w;
    do {
        w = write(fd, ev, sizeof *ev);
    } while (w == -1 && errno == EINTR);
    if (w != (ssize_t)sizeof *ev) die("write log pipe");
}

const char *mode_name(sync_mode_t m)
{
    switch (m) {
    case MODE_NOSYNC:   return "nosync";
    case MODE_SEM_FIFO: return "sem_fifo";
    case MODE_SEM:      return "sem";
    case MODE_MONITOR:  return "monitor";
    }
    return "?";
}

static long count_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;
    long n = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF)
        if (ch == '\n') n++;
    fclose(f);
    return n;
}

static void check_line(int ok, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int g_failed;                         /* number of FAIL lines    */

static void check_line(int ok, const char *name, const char *fmt, ...)
{
    char details[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(details, sizeof details, fmt, ap);
    va_end(ap);
    printf("CHECK %s: %s (%s)\n", name, ok ? "PASS" : "FAIL", details);
    if (!ok) g_failed++;
}

/* ------------------------------------------------------------------ */
/* Final report: called by the coordinator after every child exited.   */
/* Returns the number of failed checks.                                */
/* ------------------------------------------------------------------ */
int report_results(er_shared_t *sh, double elapsed_s)
{
    const config_t *cfg = &sh->cfg;
    const char *mname = mode_name(cfg->mode);
    int P = cfg->ambulances, D = cfg->doctors;

    /* ---- add up the per-process statistics ---- */
    long gen[NSEV] = { 0 }, trt[NSEV] = { 0 };
    long long wsum[NSEV] = { 0 }, wmax[NSEV] = { 0 };
    long long busy = 0, blocked = 0;
    long sem_ops = sh->coord_sem_ops;
    wa_counters_t c = { 0 };

    for (int a = 0; a < P; a++) {
        for (int s = 0; s < NSEV; s++) gen[s] += sh->amb[a].generated[s];
        blocked += sh->amb[a].blocked_ns;
        sem_ops += sh->amb[a].sem_ops;
        c.cap += sh->amb[a].c.cap;
        c.prio += sh->amb[a].c.prio;
        c.fifo += sh->amb[a].c.fifo;
        c.invalid += sh->amb[a].c.invalid;
        c.promoted += sh->amb[a].c.promoted;
    }
    for (int d = 0; d < D; d++) {
        for (int s = 0; s < NSEV; s++) {
            trt[s]  += sh->doc[d].treated[s];
            wsum[s] += sh->doc[d].wait_sum_ns[s];
            if (sh->doc[d].wait_max_ns[s] > wmax[s])
                wmax[s] = sh->doc[d].wait_max_ns[s];
        }
        busy += sh->doc[d].busy_ns;
        sem_ops += sh->doc[d].sem_ops;
        c.cap += sh->doc[d].c.cap;
        c.prio += sh->doc[d].c.prio;
        c.fifo += sh->doc[d].c.fifo;
        c.invalid += sh->doc[d].c.invalid;
        c.promoted += sh->doc[d].c.promoted;
    }
    long gen_all = gen[0] + gen[1] + gen[2];
    long trt_all = trt[0] + trt[1] + trt[2];
    long long wsum_all = wsum[0] + wsum[1] + wsum[2];
    long long wmax_all = wmax[0];
    for (int s = 1; s < NSEV; s++) if (wmax[s] > wmax_all) wmax_all = wmax[s];

    /* ---- exactly-once: every generated id treated exactly once ---- */
    long lost = 0, dup = 0, phantom = 0;
    for (int a = 0; a < P; a++) {
        long ngen = sh->amb[a].generated[0] + sh->amb[a].generated[1] +
                    sh->amb[a].generated[2];
        for (long k = 0; k < cfg->id_stride; k++) {
            size_t idx = (size_t)a * (size_t)cfg->id_stride + (size_t)k;
            long times = 0;
            for (int d = 0; d < D; d++)
                times += sh->seen[(size_t)d * sh->ntrack + idx];
            if (k < ngen) {
                if (times == 0) lost++;
                else if (times > 1) dup += times - 1;
            } else if (times > 0) {
                phantom += times;            /* id never generated     */
            }
        }
    }
    long log_lines = count_lines(cfg->log_path);
    int  log_ok = (log_lines == 2 * gen_all);
    long violations = c.cap + c.prio + c.fifo + c.invalid + lost + dup +
                      phantom + (log_ok ? 0 : 1);

    double avg[NSEV], avg_all = trt_all ? wsum_all / 1e6 / trt_all : 0;
    for (int s = 0; s < NSEV; s++) avg[s] = trt[s] ? wsum[s] / 1e6 / trt[s] : 0;
    double util = elapsed_s > 0 ? busy / 1e9 / (D * elapsed_s) : 0;
    double thr  = elapsed_s > 0 ? trt_all / elapsed_s : 0;
    double ops_per_item = trt_all ? (double)sem_ops / trt_all : 0;

    /* ---- F6: summary ---- */
    printf("=== ER triage summary: mode=%s P=%d D=%d Q=%d seed=%u gap=0-%dms"
           " aging=%ldms ===\n", mname, P, D, cfg->beds, cfg->seed,
           cfg->gap_ms, cfg->aging_ms);
    printf("elapsed             : %.3f s\n", elapsed_s);
    printf("patients generated  : %ld (sev1 %ld, sev2 %ld, sev3 %ld)\n",
           gen_all, gen[0], gen[1], gen[2]);
    printf("patients treated    : %ld\n", trt_all);
    printf("throughput          : %.1f patients/s\n", thr);
    printf("severity   treated   avg_wait_ms   max_wait_ms\n");
    for (int s = 0; s < NSEV; s++)
        printf("   %d      %7ld   %11.3f   %11.3f\n", s + 1, trt[s], avg[s],
               wmax[s] / 1e6);
    printf("  all      %7ld   %11.3f   %11.3f\n", trt_all, avg_all,
           wmax_all / 1e6);
    printf("doctor utilisation  : %.1f %% (", util * 100);
    for (int d = 0; d < D; d++)
        printf("%sdoc%d %.1f%%", d ? ", " : "", d,
               elapsed_s > 0 ? sh->doc[d].busy_ns / 1e9 / elapsed_s * 100 : 0);
    printf(")\n");
    printf("ambulances blocked  : %.1f ms in total\n", blocked / 1e6);
    printf("semaphore ops       : %ld (%.2f per treated patient)\n",
           sem_ops, ops_per_item);
    if (cfg->aging_ms > 0)
        printf("aging promotions    : %ld\n", c.promoted);

    /* ---- self-checks (Section 3.3) ---- */
    g_failed = 0;
    check_line(trt_all == gen_all, "conservation",
               "%s: %ld treated, %ld generated", mname, trt_all, gen_all);
    check_line(lost == 0 && dup == 0 && phantom == 0, "exactly_once",
               "%s: %ld lost, %ld duplicated, %ld unknown ids", mname,
               lost, dup, phantom);
    check_line(c.cap == 0, "capacity",
               "%s: %ld times waiting was outside 0..%d", mname, c.cap,
               cfg->beds);
    if (cfg->mode == MODE_SEM_FIFO)
        check_line(c.fifo == 0, "priority",
                   "%s: severity ignored by design; %ld FIFO-order "
                   "violations", mname, c.fifo);
    else
        check_line(c.prio == 0 && c.fifo == 0, "priority",
                   "%s: %ld priority violations, %ld FIFO-order violations",
                   mname, c.prio, c.fifo);
    check_line(log_ok, "log", "%s: %ld lines in %s, expected %ld", mname,
               log_lines, cfg->log_path, 2 * gen_all);
    if (c.invalid > 0)
        printf("note: %ld reads of an empty slot or a garbage id\n",
               c.invalid);
    fflush(stdout);

    /* ---- one CSV row ---- */
    if (cfg->csv_path[0]) {
        struct stat stbuf;
        int need_header = (stat(cfg->csv_path, &stbuf) != 0 ||
                           stbuf.st_size == 0);
        FILE *f = fopen(cfg->csv_path, "a");
        if (f == NULL) {
            perror("fopen csv");
        } else {
            if (need_header)
                fprintf(f, "group,experiment,mode,param_name,param_value,run,"
                           "elapsed_s,ops,throughput_ops_s,avg_wait_ms,"
                           "max_wait_ms,violations,lost,duplicated,"
                           "wait_sev1_ms,wait_sev2_ms,wait_sev3_ms,"
                           "max_sev1_ms,max_sev2_ms,max_sev3_ms,"
                           "amb_blocked_ms,doctor_util,sem_ops,"
                           "sem_ops_per_item,P,D,Q,gap_ms,aging_ms,"
                           "promoted,checks_failed\n");
            fprintf(f, "%d,%s,%s,%s,%s,%d,%.6f,%ld,%.3f,%.4f,%.4f,%ld,%ld,"
                       "%ld,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.4f,%ld,"
                       "%.3f,%d,%d,%d,%d,%ld,%ld,%d\n",
                    GROUP_ID, cfg->experiment, mname, cfg->param_name,
                    cfg->param_value, cfg->run, elapsed_s, trt_all, thr,
                    avg_all, wmax_all / 1e6, violations, lost, dup,
                    avg[0], avg[1], avg[2], wmax[0] / 1e6, wmax[1] / 1e6,
                    wmax[2] / 1e6, blocked / 1e6, util, sem_ops,
                    ops_per_item, P, D, cfg->beds, cfg->gap_ms,
                    cfg->aging_ms, c.promoted, g_failed);
            fclose(f);
        }
    }
    return g_failed;
}
