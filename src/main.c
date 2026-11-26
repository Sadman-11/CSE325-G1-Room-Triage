/*
 * main.c - coordinator process
 *
 *   1. parse the command line
 *   2. create shared memory + semaphores + monitor (ipc_create)
 *   3. create the log pipe, fork the logger, the doctors, the ambulances
 *   4. wait for the ambulances, then send one poison pill per doctor
 *   5. wait for the doctors and the logger
 *   6. print the summary and the CHECK lines, append a CSV row
 *   7. destroy every IPC object (also on Ctrl+C)
 */
#include "common.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- state needed by the cleanup path ---- */
static er_shared_t *g_sh;
static char   g_shm_name[64];
static size_t g_shm_size;
static pid_t  g_child[1 + MAX_DOCTORS + MAX_AMBULANCES];
static int    g_nchild;

/* ------------------------------------------------------------------ */
/* Ctrl+C handling                                                     */
/* ------------------------------------------------------------------ */
/* The handler only does async-signal-safe work: set a flag and kill. */
static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
    for (int i = 0; i < g_nchild; i++)
        if (g_child[i] > 0) kill(g_child[i], SIGTERM);
}

/* Called from normal code (sem_P, wait_child, die) once g_stop is set:
 * reap every child, remove every IPC object, exit. */
static void emergency_stop(void)
{
    g_on_stop = NULL;                        /* no recursion            */
    for (int i = 0; i < g_nchild; i++) {
        if (g_child[i] <= 0) continue;
        kill(g_child[i], SIGTERM);
        while (waitpid(g_child[i], NULL, 0) == -1 && errno == EINTR)
            ;
        g_child[i] = 0;
    }
    ipc_destroy(g_sh, g_shm_name, g_shm_size);
    g_sh = NULL;
    const char msg[] = "\ner_triage: interrupted - children stopped, "
                       "IPC objects removed\n";
    if (write(STDERR_FILENO, msg, sizeof msg - 1) < 0) { /* ignore */ }
    exit(130);
}

static void set_signal(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                         /* no SA_RESTART: blocking */
    if (sigaction(sig, &sa, NULL) == -1)     /* calls return EINTR      */
        die("sigaction");
}

/* Wait for one child; on Ctrl+C go to the cleanup path. */
static void wait_child(int i)
{
    int status;
    while (waitpid(g_child[i], &status, 0) == -1) {
        if (errno != EINTR) die("waitpid");
        if (g_stop) emergency_stop();
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fprintf(stderr, "warning: child %d (pid %d) ended abnormally\n",
                i, (int)g_child[i]);
    g_child[i] = 0;                          /* reaped: no zombie       */
    if (g_stop) emergency_stop();            /* Ctrl+C while it ended   */
}

/* fork() a child that runs role(sh, idx, fd) and then exits. */
static pid_t spawn(int kind, int idx, int pipefd[2])
{
    fflush(stdout);                          /* don't copy stdio buffers */
    fflush(stderr);
    pid_t pid = fork();
    if (pid == -1) die("fork");
    if (pid == 0) {                          /* ---- child ---- */
        g_on_stop = NULL;                    /* only the parent cleans */
        g_sem_ops = 0;
        set_signal(SIGINT, SIG_DFL);         /* Ctrl+C kills children  */
        set_signal(SIGTERM, SIG_DFL);
        set_signal(SIGHUP, SIG_DFL);
        if (kind == 0) {                     /* logger: reads only     */
            close(pipefd[1]);
            logger_main(pipefd[0], g_sh->cfg.log_path);
        } else {                             /* writers: write only    */
            close(pipefd[0]);
            if (kind == 1) doctor_main(g_sh, idx, pipefd[1]);
            else           ambulance_main(g_sh, idx, pipefd[1]);
            close(pipefd[1]);
        }
        _exit(0);       /* _exit: no atexit handlers, no stdio flush  */
    }
    g_child[g_nchild++] = pid;
    return pid;
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --mode=NAME        nosync | sem_fifo | sem | monitor  (default sem)\n"
        "  --ambulances=P     producer processes              (default 3)\n"
        "  --doctors=D        consumer processes              (default 2)\n"
        "  --beds=Q           capacity of the waiting area    (default 8)\n"
        "  --patients=N       patients per ambulance          (default 300)\n"
        "  --items=N          total patients (split over the ambulances)\n"
        "  --seconds=T        ambulances generate for T seconds instead\n"
        "  --mix=A,B,C        %% of severity 1,2,3             (default 20,30,50)\n"
        "  --gap-ms=G         random gap 0..G ms between arrivals (default 3)\n"
        "  --treat-ms=A,B,C   treatment time per severity     (default 2,4,6)\n"
        "  --aging-ms=A       bonus: promote minor patients after A ms (0=off)\n"
        "  --seed=S           random seed                     (default 325)\n"
        "  --log=FILE         event log file                  (default er_log.txt)\n"
        "  --csv=FILE         append one result row to FILE\n"
        "  --experiment=NAME --param-name=X --param-value=V --run=K  CSV labels\n"
        "  --verbose          print every event\n"
        "  --help             this message\n", prog);
}

static int parse_triple(const char *s, int out[NSEV])
{
    return sscanf(s, "%d,%d,%d", &out[0], &out[1], &out[2]) == 3;
}

static void parse_args(int argc, char **argv, config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->mode = MODE_SEM;
    cfg->ambulances = 3;
    cfg->doctors = 2;
    cfg->beds = 8;
    cfg->patients_per_amb = 300;
    cfg->mix[0] = 20; cfg->mix[1] = 30; cfg->mix[2] = 50;
    cfg->gap_ms = 3;
    cfg->treat_ms[0] = 2; cfg->treat_ms[1] = 4; cfg->treat_ms[2] = 6;
    cfg->seed = 325;
    cfg->run = 1;
    strcpy(cfg->log_path, "er_log.txt");
    strcpy(cfg->experiment, "-");
    strcpy(cfg->param_name, "-");
    strcpy(cfg->param_value, "-");

    enum { O_MODE = 1000, O_AMB, O_DOC, O_BEDS, O_PAT, O_ITEMS, O_SECS,
           O_MIX, O_GAP, O_TREAT, O_AGING, O_SEED, O_LOG, O_CSV, O_EXP,
           O_PNAME, O_PVAL, O_RUN, O_VERB, O_HELP };
    static const struct option opts[] = {
        { "mode",        required_argument, 0, O_MODE  },
        { "ambulances",  required_argument, 0, O_AMB   },
        { "doctors",     required_argument, 0, O_DOC   },
        { "beds",        required_argument, 0, O_BEDS  },
        { "patients",    required_argument, 0, O_PAT   },
        { "items",       required_argument, 0, O_ITEMS },
        { "seconds",     required_argument, 0, O_SECS  },
        { "mix",         required_argument, 0, O_MIX   },
        { "gap-ms",      required_argument, 0, O_GAP   },
        { "treat-ms",    required_argument, 0, O_TREAT },
        { "aging-ms",    required_argument, 0, O_AGING },
        { "seed",        required_argument, 0, O_SEED  },
        { "log",         required_argument, 0, O_LOG   },
        { "csv",         required_argument, 0, O_CSV   },
        { "experiment",  required_argument, 0, O_EXP   },
        { "param-name",  required_argument, 0, O_PNAME },
        { "param-value", required_argument, 0, O_PVAL  },
        { "run",         required_argument, 0, O_RUN   },
        { "verbose",     no_argument,       0, O_VERB  },
        { "help",        no_argument,       0, O_HELP  },
        { 0, 0, 0, 0 }
    };

    int ch;
    while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (ch) {
        case O_MODE:
            if      (!strcmp(optarg, "nosync"))   cfg->mode = MODE_NOSYNC;
            else if (!strcmp(optarg, "sem_fifo")) cfg->mode = MODE_SEM_FIFO;
            else if (!strcmp(optarg, "sem"))      cfg->mode = MODE_SEM;
            else if (!strcmp(optarg, "monitor"))  cfg->mode = MODE_MONITOR;
            else { fprintf(stderr, "unknown mode '%s'\n", optarg);
                   usage(argv[0]); exit(2); }
            break;
        case O_AMB:   cfg->ambulances = atoi(optarg); break;
        case O_DOC:   cfg->doctors = atoi(optarg); break;
        case O_BEDS:  cfg->beds = atoi(optarg); break;
        case O_PAT:   cfg->patients_per_amb = atoi(optarg); break;
        case O_ITEMS: cfg->items = atol(optarg); break;
        case O_SECS:  cfg->seconds = atof(optarg); break;
        case O_MIX:
            if (!parse_triple(optarg, cfg->mix)) { usage(argv[0]); exit(2); }
            break;
        case O_GAP:   cfg->gap_ms = atoi(optarg); break;
        case O_TREAT:
            if (!parse_triple(optarg, cfg->treat_ms)) { usage(argv[0]); exit(2); }
            break;
        case O_AGING: cfg->aging_ms = atol(optarg); break;
        case O_SEED:  cfg->seed = (unsigned)strtoul(optarg, NULL, 10); break;
        case O_LOG:   snprintf(cfg->log_path, sizeof cfg->log_path, "%s", optarg); break;
        case O_CSV:   snprintf(cfg->csv_path, sizeof cfg->csv_path, "%s", optarg); break;
        case O_EXP:   snprintf(cfg->experiment, sizeof cfg->experiment, "%s", optarg); break;
        case O_PNAME: snprintf(cfg->param_name, sizeof cfg->param_name, "%s", optarg); break;
        case O_PVAL:  snprintf(cfg->param_value, sizeof cfg->param_value, "%s", optarg); break;
        case O_RUN:   cfg->run = atoi(optarg); break;
        case O_VERB:  cfg->verbose = 1; break;
        case O_HELP:  usage(argv[0]); exit(0);
        default:      usage(argv[0]); exit(2);   /* unknown option */
        }
    }
    if (optind < argc) { usage(argv[0]); exit(2); }

    /* ---- validate ---- */
    const char *err = NULL;
    if (cfg->ambulances < 1 || cfg->ambulances > MAX_AMBULANCES) err = "--ambulances out of range (1..32)";
    else if (cfg->doctors < 1 || cfg->doctors > MAX_DOCTORS)     err = "--doctors out of range (1..32)";
    else if (cfg->beds < 1 || cfg->beds > MAX_BEDS)              err = "--beds out of range (1..256)";
    else if (cfg->patients_per_amb < 0)                          err = "--patients must be >= 0";
    else if (cfg->items < 0 || cfg->items > 10000000)            err = "--items out of range";
    else if (cfg->seconds < 0)                                   err = "--seconds must be >= 0";
    else if (cfg->gap_ms < 0 || cfg->gap_ms > 1000)              err = "--gap-ms out of range (0..1000)";
    else if (cfg->aging_ms < 0)                                  err = "--aging-ms must be >= 0";
    else if (cfg->mix[0] < 0 || cfg->mix[1] < 0 || cfg->mix[2] < 0 ||
             cfg->mix[0] + cfg->mix[1] + cfg->mix[2] != 100)      err = "--mix must be 3 values that sum to 100";
    else if (cfg->treat_ms[0] < 0 || cfg->treat_ms[1] < 0 || cfg->treat_ms[2] < 0)
                                                                 err = "--treat-ms must be >= 0";
    if (err) { fprintf(stderr, "error: %s\n", err); usage(argv[0]); exit(2); }

    /* ---- how many patients each ambulance generates ---- */
    int P = cfg->ambulances, maxq = 1;
    for (int a = 0; a < P; a++) {
        if (cfg->seconds > 0)
            cfg->quota[a] = SECONDS_MODE_CAP;        /* stop by time  */
        else if (cfg->items > 0)
            cfg->quota[a] = (int)(cfg->items / P + (a < cfg->items % P));
        else
            cfg->quota[a] = cfg->patients_per_amb;
        if (cfg->quota[a] > maxq) maxq = cfg->quota[a];
    }
    cfg->id_stride = maxq;                   /* ids never collide      */
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    config_t cfg;
    parse_args(argc, argv, &cfg);

    /* 1. IPC objects - before any fork() */
    g_sh = ipc_create(&cfg, g_shm_name, sizeof g_shm_name, &g_shm_size);
    g_on_stop = emergency_stop;              /* die()/Ctrl+C clean up  */
    set_signal(SIGINT, on_sigint);
    set_signal(SIGTERM, on_sigint);
    set_signal(SIGHUP, on_sigint);           /* terminal closed        */
    set_signal(SIGPIPE, SIG_IGN);            /* e.g. "| head": a failed
                                                write must not kill us
                                                before clean-up (children
                                                inherit this)           */

    int pipefd[2];
    if (pipe(pipefd) == -1) die("pipe");

    if (cfg.verbose)
        vlog("coordinator: mode=%s P=%d D=%d Q=%d shm=%s\n",
             mode_name(cfg.mode), cfg.ambulances, cfg.doctors, cfg.beds,
             g_shm_name);

    /* 2. fork: logger, doctors, ambulances */
    spawn(0, 0, pipefd);                     /* g_child[0] = logger    */
    long long t_start = now_ns();
    g_sh->t0_ns = t_start;                   /* set before the writers */
    for (int d = 0; d < cfg.doctors; d++)    /* g_child[1..D]          */
        spawn(1, d, pipefd);
    for (int a = 0; a < cfg.ambulances; a++) /* g_child[D+1..D+P]      */
        spawn(2, a, pipefd);
    close(pipefd[0]);                        /* coordinator neither    */
    close(pipefd[1]);                        /* reads nor writes logs  */
    if (g_stop) emergency_stop();

    /* 3. wait until every ambulance has finished */
    int first_amb = 1 + cfg.doctors;
    for (int i = 0; i < cfg.ambulances; i++)
        wait_child(first_amb + i);
    g_sh->producers_done = 1;

    /* 4. F4: one poison pill per doctor (behind every real patient) */
    long saved_ops = g_sem_ops;
    for (int d = 0; d < cfg.doctors; d++) {
        patient_t pill = { .id = PILL_ID, .severity = 3, .ambulance = -1,
                           .arrival_ns = now_ns() };
        wa_counters_t pc = { 0 };
        int w;
        wa_admit(g_sh, &pill, &pc, &w);
        g_sh->amb[0].c.cap += pc.cap;        /* count, never lose      */
    }
    g_sh->coord_sem_ops = g_sem_ops - saved_ops;

    /* 5. wait for the doctors, then the logger (EOF after writers end) */
    for (int d = 0; d < cfg.doctors; d++)
        wait_child(1 + d);
    long long t_end = now_ns();
    wait_child(0);

    /* 6. results */
    double elapsed = (t_end - t_start) / 1e9;
    int failed = report_results(g_sh, elapsed);

    /* 7. clean-up: S5 */
    g_on_stop = NULL;
    ipc_destroy(g_sh, g_shm_name, g_shm_size);
    g_sh = NULL;
    return failed ? 1 : 0;
}
