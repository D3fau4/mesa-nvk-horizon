# logcat — build the SD-card log reader (tools/logcat/logcat.c).
#
# Standalone on purpose: it links libnx and nothing this project builds,
# so it neither needs nor disturbs the main build. Same toolchain
# resolution as the repository Makefile — $(DEVKITPRO) only, no
# machine-specific absolute paths.
#
#   make -C tools/logcat            # with devkitA64 installed
#   scripts/build-logcat.sh         # or through the container
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set; use scripts/build-logcat.sh)
endif

CC       := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
ELF2NRO  := $(DEVKITPRO)/tools/bin/elf2nro
NACPTOOL := $(DEVKITPRO)/tools/bin/nacptool

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -O2 -Wall -Wextra -Werror -std=gnu11 -ffunction-sections \
           -D__SWITCH__ $(ARCH) -I$(DEVKITPRO)/libnx/include
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
           -Wl,--gc-sections -L$(DEVKITPRO)/libnx/lib
LIBS    := -lnx

# Beside the test .nro, so one directory holds everything that gets sent
# to a console.
BUILD := ../../build

.PHONY: all clean
all: $(BUILD)/logcat.nro

$(BUILD)/logcat.o: logcat.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/logcat.elf: $(BUILD)/logcat.o
	$(CC) $(LDFLAGS) $< $(LIBS) -o $@

$(BUILD)/logcat.nacp:
	$(NACPTOOL) --create "logcat" "mesa-nvk-horizon" "tool" $@

$(BUILD)/logcat.nro: $(BUILD)/logcat.elf $(BUILD)/logcat.nacp
	$(ELF2NRO) $< $@ --nacp=$(BUILD)/logcat.nacp

clean:
	rm -f $(BUILD)/logcat.o $(BUILD)/logcat.elf $(BUILD)/logcat.nacp \
	      $(BUILD)/logcat.nro
