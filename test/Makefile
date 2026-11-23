# Native build of the mt-actions test host.
CJSON_DIR ?= $(HOME)/.platformio/packages/framework-espidf/components/json/cJSON
CC ?= cc
CFLAGS ?= -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter

build/mta_host: mta_host.c ../src/*.c ../include/*.h
	mkdir -p build
	$(CC) $(CFLAGS) -I../include -I$(CJSON_DIR) ../src/*.c mta_host.c $(CJSON_DIR)/cJSON.c -o $@

clean:
	rm -rf build

# Firmware-only paths (RTOS event lock/queue, driver instances, permissions) via a pthread shim.
build/fwtest: firmware_paths_test.c rtos_shim.h ../src/*.c ../include/*.h
	mkdir -p build
	$(CC) $(CFLAGS) -DMTA_RTOS_SHIM='"rtos_shim.h"' -I. -I../include -I$(CJSON_DIR) ../src/*.c firmware_paths_test.c $(CJSON_DIR)/cJSON.c -lpthread -o $@

fwtest: build/fwtest

# mta_host with the RTOS event lock/queue enabled (pthread shim), for firmware-faithful replays.
build/mta_host_rtos: mta_host.c rtos_shim.h ../src/*.c ../include/*.h
	mkdir -p build
	$(CC) $(CFLAGS) -DMTA_RTOS_SHIM='"rtos_shim.h"' -I. -I../include -I$(CJSON_DIR) ../src/*.c mta_host.c $(CJSON_DIR)/cJSON.c -lpthread -o $@
