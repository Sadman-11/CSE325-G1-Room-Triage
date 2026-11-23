# CSE325 Group 1 - Emergency Room Triage
#   make         build bin/er_triage
#   make run     one default demo (mode sem)
#   make demo    short --verbose demo in sem and monitor mode
#   make test    every correct mode once: prints only CHECK ... PASS lines
#   make nosync  show that removing synchronization breaks the checks
#   make clean   remove binaries, logs and leftover IPC objects

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O0 -g -D_DEFAULT_SOURCE -Iinclude
# -pthread/-lrt: only link flags, needed by old glibc (< 2.34)
LDLIBS  := -pthread -lrt

SRC := src/main.c src/ambulance.c src/doctor.c src/logger.c \
       src/waiting_area.c src/monitor.c src/ipc.c src/stats.c
OBJ := $(SRC:src/%.c=build/%.o)
BIN := bin/er_triage
HDR := include/common.h include/monitor.h

.PHONY: all run demo test nosync clean

all: $(BIN)

$(BIN): $(OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c $(HDR) | build
	$(CC) $(CFLAGS) -c $< -o $@

bin build:
	mkdir -p $@

run: $(BIN)
	./$(BIN) --mode=sem

demo: $(BIN)
	./$(BIN) --mode=sem --items=24 --verbose
	./$(BIN) --mode=monitor --items=24 --verbose

# Runs sem_fifo, sem, monitor (and sem/monitor with aging) and prints only
# their CHECK lines. Exit status is non-zero if any check fails.
test: $(BIN)
	@fail=0; \
	for args in "--mode=sem_fifo" "--mode=sem" "--mode=monitor" \
	            "--mode=sem --aging-ms=20" "--mode=monitor --aging-ms=20"; do \
	  ./$(BIN) $$args --log=build/test.log > build/test.out 2>&1 || fail=1; \
	  grep '^CHECK' build/test.out; \
	done; \
	rm -f build/test.log build/test.out; \
	if [ $$fail -ne 0 ]; then echo "make test: some CHECK failed"; exit 1; fi

nosync: $(BIN)
	-./$(BIN) --mode=nosync --ambulances=4 --doctors=4

clean:
	rm -rf bin build er_log.txt
	rm -f /dev/shm/cse325_g1_er_* 2>/dev/null || true
