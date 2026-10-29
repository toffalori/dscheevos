# Host tests for the module's network code and heap.
#   make test        build and run them
MBEDTLS  ?= ../../../external/mbedtls
TLSCONF  := ../../launcher/tls
CC       ?= cc
CFLAGS   ?= -O1 -g -Wall -Wextra
INCLUDES := -I../src/net -I$(TLSCONF) -I$(MBEDTLS)/include '-DMBEDTLS_CONFIG_FILE="dsc_mbedtls_config.h"'

BUILD   := build
TLS_SRC := $(wildcard $(MBEDTLS)/library/*.c)
TLS_OBJ := $(patsubst $(MBEDTLS)/library/%.c,$(BUILD)/tls/%.o,$(TLS_SRC))

.PHONY: test clean
test: $(BUILD)/test_wpa $(BUILD)/test_heap $(BUILD)/test_popup
	$(BUILD)/test_wpa
	$(BUILD)/test_heap
	$(BUILD)/test_popup $(FONTS) $(BUILD)/popup

$(BUILD)/tls/%.o: $(MBEDTLS)/library/%.c $(TLSCONF)/dsc_mbedtls_config.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -w $(INCLUDES) -c $< -o $@

$(BUILD)/libtls.a: $(TLS_OBJ)
	ar rcs $@ $^

$(BUILD)/test_wpa: test_wpa.c ../src/net/wpa.c ../src/net/wpa.h $(BUILD)/libtls.a
	$(CC) $(CFLAGS) $(INCLUDES) test_wpa.c ../src/net/wpa.c $(BUILD)/libtls.a -o $@

# test_heap: heap.c with the core and real rcheevos. The host linker may have
# no --wrap, so the wraps are macros: every malloc in the core and rcheevos
# is heap.c's, and heap.c's __real_* are the test's counted malloc.
RCHEEVOS ?= ../../../external/rcheevos
CORE     := ../../core
RC_SRC   := $(addprefix $(RCHEEVOS)/src/, client/rc_client.c \
  runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c runtime/rc_consoleinfo.c \
  runtime/rc_format.c runtime/rc_lboard.c runtime/rc_memref.c runtime/rc_operand.c \
  runtime/rc_richpresence.c runtime/rc_runtime.c runtime/rc_runtime_progress.c \
  runtime/rc_trigger.c runtime/rc_value.c \
  api/rc_api_common.c api/rc_api_info.c api/rc_api_runtime.c api/rc_api_user.c \
  util/md5.c util/rc_compat.c util/rc_util.c util/rc_version.c)
RC_OBJ   := $(patsubst $(RCHEEVOS)/src/%.c,$(BUILD)/heap/rc/%.o,$(RC_SRC))
HEAP_INC := -I../include -I$(CORE)/include -I$(RCHEEVOS)/include -I$(RCHEEVOS)/src -DRC_NO_THREADS
WRAP     := -Dmalloc=__wrap_malloc -Dfree=__wrap_free -Dcalloc=__wrap_calloc -Drealloc=__wrap_realloc
PARSE    := rc_api_process_fetch_game_sets_server_response

$(BUILD)/heap/rc/client/rc_client.o: $(RCHEEVOS)/src/client/rc_client.c
	@mkdir -p $(dir $@)
	$(CC) -O1 -w $(HEAP_INC) $(WRAP) -D$(PARSE)=__wrap_$(PARSE) -c $< -o $@
$(BUILD)/heap/rc/%.o: $(RCHEEVOS)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -O1 -w $(HEAP_INC) $(WRAP) -c $< -o $@
$(BUILD)/heap/dscheevos.o: $(CORE)/src/dscheevos.c $(CORE)/include/dscheevos.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -w $(HEAP_INC) $(WRAP) -c $< -o $@
$(BUILD)/heap/heap.o: ../src/heap.c ../src/heap.h ../include/dscheevos_module.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(HEAP_INC) -DDSCM_HEAP_HOST_TEST -D__real_malloc=h_malloc -D__real_free=h_free \
	  -D__real_calloc=h_calloc -D__real_realloc=h_realloc -D__real_$(PARSE)=$(PARSE) -c $< -o $@
$(BUILD)/test_heap: test_heap.c $(BUILD)/heap/heap.o $(BUILD)/heap/dscheevos.o $(RC_OBJ)
	$(CC) $(CFLAGS) $(HEAP_INC) test_heap.c $(BUILD)/heap/heap.o $(BUILD)/heap/dscheevos.o $(RC_OBJ) -lm -o $@


# test_popup: the streamer's pop-ups (src/popup.c) with the registers in plain memory;
# writes the bands it draws as PPM images into build/popup.
FONTS    ?= ../../../external/nds-bootstrap/retail/nitrofiles/fonts
DISPLAY  := ../../display
$(BUILD)/test_popup: test_popup.c ../src/popup.c ../include/ds_stream_protocol.h $(DISPLAY)/src/dsd_render.c $(DISPLAY)/src/dsd_font_small.c
	@mkdir -p $(BUILD)/popup
	$(CC) $(CFLAGS) -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -DDSCM_STREAM -I../include -I$(DISPLAY)/include \
	  test_popup.c $(DISPLAY)/src/dsd_render.c $(DISPLAY)/src/dsd_badge.c $(DISPLAY)/src/dsd_sched.c $(DISPLAY)/src/dsd_font_small.c -o $@
clean:
	rm -rf $(BUILD)
