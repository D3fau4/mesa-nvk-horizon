# mesa-nvk-horizon — Phase 1 build: libhorizon_gpu.a + the test suites.
#
# ONE .nro PER SUITE, several cases per suite. A suite is tests/<suite>/:
# suite.c names its cases and <case>.c is one of them. SUITES and the
# CASES_<suite> lists below are this path's half of that; meson.build
# states the same thing for the other path and
# scripts/check-mesa-test-parity.sh fails if they disagree.
#
# Requires devkitA64 + libnx, resolved through $(DEVKITPRO) only (no
# machine-specific absolute paths; Phase 2 gate).
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
# -lhorizon_compat supplies functions newlib declares and does not
# define (compat/). It is produced by scripts/build-compat.sh, not by
# this Makefile: the Meson cross file names it too, and it has to exist
# before `meson setup` runs, so one provisioning step feeds both build
# paths. Keeping it in LIBS here is what stops the two paths diverging.
COMPAT_LIBDIR := build/toolchain/lib
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
           -Wl,--gc-sections -L$(COMPAT_LIBDIR) -L$(DEVKITPRO)/libnx/lib
LIBS    := -lhorizon_compat -lnx

BUILD   := build

LIB_SRCS := \
    horizon/cache/crc32.c \
    horizon/cache/blob_cache.c \
    horizon/debug/log.c \
    horizon/debug/status.c \
    horizon/device/device.c \
    horizon/memory/mem.c \
    horizon/vm/va_space.c \
    horizon/vm/vm.c \
    horizon/channel/channel.c \
    horizon/submit/cmds.c \
    horizon/submit/submit.c \
    horizon/surface/surface.c \
    horizon/sync/syncpt.c

LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libhorizon_gpu.a

COMPAT_SRCS := $(wildcard compat/*.c)
COMPAT_OBJS := $(COMPAT_SRCS:compat/%.c=$(COMPAT_LIBDIR)/%.o)
COMPAT_LIB  := $(COMPAT_LIBDIR)/libhorizon_compat.a


# The suites that need nothing but the toolchain, and the cases in each.
# The order inside a suite is the order they run in: a case reads its
# result back through machinery the case before it established.
SUITES := platform gpu_memory gpu_submit gpu_fault display dock

CASES_platform     := nv_bringup teardown sysconf crc32
CASES_gpu_memory   := alloc nvmap va_reserve va_map uncached \
                      shader_window borrowed_pages
CASES_gpu_submit   := channel submit syncpt_per_submit syncpt_cpu_incr \
                      fence_wait fence_wait_many gpu_write pushbuf_size
# Both fault the GPU on purpose and lose a channel doing it, so they are
# one .nro that is run last. tests/gpu_fault/suite.c says it in full.
CASES_gpu_fault    := sparse mmu_fault
# Owns the display: no console.
CASES_display      := console_handoff nwindow
# Needs a console *and* an operator, which is why it is neither in
# `display` nor in `platform`.
CASES_dock         := mode_change

# The objects one suite links: its table, then its cases. Used for the
# link line, for the target-specific flags below and for the depfiles.
suite_objs = $(BUILD)/$(1)/suite.t.o \
             $(foreach c,$(CASES_$(1)),$(BUILD)/$(1)/$(c).t.o)

# mesa_runtime measures Mesa's own code on hardware (Phase 3 items 4 and
# 5, plus the shader disk cache): the C11 threads shim Mesa selects
# here, os_time.c, and disk_cache. They link the archives Mesa's build
# produced rather than recompiling those sources with flags of our own —
# the object under test has to be the object Mesa builds, or the
# measurement is about a different build.
#
# -DHAVE_PTHREAD, -DHAVE_STRUCT_TIMESPEC and -DENABLE_SHADER_CACHE are
# not choices: they are what Mesa's own configure decided here, copied so
# the headers declare the same things the archives were built with. All
# three are visible in build/mesa-probe/build.ninja. The last one in
# particular is load-bearing rather than cosmetic — without it
# disk_cache.h offers static-inline stubs instead of prototypes, so
# mesa_runtime/disk_cache would compile against a cache that does
# nothing and then fail to link against one that does.
#
# meson.build states these same four things for the other build path,
# and scripts/check-mesa-test-parity.sh fails if the two ever disagree.
# They are duplicated on purpose: this file is the path whose output was
# verified on hardware and it has to be readable without a script.
#
# Conditional because they need `scripts/configure-mesa.sh &&
# scripts/build-mesa.sh` first, and a bare clone must still build the
# six suites that need nothing but the toolchain.
# $(MESA_BUILD_DIR) is what scripts/{configure,build}-mesa.sh honour and
# what scripts/toolchain-env.sh defaults; looking anywhere else would
# report "Mesa is not built" about a directory the caller never used.
# scripts/build-switch.sh forwards it into the container.
MESA_BUILD  := $(or $(MESA_BUILD_DIR),build/mesa-probe)
MESA_LIBS   := $(MESA_BUILD)/src/c11/impl/libmesa_util_c11.a \
               $(MESA_BUILD)/src/util/libmesa_util.a \
               $(MESA_BUILD)/src/util/blake3/libblake3.a
MESA_CFLAGS := -Imesa/src -Imesa/include -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC -DENABLE_SHADER_CACHE
MESA_SUITES := mesa_runtime

# disk_cache first: it times fsync() and file creation on the SD card,
# and those are the only numbers in the suite a busier process changes.
CASES_mesa_runtime := disk_cache c11_threads os_time

ifeq ($(words $(wildcard $(MESA_LIBS))),$(words $(MESA_LIBS)))
SUITES += $(MESA_SUITES)
STALE_MESA :=
else
$(info Makefile: skipping $(MESA_SUITES) — no Mesa archives in $(MESA_BUILD);)
$(info Makefile: run scripts/configure-mesa.sh && scripts/build-mesa.sh first.)
# Anything an earlier build left behind when Mesa *was* present. It has
# to go: scripts/package-horizon.sh copies every $(BUILD)/*.nro it finds
# and records its sha256 in a manifest whose whole job is to attribute an
# artefact to one build. Leaving these would ship the previous build's
# binaries under this build's manifest, right after this build said it
# was skipping them.
STALE_MESA := $(wildcard $(MESA_SUITES:%=$(BUILD)/%.nro) \
                         $(MESA_SUITES:%=$(BUILD)/%.elf) \
                         $(MESA_SUITES:%=$(BUILD)/%.nacp) \
                         $(foreach s,$(MESA_SUITES),$(call suite_objs,$(s))))
endif

TEST_NROS   := $(SUITES:%=$(BUILD)/%.nro)
SUITE_OBJS  := $(foreach s,$(SUITES),$(call suite_objs,$(s)))

# KEPT, NOT DELETED AS INTERMEDIATE. A case object is reached only
# through a chain of pattern rules — tests/<suite>/<case>.c to
# $(BUILD)/<suite>/<case>.t.o to $(BUILD)/<suite>.elf — which is exactly
# make's definition of an intermediate file, so it removes each one after
# the link. That was tolerable while a .nro was one object; with a suite
# holding up to eight of them it means every `make test` recompiles all
# fifty-three cases, and the depfiles below then describe objects that
# are not there. .SECONDARY names them so they survive, and the
# incremental build works again.
.SECONDARY: $(SUITE_OBJS)

# Object directories as order-only prerequisites, not `mkdir -p` inside
# each recipe: under `make -j`, two recipes racing to create the same new
# directory can lose an object file silently (observed with -j4 on this
# toolchain image's overlay filesystem — the compile for that object
# never ran and `ar` failed with "No such file or directory"). One
# directory per suite now, since a suite's objects live under
# $(BUILD)/<suite>/.
OBJ_DIRS := $(sort $(dir $(LIB_OBJS)) $(BUILD)/ $(COMPAT_LIBDIR)/ \
                   $(SUITES:%=$(BUILD)/%/))

# `lib` is the default goal (the first real target below) because a change
# under horizon/ only needs $(LIB) to verify compile-clean; the test
# .nros it does not touch are wasted rebuild time on every iteration.
# `test` is the one that also needs them, and `all` is kept as a synonym
# so `make all` still means what it always has.
.PHONY: lib test all clean prune-stale install uninstall
lib: $(LIB)
test: prune-stale lib $(TEST_NROS)
all: test

# INSTALL AND UNINSTALL ARE ONE SCRIPT, CALLED TWICE. The set of files
# an install places and the set an uninstall removes have to be one list
# or they are two lists that drift, and the drift is silent in the
# direction that matters: files left behind inside $(DEVKITPRO), where
# every later build on the machine goes on finding them.
#
# The script installs by extracting the tarball
# scripts/package-portlibs.sh builds, which is also what CI publishes —
# so what a release contains and what an install places are the same
# file set by construction. It builds that tarball first unless one is
# named; on a second run nothing is rebuilt.
#
# PREFIX defaults to devkitPro's portlibs prefix because that is the one
# directory $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
# looks in: it clears PKG_CONFIG_PATH and points PKG_CONFIG_LIBDIR
# there and nowhere else. ?= rather than =, so both
# `make install PREFIX=...` and `PREFIX=... make install` work.
#
# NEITHER DEPENDS ON `all`, and that is deliberate three times over.
# `all` builds tests an install does not ship. `all` cannot build any of
# the seventeen NVK archives, which are the thing being installed —
# scripts/build-mesa-nvk.sh does. And `all` produces
# $(BUILD)/libhorizon_gpu.a, which is the WRONG library here: the NVK
# archives were built against the Meson one, and the script refuses
# rather than substitute it. What turns "not built" into something
# useful is the script's gates, not a prerequisite.
PREFIX  ?= $(DEVKITPRO)/portlibs/switch
DESTDIR ?=

# AS ARGUMENTS, NOT AS ENVIRONMENT. devkitPro's make on Windows is a
# Cygwin build and an environment assignment in front of a recipe
# command reaches no child of it — measured, `FOO=hello sh -c 'echo
# $$FOO'` in a recipe prints nothing. Written that way, `make install
# PREFIX=...` installed into the default prefix while echoing the one it
# had been asked for, which is the worst shape a bug can have here.
install:
	scripts/install-horizon.sh --prefix '$(PREFIX)' --destdir '$(DESTDIR)'

uninstall:
	scripts/install-horizon.sh --uninstall \
	    --prefix '$(PREFIX)' --destdir '$(DESTDIR)'

# Safe under -j: $(STALE_MESA) is non-empty only for tests this build is
# not producing, so nothing else has these files as a target.
prune-stale:
ifneq ($(STALE_MESA),)
	@echo "removing stale Mesa test artefacts: $(STALE_MESA)"
	rm -f $(STALE_MESA)
endif

$(OBJ_DIRS):
	mkdir -p $@

$(BUILD)/%.o: %.c | $(OBJ_DIRS)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJS) | $(BUILD)/
	$(AR) rcs $@ $^

# compat/ — functions newlib declares and does not define. Built here
# with this Makefile's own CC and CFLAGS so `make clean && make` works
# on its own: clean removes $(BUILD), and $(COMPAT_LIBDIR) lives inside
# it. scripts/build-compat.sh produces the identical archive at the same
# path for the Meson and Mesa builds, which need it to exist *before*
# `meson setup` runs and therefore cannot get it from a make rule. The
# two recipes must keep emitting the same flags; the script's header
# says so too.
$(COMPAT_LIB): $(COMPAT_OBJS) | $(COMPAT_LIBDIR)/
	$(AR) rcs $@ $^

$(COMPAT_LIBDIR)/%.o: compat/%.c | $(COMPAT_LIBDIR)/
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# The build stamp every test prints in its first lines. Regenerated on
# every build — FORCE, not a timestamp comparison — because that is the
# whole point: a .nro on an SD card looks exactly like the one it
# replaced. See scripts/gen-build-id.sh.
.PHONY: FORCE
FORCE:

$(BUILD)/horizon_build_id.h: FORCE | $(BUILD)/
	scripts/gen-build-id.sh $@

$(BUILD)/testfw.o: tests/common/testfw.c $(BUILD)/horizon_build_id.h | $(BUILD)/
	$(CC) $(CFLAGS) -I$(BUILD) -MMD -MP -c $< -o $@

# $(OBJ_DIRS) AND NOT JUST $(BUILD)/, because a case object is written
# to $(BUILD)/<suite>/ and nothing in this rule's own graph creates that
# directory. It used to work only because $(LIB_OBJS) names every
# directory as an order-only prerequisite and `test` asks for `lib`
# first — an ordering `make -j` does not honour, since the goal's
# prerequisites are made in parallel, and one `make $(BUILD)/<suite>.elf`
# on a clean tree does not have at all: `make -n BUILD=zzbuild
# zzbuild/platform/suite.t.o` printed `mkdir -p zzbuild/` and then a
# compile into zzbuild/platform/, which is the "No such file or
# directory" the comment above OBJ_DIRS describes.
$(BUILD)/%.t.o: tests/%.c | $(OBJ_DIRS)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -MMD -MP -c $< -o $@

# $(COMPAT_LIB) is a prerequisite but not in $^: it is reached through
# -lhorizon_compat in $(LIBS), which is where it has to be so the linker
# resolves it after the objects that reference it. $(EXTRA_LIBS) is the
# same idea for the Mesa archives — before -lhorizon_compat and -lnx,
# because it is Mesa's objects that reference sysconf and libnx.
#
# This is a plain left-to-right link, so that order is the whole
# mechanism. The Meson path arrives at the same result differently and
# says how, in meson.build beside idep_mesa_core; the two are not
# expected to emit the same link line.
#
# .SECONDEXPANSION is what lets one pattern rule name a *set* of
# prerequisites that depends on the stem: $$* is the suite name on the
# second pass, so $$(call suite_objs,$$*) is that suite's table plus its
# cases. Without it the rule would have to be written out once per suite.
# The recipe needs no such trick — a recipe is expanded when it runs,
# with $* already known.
.SECONDEXPANSION:
$(BUILD)/%.elf: $$(call suite_objs,$$*) $(BUILD)/testfw.o $(LIB) $(COMPAT_LIB)
	$(CC) $(LDFLAGS) $(call suite_objs,$*) $(BUILD)/testfw.o $(LIB) \
	    $(EXTRA_LIBS) $(LIBS) -o $@

# Target-specific, so only mesa_runtime sees the Mesa include path and
# archives; the other six suites keep building with no Mesa in sight.
$(foreach s,$(MESA_SUITES),$(call suite_objs,$(s))): EXTRA_CFLAGS := $(MESA_CFLAGS)
# $(LIB) appears a second time AFTER the Mesa archives, and -lzstd -lz
# after those, because this is a plain left-to-right link and the shader
# cache made libmesa_util.a depend on both directions:
#
#   libmesa_util.a(disk_cache_horizon.c.o) -> horizon_gpu_status_str
#                                             ... in libhorizon_gpu.a
#   libmesa_util.a(compress.c.o)           -> ZSTD_compress, deflate
#                                             ... in devkitPro's portlibs
#
# The first is the whole point of the backend and the second is Mesa's
# own configure result (-DHAVE_ZSTD -DHAVE_ZLIB, from portlibs' pkg-config).
# An archive already passed is not searched again, so libhorizon_gpu.a
# has to be named on both sides of libmesa_util.a.
$(MESA_SUITES:%=$(BUILD)/%.elf): \
    EXTRA_LIBS := $(MESA_LIBS) $(LIB) \
                  -L$(DEVKITPRO)/portlibs/switch/lib -lzstd -lz
$(MESA_SUITES:%=$(BUILD)/%.elf): $(MESA_LIBS)

$(BUILD)/%.nacp: | $(BUILD)/
	$(NACPTOOL) --create "$*" "mesa-nvk-horizon" "phase1" $@

$(BUILD)/%.nro: $(BUILD)/%.elf $(BUILD)/%.nacp
	$(ELF2NRO) $< $@ --nacp=$(BUILD)/$*.nacp

# The rule is NOT "only what this Makefile produces" — build/meson is
# Meson's and it goes. It is: anything cheap to regenerate from this tree
# is removed; anything that costs minutes of compilation or a network
# fetch is kept, because `make clean` is a request to start the build
# over, not to pay for the toolchain again.
#
# Removed although it is not ours: the Meson build directory and its
# stamp. It holds this project's own sources, reconfigures in seconds
# through scripts/configure-horizon.sh, and a stale cross build
# directory is a hazard rather than an asset.
#
# Kept:
#
#   $(MESA_BUILD)  — scripts/{configure,build}-mesa.sh produce it, it
#     costs minutes to rebuild, and its presence is what selects tests 12
#     and 13. Deleting it made `make clean && make` drop those two tests
#     without the caller having asked for anything of the sort.
#     $(MESA_BUILD).crossid goes with it: scripts/toolchain-env.sh keeps
#     the stamp beside the directory (--wipe empties the directory
#     itself) and treats a configured directory with no stamp as stale,
#     so removing the stamp alone forces a full Mesa reconfigure.
#
#   $(BUILD)/toolchain — the pinned Meson and Mesa's Python generator
#     deps are installed there from the network, which CLAUDE.md
#     documents as the thing that may not be reachable. `make clean`
#     uninstalling the build system is not what anyone asks for by
#     typing it. The parts of it this Makefile does produce are removed
#     explicitly below; the generated cross file is regenerated by
#     scripts/gen-cross-file.sh on the next configure.
#
# $(filter) is a literal string comparison, so the kept set has to be
# spelled the same way $(wildcard) spells it. It was not: measured with
# `make -n clean`, MESA_BUILD_DIR=build/mesa-probe/ — and ./build/mesa-probe,
# and build//mesa-probe — all failed to match and put the Mesa build in
# the rm -rf, which is the loss this rule exists to prevent.
# $(abspath) normalises both sides without requiring the path to exist:
# it collapses repeated slashes, resolves . and .., and strips a trailing
# slash. scripts/toolchain-env.sh normalises for the script path too;
# this Makefile does its own because it is also a standalone entry point.
#
# An exact match is not enough either. $(wildcard $(BUILD)/*) lists the
# immediate children of build/, so MESA_BUILD_DIR=build/cache/mesa-probe
# puts build/cache in that list while the kept set holds only
# build/cache/mesa-probe — and `rm -rf build/cache` takes the Mesa build
# with it. Measured with make -n clean. An entry is therefore kept when
# it *is* one of the paths below or when it *contains* the Mesa build.
# Keeping a whole intermediate directory is the conservative direction:
# a nested Mesa build makes clean spare that directory entirely, which
# is stated here rather than discovered.
MESA_BUILD_ABS := $(abspath $(MESA_BUILD))
CLEAN_KEEP := $(MESA_BUILD_ABS) $(MESA_BUILD_ABS).crossid \
              $(abspath $(BUILD)/toolchain)

# Non-empty when $1 must survive clean. $(strip) is load-bearing: the
# two filters are joined by the line continuation's space, so with both
# empty this expands to " ", and $(if) reads a lone space as true —
# which kept every file and made clean delete nothing at all. Measured
# with make -n clean, which printed a bare `rm -rf`.
clean_keeps = $(strip $(filter $(abspath $1),$(CLEAN_KEEP)) \
                      $(filter $(abspath $1)/%,$(MESA_BUILD_ABS)))

clean:
	rm -rf $(foreach e,$(wildcard $(BUILD)/*),\
	          $(if $(call clean_keeps,$(e)),,$(e)))
	rm -rf $(COMPAT_LIBDIR) $(BUILD)/toolchain/compat-obj

# The compat depfiles were generated but never included, so a change to a
# newlib or libnx header did not rebuild compat/ on this path either.
-include $(LIB_OBJS:.o=.d) $(SUITE_OBJS:.t.o=.t.d) $(BUILD)/testfw.d \
         $(COMPAT_OBJS:.o=.d)
