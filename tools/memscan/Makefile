# memscan — build the SD-card log reader (tools/memscan/memscan.c).
#
# Standalone on purpose: it links libnx and nothing this project builds,
# so it neither needs nor disturbs the main build. Same toolchain
# resolution as the repository Makefile — $(DEVKITPRO) only, no
# machine-specific absolute paths.
#
#   make -C tools/memscan            # with devkitA64 installed
#   scripts/build-memscan.sh         # or through the container
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set; use scripts/build-memscan.sh)
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
all: $(BUILD)/memscan.nro

$(BUILD)/memscan.o: memscan.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/memscan.elf: $(BUILD)/memscan.o
	$(CC) $(LDFLAGS) $< $(LIBS) -o $@

$(BUILD)/memscan.nacp:
	$(NACPTOOL) --create "memscan" "mesa-nvk-horizon" "tool" $@

$(BUILD)/memscan.nro: $(BUILD)/memscan.elf $(BUILD)/memscan.nacp
	$(ELF2NRO) $< $@ --nacp=$(BUILD)/memscan.nacp

clean:
	rm -f $(BUILD)/memscan.o $(BUILD)/memscan.elf $(BUILD)/memscan.nacp \
	      $(BUILD)/memscan.nro
