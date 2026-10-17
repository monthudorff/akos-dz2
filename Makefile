CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -pthread -D_POSIX_C_SOURCE=200809L
OPT     = -O2
TSAN    = -O1 -g -fsanitize=thread

COMMON  = src/main.c src/config.c src/log.c src/bqueue.c src/kitchen.c src/equip.c
HEADERS = $(wildcard src/*.h)
BINS    = kitchen_sem kitchen_cv

all: $(BINS)

kitchen_sem: $(COMMON) src/sync_sem.c $(HEADERS)
	$(CC) $(CFLAGS) $(OPT) -o $@ $(COMMON) src/sync_sem.c

kitchen_cv: $(COMMON) src/sync_cv.c $(HEADERS)
	$(CC) $(CFLAGS) $(OPT) -o $@ $(COMMON) src/sync_cv.c

kitchen_sem_tsan: $(COMMON) src/sync_sem.c $(HEADERS)
	$(CC) $(CFLAGS) $(TSAN) -o $@ $(COMMON) src/sync_sem.c

kitchen_cv_tsan: $(COMMON) src/sync_cv.c $(HEADERS)
	$(CC) $(CFLAGS) $(TSAN) -o $@ $(COMMON) src/sync_cv.c

test: $(BINS)
	tests/run_tests.sh ./kitchen_sem ./kitchen_cv

tsan: kitchen_sem_tsan kitchen_cv_tsan
	TSAN_OPTIONS="halt_on_error=1 exitcode=66" tests/run_tests.sh ./kitchen_sem_tsan ./kitchen_cv_tsan

clean:
	rm -f $(BINS) kitchen_sem_tsan kitchen_cv_tsan kitchen.log

.PHONY: all test tsan clean
