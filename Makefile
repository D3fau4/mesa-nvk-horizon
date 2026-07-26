# mesa-nvk-horizon — Phase 1 build: libhorizon_gpu.a + standalone test .nros.
#
# Requires devkitA64 + libnx, resolved through $(DEVKITPRO) only (no
# machine-specific absolute paths; docs/milestones.md Phase 2 gate).
# Host-side unit tests are separate: scripts/run-host-tests.sh.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set; install devkitA64 or use scripts/build-switch.sh)
endif

CC      := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
AR      := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc-ar
ELF2NRO := $(DEVKITPRO)/tools/bin/elf2nro
NACPTOOL:= $(DEVKITPRO)/tools/bin/nacptool

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -O2 -Wall -Wextra -Werror -std=gnu11 -ffunction-sections \
           -D__SWITCH__ $(ARCH) \
           -Ihorizon/include -Itests \
           -I$(DEVKITPRO)/libnx/include
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
           -L$(DEVKITPRO)/libnx/lib
LIBS    := -lnx

BUILD   := build

LIB_SRCS := \
    horizon/debug/log.c \
    horizon/debug/status.c \
    horizon/device/device.c \
    horizon/memory/mem.c \
    horizon/vm/va_space.c \
    horizon/vm/vm.c \
    horizon/channel/channel.c \
    horizon/submit/cmds.c \
    horizon/submit/submit.c \
    horizon/sync/syncpt.c

LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libhorizon_gpu.a

# Object directories as order-only prerequisites, not `mkdir -p` inside each
# recipe: under `make -j`, two recipes racing to create the same new
# directory can lose an object file silently (observed with -j4 on this
# toolchain image's overlay filesystem — the compile for that object never
# ran and `ar` failed with "No such file or directory").
OBJ_DIRS := $(sort $(dir $(LIB_OBJS)) $(BUILD)/)

TESTS := t_init t_alloc t_nvmap t_va_reserve t_map t_channel t_submit \
         t_syncpt t_fence_wait t_teardown

TEST_NROS := $(TESTS:%=$(BUILD)/%.nro)

.PHONY: all lib clean
all: lib $(TEST_NROS)
lib: $(LIB)

$(OBJ_DIRS):
	mkdir -p $@

$(BUILD)/%.o: %.c | $(OBJ_DIRS)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJS) | $(BUILD)/
	$(AR) rcs $@ $^

$(BUILD)/testfw.o: tests/common/testfw.c | $(BUILD)/
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.t.o: tests/%.c | $(BUILD)/
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.elf: $(BUILD)/%.t.o $(BUILD)/testfw.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/%.nacp: | $(BUILD)/
	$(NACPTOOL) --create "$*" "mesa-nvk-horizon" "phase1" $@

$(BUILD)/%.nro: $(BUILD)/%.elf $(BUILD)/%.nacp
	$(ELF2NRO) $< $@ --nacp=$(BUILD)/$*.nacp

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(TESTS:%=$(BUILD)/%.t.d) $(BUILD)/testfw.d
