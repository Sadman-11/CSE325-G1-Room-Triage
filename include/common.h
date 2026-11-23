/*
 * common.h - shared structs, constants, IPC names and prototypes
 * CSE325 Group 1 - Emergency Room Triage
 */
#ifndef COMMON_H
#define COMMON_H

#include <limits.h>      /* PIPE_BUF */
#include <semaphore.h>
#include <signal.h>      /* sig_atomic_t */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "monitor.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define GROUP_ID          1
#define SHM_PREFIX        "/cse325_g1_er_"   /* + coordinator pid      */
#define MAX_BEDS          256    /* largest Q we accept               */
#define MAX_AMBULANCES    32     /* largest P                         */
#define MAX_DOCTORS       32     /* largest D                         */
#define NSEV              3      /* severities 1, 2, 3                */
#define SECONDS_MODE_CAP  50000  /* max patients/ambulance, --seconds */
#define PILL_ID           (-1)   /* poison pill: "doctor, go home"    */
#define EMPTY_ID          0      /* an empty / already-taken slot     */

/* nosync mode only: bounded retry loop instead of blocking */
#define NOSYNC_RETRIES    20     /* tries before giving up            */
#define NOSYNC_POLL_US    100    /* sleep between tries               */
#define NOSYNC_IDLE_EXIT  5      /* empty rounds after the end -> exit */

/* ------------------------------------------------------------------ */
/* Configuration (filled from the command line by main.c)             */
/* ------------------------------------------------------------------ */
typedef enum {
    MODE_NOSYNC = 0,   /* no synchronization at all (baseline)        */
    MODE_SEM_FIFO,     /* semaphores, one FIFO queue, severity ignored */
    MODE_SEM,          /* semaphores, three severity queues            */
    MODE_MONITOR       /* monitor ERoom built from semaphores          */
} sync_mode_t;

typedef struct {
    sync_mode_t mode;
    int    ambulances;              /* P                               */
    int    doctors;                 /* D                               */
    int    beds;                    /* Q                               */
    int    patients_per_amb;        /* N                               */
    long   items;                   /* --items: total patients (0=off) */
    double seconds;                 /* --seconds: run time (0 = off)   */
    int    mix[NSEV];               /* % of severity 1, 2, 3           */
    int    gap_ms;                  /* max random gap between arrivals */
    int    treat_ms[NSEV];          /* treatment time per severity     */
    long   aging_ms;                /* bonus: promote after A ms (0=off) */
    unsigned seed;
    int    verbose;
    int    run;                     /* labels for the CSV row          */
    char   experiment[32];
    char   param_name[32];
    char   param_value[32];
    char   csv_path[256];
    char   log_path[256];
    int    quota[MAX_AMBULANCES];   /* patients ambulance a generates  */
    int    id_stride;               /* ids of ambulance a: a*stride+1.. */
} config_t;

/* ------------------------------------------------------------------ */
/* Data that moves through the system                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    int       id;          /* unique (>0); PILL_ID; EMPTY_ID = no patient */
    int       severity;    /* 1 critical, 2 urgent, 3 minor (original)    */
    int       ambulance;   /* which ambulance brought the patient         */
    int       qseq;        /* order of entry into its queue (FIFO check)  */
    long long arrival_ns;  /* CLOCK_MONOTONIC time the patient arrived    */
} patient_t;

/* Counters a process keeps about the invariants it observed.
 * Each process only ever writes its own copy -> no race on statistics. */
typedef struct {
    long cap;        /* waiting < 0 or > Q seen at an insert/remove      */
    long prio;       /* removed sev s while a smaller number was waiting */
    long fifo;       /* removed out of arrival order inside one queue    */
    long invalid;    /* nosync: read an empty slot / garbage id          */
    long promoted;   /* bonus: minor patients promoted by aging          */
} wa_counters_t;

typedef struct {                    /* written only by ambulance a     */
    long          generated[NSEV];
    long long     blocked_ns;       /* time spent inside admit()       */
    long          sem_ops;
    wa_counters_t c;
} amb_stats_t;

typedef struct {                    /* written only by doctor d        */
    long          treated[NSEV];
    long long     wait_sum_ns[NSEV];
    long long     wait_max_ns[NSEV];
    long long     busy_ns;          /* time spent treating             */
    long          sem_ops;
    long          pills;
    wa_counters_t c;
} doc_stats_t;

/* ------------------------------------------------------------------ */
/* The one shared-memory segment (created before fork)                 */
/* ------------------------------------------------------------------ */
typedef struct {
    config_t  cfg;                  /* read-only copy for the children */

    /* semaphore solution (modes sem_fifo, sem) */
    sem_t     empty;                /* free beds,        starts at Q   */
    sem_t     full;                 /* waiting patients, starts at 0   */
    sem_t     mutex;                /* protects the queues, starts 1   */

    /* monitor solution (mode monitor) */
    monitor_t mon;
    cond_t    not_full;
    cond_t    not_empty;

    /* the waiting area: three circular queues of capacity Q          */
    patient_t q[NSEV][MAX_BEDS];
    int       head[NSEV];
    int       tail[NSEV];
    int       cnt[NSEV];
    int       waiting;              /* total patients in all queues    */
    int       next_qseq;            /* next queue-entry number         */
    int       last_qseq[NSEV];      /* qseq last removed from queue s  */

    volatile int producers_done;    /* set by coordinator (nosync exit) */
    long long t0_ns;                /* run start (for log timestamps)  */

    amb_stats_t amb[MAX_AMBULANCES];
    doc_stats_t doc[MAX_DOCTORS];
    long        coord_sem_ops;

    size_t    ntrack;               /* ids tracked = P * id_stride     */
    uint8_t   seen[];               /* [D][ntrack]: times doctor d
                                       treated id (saturates at 255)   */
} er_shared_t;

/* ------------------------------------------------------------------ */
/* Log record sent through the pipe                                    */
/* ------------------------------------------------------------------ */
enum { EV_ADMIT = 1, EV_TREAT = 2 };

typedef struct {
    int       type;       /* EV_ADMIT or EV_TREAT                       */
    int       id;
    int       severity;
    int       ambulance;
    int       doctor;     /* -1 for admissions                         */
    int       waiting;    /* patients waiting right after admission    */
    long long t_ns;       /* event time since run start                */
    long long wait_ns;    /* treatment only: start - arrival           */
} log_event_t;

/* A write of at most PIPE_BUF bytes to a pipe is atomic (POSIX), so
 * records from many writers never interleave. */
_Static_assert(sizeof(log_event_t) <= PIPE_BUF, "log record too big");

/* ------------------------------------------------------------------ */
/* Prototypes                                                          */
/* ------------------------------------------------------------------ */
/* ipc.c */
extern long g_sem_ops;                    /* per process (copied on fork) */
extern volatile sig_atomic_t g_stop;      /* set by SIGINT in coordinator */
extern void (*g_on_stop)(void);           /* coordinator's cleanup path   */
void sem_P(sem_t *s);                     /* sem_wait + count + EINTR     */
void sem_V(sem_t *s);                     /* sem_post + count             */
er_shared_t *ipc_create(const config_t *cfg, char *name, size_t namelen,
                        size_t *size_out);
void ipc_destroy(er_shared_t *sh, const char *name, size_t size);
void die(const char *what);

/* waiting_area.c */
void wa_admit(er_shared_t *sh, const patient_t *p, wa_counters_t *c,
              int *waiting_after);
int  wa_next(er_shared_t *sh, patient_t *out, long long *start_ns,
             wa_counters_t *c);

/* roles */
void ambulance_main(er_shared_t *sh, int a, int logfd);
void doctor_main(er_shared_t *sh, int d, int logfd);
void logger_main(int readfd, const char *path);

/* stats.c */
long long now_ns(void);
void sleep_us(long us);
void vlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void send_event(int fd, const log_event_t *ev);
const char *mode_name(sync_mode_t m);
int  report_results(er_shared_t *sh, double elapsed_s);

#endif /* COMMON_H */
