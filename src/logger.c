/*
 * logger.c - one reader of the event pipe; writes er_log.txt
 *
 * Many writers (ambulances, doctors) share the write end of one
 * anonymous pipe. Each record is <= PIPE_BUF bytes, so each write() is
 * atomic and records never interleave. read() returns 0 (EOF) only when
 * every write end is closed, i.e. when all writers have exited.
 */
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

/* Read exactly n bytes. Returns 1 on success, 0 on EOF. */
static int read_full(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0;                       /* EOF            */
        if (r < 0) {
            if (errno == EINTR) continue;
            die("logger read");
        }
        got += (size_t)r;
    }
    return 1;
}

void logger_main(int readfd, const char *path)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) die("fopen log");

    log_event_t ev;
    while (read_full(readfd, &ev, sizeof ev)) {
        if (ev.type == EV_ADMIT)
            fprintf(f, "ADMIT t=%.3fms id=%d sev=%d amb=%d waiting=%d\n",
                    ev.t_ns / 1e6, ev.id, ev.severity, ev.ambulance,
                    ev.waiting);
        else
            fprintf(f, "TREAT t=%.3fms id=%d sev=%d amb=%d doctor=%d "
                       "wait=%.3fms\n", ev.t_ns / 1e6, ev.id, ev.severity,
                    ev.ambulance, ev.doctor, ev.wait_ns / 1e6);
    }
    if (fclose(f) != 0) die("fclose log");
    close(readfd);
}
