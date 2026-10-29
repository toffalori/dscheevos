# Host tests for the launcher's plain-C parts: make -C dscheevos/launcher/tests test
CC ?= cc
CFLAGS += -std=c99 -Wall -Wextra -Werror -I../source -I../../module/include

RC := ../../../external/rcheevos
RC_SRC := $(wildcard $(RC)/src/api/rc_api_*.c) $(wildcard $(RC)/src/util/*.c) $(wildcard $(RC)/src/runtime/*.c)

test: test_clock test_request test_recording
	./test_clock
	./test_request
	./test_recording

test_clock: test_clock.c ../source/clock.c ../source/clock.h ../../module/include/dscheevos_module.h
	$(CC) $(CFLAGS) -o $@ test_clock.c ../source/clock.c

# the replay from the SD copy of a recording, with the module's recorder writing it
test_recording: test_recording.c ../source/recording.c ../source/recording.h ../../module/src/record.c \
                ../../module/include/dscm_record.h
	$(CC) $(CFLAGS) -std=gnu99 -Wno-pointer-to-int-cast -DDSCM_STREAM -o $@ test_recording.c ../source/recording.c ../../module/src/record.c

# the request glue against rcheevos' own request builder (unmodified)
test_request: test_request.c ../source/request.c ../source/request.h
	$(CC) -std=c99 -I../source -I$(RC)/include -I$(RC)/src -o $@ test_request.c ../source/request.c $(RC_SRC)

clean:
	rm -f test_clock test_request test_recording

.PHONY: test clean
