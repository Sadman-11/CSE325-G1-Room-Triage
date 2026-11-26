#!/usr/bin/env bash
# run_experiments.sh - runs E1..E5 (and the bonus aging run), writes
# results/E1.csv ... results/E5.csv, results/E4_aging.csv,
# results/E5_loc.txt, results/machine.txt
#
# usage: scripts/run_experiments.sh [REPS]        (default REPS = 5)
#        EXPS="E1 E3" scripts/run_experiments.sh  (run only some)
set -u
cd "$(dirname "$0")/.."                 # repository root

REPS=${1:-5}
EXPS=${EXPS:-"E1 E2 E3 E4 E5 BONUS"}
BIN=./bin/er_triage
OUT=results
LOG=/tmp/er_triage_exp_$$.log           # event log of each run (scratch)

make -s >/dev/null || { echo "build failed"; exit 1; }
mkdir -p "$OUT"

# ---- record the machine (Section 4.1) ---------------------------------
{
    echo "date:   $(date)"
    echo "cpu:    $(lscpu 2>/dev/null | sed -n 's/^Model name: *//p')"
    echo "cores:  $(nproc)"
    echo "ram:    $(free -h 2>/dev/null | awk '/^Mem:/ {print $2}')"
    echo "os:     $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
    echo "kernel: $(uname -r)"
    echo "gcc:    $(gcc --version | head -1)"
    if grep -qi microsoft /proc/version 2>/dev/null; then
        echo "wsl2:   yes"; else echo "wsl2:   no"; fi
} > "$OUT/machine.txt"

# one ROW = one run: run EXP CSVFILE PARAM_NAME PARAM_VALUE REP [options]
run() {
    local exp=$1 csv=$2 pname=$3 pval=$4 rep=$5; shift 5
    $BIN --experiment="$exp" --param-name="$pname" --param-value="$pval" \
         --run="$rep" --seed=$((325 + rep)) --csv="$csv" --log="$LOG" \
         "$@" > /dev/null 2>&1
    # nosync is expected to fail its checks: never stop the script
}

want() { [[ " $EXPS " == *" $1 "* ]]; }

# ---- E1: why synchronization? -----------------------------------------
if want E1; then
    echo "E1: nosync vs sem, P=D in {1,2,4}"
    rm -f "$OUT/E1.csv"
    for mode in nosync sem; do
      for pd in 1 2 4; do
        for r in $(seq 1 "$REPS"); do
          run E1 "$OUT/E1.csv" P=D "$pd" "$r" --mode=$mode \
              --ambulances=$pd --doctors=$pd
        done
      done
    done
fi

# ---- E2: size of the waiting area -------------------------------------
if want E2; then
    echo "E2: Q in {1,2,4,8,16,32}, sem and monitor"
    rm -f "$OUT/E2.csv"
    for mode in sem monitor; do
      for q in 1 2 4 8 16 32; do
        for r in $(seq 1 "$REPS"); do
          run E2 "$OUT/E2.csv" Q "$q" "$r" --mode=$mode --beds=$q
        done
      done
    done
fi

# ---- E3: number of doctors --------------------------------------------
if want E3; then
    echo "E3: D in {1,2,3,4,6}, fixed load"
    rm -f "$OUT/E3.csv"
    for d in 1 2 3 4 6; do
      for r in $(seq 1 "$REPS"); do
        run E3 "$OUT/E3.csv" D "$d" "$r" --mode=sem --doctors=$d
      done
    done
fi

# ---- E4: FIFO vs priority under load ----------------------------------
if want E4; then
    echo "E4: sem_fifo / sem / monitor, gap in {0,1,3} ms"
    rm -f "$OUT/E4.csv"
    for mode in sem_fifo sem monitor; do
      for g in 0 1 3; do
        for r in $(seq 1 "$REPS"); do
          run E4 "$OUT/E4.csv" gap_ms "$g" "$r" --mode=$mode --gap-ms=$g
        done
      done
    done
fi

# ---- E5: semaphores vs monitor ----------------------------------------
# default = default parameters; heavy = heaviest load of E2 (Q=1) with
# gap 0; stress = heavy with treatment time 0 (only synchronization cost)
if want E5; then
    echo "E5: sem_fifo / sem / monitor at default, heavy, stress"
    rm -f "$OUT/E5.csv"
    for mode in sem_fifo sem monitor; do
      for r in $(seq 1 "$REPS"); do
        run E5 "$OUT/E5.csv" load default "$r" --mode=$mode
        run E5 "$OUT/E5.csv" load heavy   "$r" --mode=$mode --beds=1 --gap-ms=0
        run E5 "$OUT/E5.csv" load stress  "$r" --mode=$mode --beds=1 --gap-ms=0 \
            --treat-ms=0,0,0 --patients=3000
      done
    done
    # lines of code of the synchronization part (non-blank lines)
    count() { awk "/==== BEGIN $1 ====/{f=1;next} /==== END $1 ====/{f=0} f && NF" src/waiting_area.c | wc -l; }
    {
        echo "semaphore solution (sem_admit + sem_next):    $(count SEM) lines"
        echo "monitor procedures (mon_admit + mon_next):    $(count MONITOR) lines"
        echo "monitor library (src/monitor.c, non-blank):   $(awk 'NF' src/monitor.c | wc -l) lines"
        echo "shared queue helpers used by both are not counted"
    } > "$OUT/E5_loc.txt"
    cat "$OUT/E5_loc.txt"
fi

# ---- Bonus: E4 again with aging ---------------------------------------
if want BONUS; then
    echo "BONUS: E4 with --aging-ms=20 (sem, monitor)"
    rm -f "$OUT/E4_aging.csv"
    for mode in sem monitor; do
      for g in 0 1 3; do
        for r in $(seq 1 "$REPS"); do
          run E4_aging "$OUT/E4_aging.csv" gap_ms "$g" "$r" --mode=$mode \
              --gap-ms=$g --aging-ms=20
        done
      done
    done
fi

rm -f "$LOG"
echo "done: $(ls "$OUT"/*.csv | tr '\n' ' ')"
echo "tables: python3 scripts/summarize.py > results/summary.md"
