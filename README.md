# CSE325 Group 1 — Emergency Room Triage

Multi-producer, multi-consumer bounded buffer with priorities, built from
Linux processes and IPC (C11, `fork()`, POSIX shared memory, unnamed POSIX
semaphores, one anonymous pipe). No threads.

- **Ambulances** (P producer processes) generate patients with severity
  1 = critical, 2 = urgent, 3 = minor and admit them into a waiting area of
  Q beds (shared memory, one circular queue per severity).
- **Doctors** (D consumer processes) always take a critical patient first,
  then urgent, then minor; FIFO inside one severity.
- **Logger** (1 process) reads fixed-size records from a pipe and writes
  `er_log.txt` (one line per admission and per treatment).
- **Coordinator** (`main`) creates every IPC object, forks everyone, sends
  one poison pill per doctor at the end, prints statistics and `CHECK` lines,
  and removes every IPC object (also on Ctrl+C).

The problem is the classic **bounded-buffer (producer–consumer) problem**
with a priority rule on the consumer side.

## Build

```
make            # gcc -std=c11 -Wall -Wextra -O0 -g, zero warnings
make run        # one default demo (mode sem)
make demo       # short --verbose runs in sem and monitor mode
make test       # every correct mode once; prints only CHECK ... PASS lines
make nosync     # the baseline without synchronization: CHECKs FAIL
make clean      # binaries, logs, leftover /dev/shm objects
```

Requires Linux or WSL2 with gcc. On glibc older than 2.34 the semaphore
functions need `-pthread` and `shm_open` needs `-lrt`; both are already in
the Makefile as link flags only (no pthread functions are used).

## Modes (`--mode=`)

| mode       | synchronization                                   | queues |
|------------|---------------------------------------------------|--------|
| `nosync`   | none; bounded retry loop instead of blocking      | 3      |
| `sem_fifo` | semaphores `empty`, `full`, `mutex`               | 1 FIFO (severity ignored) |
| `sem`      | semaphores `empty`, `full`, `mutex`               | 3 (priority) |
| `monitor`  | monitor ERoom `admit()` / `next_patient()`, conditions `not_full` / `not_empty`, built with `src/monitor.c` | 3 (priority) |

## Options

| option | meaning | default |
|---|---|---|
| `--mode=NAME` | `nosync`, `sem_fifo`, `sem`, `monitor` | `sem` |
| `--ambulances=P` | producer processes (1–32) | 3 |
| `--doctors=D` | consumer processes (1–32) | 2 |
| `--beds=Q` | capacity of the waiting area (1–256) | 8 |
| `--patients=N` | patients per ambulance | 300 |
| `--items=N` | total patients, split over the ambulances (overrides `--patients`) | off |
| `--seconds=T` | ambulances generate for T seconds instead of a fixed count | off |
| `--mix=A,B,C` | percent of severity 1, 2, 3 (must sum to 100) | 20,30,50 |
| `--gap-ms=G` | random gap 0..G ms between two arrivals of one ambulance | 3 |
| `--treat-ms=A,B,C` | treatment time of severity 1, 2, 3 | 2,4,6 |
| `--aging-ms=A` | bonus: a minor patient waiting longer than A ms is promoted to urgent (`sem`, `monitor`) | 0 (off) |
| `--seed=S` | seed of every random choice | 325 |
| `--log=FILE` | event log | `er_log.txt` |
| `--csv=FILE` | append one result row to FILE (header written if the file is new) | off |
| `--experiment=`, `--param-name=`, `--param-value=`, `--run=` | labels written into the CSV row | `-` |
| `--verbose` | print every event (demo only; slows the run) | off |
| `--help` | usage | |

Unknown options print the usage message and exit with status 2. The program
exits with status 0 when every CHECK passes and 1 otherwise.

## Self-checks printed in every run

| CHECK | PASS when |
|---|---|
| `conservation` | patients treated = patients generated |
| `exactly_once` | every generated id treated exactly once (none lost, none duplicated) |
| `capacity` | 0 ≤ waiting ≤ Q at every insert and remove |
| `priority` | no doctor took severity s while a smaller severity number was waiting, and FIFO order inside each queue (in `sem_fifo`: FIFO order only) |
| `log` | lines in the log = 2 × patients |

## Experiments

```
scripts/run_experiments.sh          # E1..E5 + bonus, 5 repeats, ~7–8 min
scripts/run_experiments.sh 3        # 3 repeats
EXPS="E1 E4" scripts/run_experiments.sh
python3 scripts/summarize.py > results/summary.md   # median [min–max] tables
```

Writes `results/E1.csv` … `results/E5.csv`, `results/E4_aging.csv` (bonus),
`results/E5_loc.txt` (lines of code of each synchronization part) and
`results/machine.txt` (CPU, cores, RAM, OS, kernel, gcc, WSL2).

CSV columns: the 12 common columns
`group,experiment,mode,param_name,param_value,run,elapsed_s,ops,throughput_ops_s,avg_wait_ms,max_wait_ms,violations`
followed by
`lost,duplicated,wait_sev1_ms,wait_sev2_ms,wait_sev3_ms,max_sev1_ms,max_sev2_ms,max_sev3_ms,amb_blocked_ms,doctor_util,sem_ops,sem_ops_per_item,P,D,Q,gap_ms,aging_ms,promoted,checks_failed`.

Waiting time = time the doctor starts − time the patient arrived at the
door (so it includes time spent waiting outside when the room is full).

## Layout

```
include/common.h      shared structs, constants, IPC names, prototypes
include/monitor.h     monitor library interface (Section 7)
src/main.c            coordinator: options, IPC, fork, pills, wait, report
src/ambulance.c       producer process
src/doctor.c          consumer process
src/logger.c          pipe reader, writes er_log.txt
src/waiting_area.c    the bounded buffer and the four modes
src/monitor.c         monitor from semaphores (Hoare, signal-and-wait)
src/ipc.c             create / destroy shared memory and semaphores
src/stats.c           timing, summary, CHECK lines, CSV
scripts/              run_experiments.sh, summarize.py
results/              CSV files from our runs
report/               report PDF and screenshots (deliverable D2)
```

## Sample output (`./bin/er_triage --mode=sem`)

```
=== ER triage summary: mode=sem P=3 D=2 Q=8 seed=325 gap=0-3ms aging=0ms ===
elapsed             : 2.157 s
patients generated  : 900 (sev1 182, sev2 248, sev3 470)
patients treated    : 900
throughput          : 417.2 patients/s
severity   treated   avg_wait_ms   max_wait_ms
   1          182         7.870        15.284
   2          248         7.951        13.737
   3          470        39.392        61.616
  all          900        24.354        61.616
doctor utilisation  : 99.8 % (doc0 99.9%, doc1 99.6%)
ambulances blocked  : 4894.4 ms in total
semaphore ops       : 7216 (8.02 per treated patient)
CHECK conservation: PASS (sem: 900 treated, 900 generated)
CHECK exactly_once: PASS (sem: 0 lost, 0 duplicated, 0 unknown ids)
CHECK capacity: PASS (sem: 0 times waiting was outside 0..8)
CHECK priority: PASS (sem: 0 priority violations, 0 FIFO-order violations)
CHECK log: PASS (sem: 1800 lines in er_log.txt, expected 1800)
```

`--verbose` adds one line per event, for example:

```
[    1.846 ms] AMB 1 admit id=4     sev=1 waiting=1/8 (door 0.013 ms)
[    1.879 ms] DOC 1 treat id=4     sev=1 wait=0.046 ms
```

## Checking clean-up

```
./bin/er_triage --mode=monitor
ls /dev/shm | grep cse325 || echo "no objects left"
ipcs            # System V objects: we create none
```
