# mesa-nvk-horizon — Meson build for libhorizon_gpu.a and the ten
# Phase 1 test .nros.
#
# This exists to exercise toolchain/horizon-aarch64.cross
# (docs/milestones.md Phase 2: "Phase 1 tests cross-compile with the new
# cross file"), and to be the build system the Mesa work in Phase 3
# plugs into.
#
# It does NOT replace the Makefile. The Makefile produced the .nro files
# that were verified on real hardware (STATUS.md: 10/10 PASS), so it
# stays as the reference path; both are expected to emit the same flags
# and comparable artefacts. Host-side unit tests remain separate
# (scripts/run-host-tests.sh) — they are built with the *host* compiler
# and sanitizers, which is a different job from this file's.
#
# Configure through scripts/configure-horizon.sh; it resolves the
# toolchain and passes both cross files in the right order.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT

project(
  'mesa-nvk-horizon',
  'c',
  version : '0.1.0',
  license : 'MIT',
  meson_version : '>= 1.4.0',
  default_options : [
    # Meson must not add -O/-g of its own. The cross file states every
    # flag explicitly so this build matches the Makefile's, rather than
    # matching Meson's interpretation of an abstract level.
    'buildtype=plain',
    # Meson otherwise appends -fPIC to static-library objects, which
    # overrides the cross file's -fPIE for exactly those objects and not
    # for the executables' own. That produced a measurable divergence
    # from the hardware-verified Makefile output (t_alloc: 48 bytes of
    # .text, 32 of .bss). Turning it off makes both paths -fPIE
    # throughout. Nothing here is a shared library.
    'b_staticpic=false',
  ],
)

# ---------------------------------------------------------------------
# This build is cross-only, and only for Horizon.
# ---------------------------------------------------------------------
# Failing loudly here beats emitting x86 objects that look fine until
# elf2nro rejects them. horizon/ is libnx-backed: there is no host
# configuration of it, by design (CLAUDE.md layer rules).
if not meson.is_cross_build()
  error(
    'mesa-nvk-horizon builds only as a cross build for Horizon.\n' +
    'Use scripts/configure-horizon.sh, which passes\n' +
    '  --cross-file build/toolchain/devkitpro.cross\n' +
    '  --cross-file toolchain/horizon-aarch64.cross\n' +
    'For the pure-logic unit tests on this machine, use\n' +
    '  scripts/run-host-tests.sh'
  )
endif

if host_machine.system() != 'horizon'
  error('host_machine.system() is @0@, expected \'horizon\''
        .format(host_machine.system()))
endif

if host_machine.cpu_family() != 'aarch64'
  error('host_machine.cpu_family() is @0@, expected \'aarch64\''
        .format(host_machine.cpu_family()))
endif

# default_options above only takes effect on the *first* configure of a
# build directory. Reconfiguring an older one would silently restore
# Meson's -fPIC on static-library objects and reintroduce the divergence
# from the hardware-verified Makefile output. Fail loudly instead.
if get_option('b_staticpic')
  error(
    'b_staticpic is true: static-library objects would be built -fPIC,\n' +
    'overriding the cross file\'s -fPIE for those objects only and\n' +
    'diverging from the Makefile build that was verified on hardware.\n' +
    'This build directory predates the default_options change; either\n' +
    '  meson setup --wipe <builddir>   (or delete it and reconfigure)\n' +
    'or configure with -Db_staticpic=false.'
  )
endif

# ---------------------------------------------------------------------
# Warning policy.
# ---------------------------------------------------------------------
# The same three flags the Makefile builds with (its CFLAGS), so both
# paths keep emitting the same set. They live here rather than in
# toolchain/horizon-aarch64.cross because a cross file's c_args are also
# handed to Meson's own detection snippets, which are not written to
# survive -Wextra -Werror — with the flags there, five of Mesa's
# configure checks returned a *false* answer (see the cross file's
# [built-in options] note for the measurement). add_project_arguments is
# not passed to those checks, so the policy applies to this project's
# targets and to nothing else.
#
# language : 'c' only — project() above declares just C, and
# add_project_arguments rejects a language the project does not have.
# The cross file's cpp_args exist for the Mesa build, which sets its own
# warning policy.
add_project_arguments('-Wall', '-Wextra', '-Werror', language : 'c')

# ---------------------------------------------------------------------
# horizon_gpu — the Vulkan-free, WSI-free GPU abstraction.
# ---------------------------------------------------------------------
# Same translation units, in the same order, as the Makefile's LIB_SRCS.
# Keep the two lists in step: a file added here and not there builds
# clean under Meson and vanishes from the hardware-verified path.
horizon_gpu_sources = files(
  'horizon/debug/log.c',
  'horizon/debug/status.c',
  'horizon/device/device.c',
  'horizon/memory/mem.c',
  'horizon/vm/va_space.c',
  'horizon/vm/vm.c',
  'horizon/channel/channel.c',
  'horizon/submit/cmds.c',
  'horizon/submit/submit.c',
  'horizon/sync/syncpt.c',
)

# horizon/include/ is the only public surface of the layer
# (docs/architecture.md §4.4); the implementation directories are not on
# the include path.
horizon_gpu_inc = include_directories('horizon/include')

libhorizon_gpu = static_library(
  'horizon_gpu',
  horizon_gpu_sources,
  include_directories : horizon_gpu_inc,
  install : false,
)

idep_horizon_gpu = declare_dependency(
  link_with : libhorizon_gpu,
  include_directories : horizon_gpu_inc,
)

# ---------------------------------------------------------------------
# Phase 1 tests — one standalone .nro each.
# ---------------------------------------------------------------------
# Packaging tools come from the cross file's [binaries]; they run on the
# build machine even though they are declared for the host toolchain,
# which is what custom_target commands do.
elf2nro = find_program('elf2nro')
nacptool = find_program('nacptool')

# Test-framework header lives in tests/common/, included as "common/…".
tests_inc = include_directories('tests')

horizon_tests = [
  't_init',
  't_alloc',
  't_nvmap',
  't_va_reserve',
  't_map',
  't_channel',
  't_submit',
  't_syncpt',
  't_fence_wait',
  't_teardown',
  # Phase 3 item 6: compat/sysconf.c against the real kernel. Uses no
  # horizon_gpu — the C library and svcGetInfo only — but it is built
  # and packaged with the others because it is a hardware measurement,
  # and hardware measurements travel together.
  't_sysinfo',
]

testfw = static_library(
  'testfw',
  files('tests/common/testfw.c'),
  include_directories : [horizon_gpu_inc, tests_inc],
  install : false,
)

test_nros = []

# ---------------------------------------------------------------------
# Tests 12 and 13 — Mesa's own code, measured on hardware.
# ---------------------------------------------------------------------
# Phase 3 items 4 (threads) and 5 (timers/clocks). They link the archives
# Mesa's build produced, not sources recompiled with flags of our own:
# the object under test has to be the object Mesa builds, or the
# measurement describes a different build. -DHAVE_PTHREAD and
# -DHAVE_STRUCT_TIMESPEC are Mesa's own configure results here, copied so
# its headers declare the same types; both appear in
# build/mesa-probe/build.ninja.
#
# Skipped, with a message, when Mesa has not been built — a bare clone
# must still produce the eleven tests that need only the toolchain.
#
# The Makefile states the same four facts — MESA_TESTS, MESA_LIBS and
# MESA_CFLAGS' defines and include paths — and each build system is meant
# to stay readable on its own, so they are duplicated rather than shared.
# scripts/check-mesa-test-parity.sh is what enforces that: a comment
# asking for them to be kept in step is not enforcement, and the .nro
# size comparison would not notice a define present here and missing
# there.
fs = import('fs')

# Not hardcoded: scripts/configure-mesa.sh and scripts/build-mesa.sh both
# honour $MESA_BUILD_DIR, and looking somewhere else than the directory
# the caller actually built in would skip these two tests while claiming
# Mesa had not been built. scripts/configure-horizon.sh passes the same
# value through (see meson.options for why it is an option and not an
# environment variable).
mesa_build_dir = get_option('mesa_build_dir')
mesa_test_libs = [
  mesa_build_dir / 'src/c11/impl/libmesa_util_c11.a',
  mesa_build_dir / 'src/util/libmesa_util.a',
]
mesa_tests = ['t_threads', 't_ostime']

# meson.project_source_root() / p returns p unchanged when p is absolute,
# so both an absolute and a source-root-relative option value resolve
# here without a second code path.
mesa_test_lib_paths = []
foreach l : mesa_test_libs
  mesa_test_lib_paths += meson.project_source_root() / l
endforeach

mesa_libs_present = true
foreach p : mesa_test_lib_paths
  if not fs.exists(p)
    mesa_libs_present = false
  endif
endforeach

if mesa_libs_present
  # WHERE THESE LAND ON THE LINK LINE, and why it works here.
  #
  # The Makefile puts the archives *before* -lhorizon_compat -lnx and
  # says so in a comment, because it is Mesa's objects that reference
  # sysconf and libnx and a plain left-to-right link resolves in that
  # order only. This path does the opposite, and that is not a
  # divergence to fix — it is a different mechanism, measured in the
  # generated build/meson/build.ninja:
  #
  #   LINK_ARGS = ... -Wl,--start-group -lhorizon_compat -lnx \
  #       libtestfw.a libhorizon_gpu.a \
  #       .../libmesa_util_c11.a .../libmesa_util.a -Wl,--end-group
  #
  # A dependency's link_args go last, after the cross file's
  # c_link_args, so the archives sit behind the libraries they need —
  # and Meson wraps the whole set in --start-group/--end-group, which
  # makes the linker rescan until no undefined symbol is left. Order
  # inside a group does not decide the outcome, which is why this links
  # at all. Recorded rather than left to inference: the property was
  # documented on the path where it is guaranteed and nowhere on the
  # path where it is not obvious.
  idep_mesa_core = declare_dependency(
    compile_args : ['-DHAVE_PTHREAD', '-DHAVE_STRUCT_TIMESPEC'],
    include_directories : include_directories('mesa/src', 'mesa/include'),
    link_args : mesa_test_lib_paths,
  )
  horizon_tests += mesa_tests
else
  message('skipping ' + ' '.join(mesa_tests) + ': no Mesa archives in ' +
          mesa_build_dir + '; run scripts/configure-mesa.sh and ' +
          'scripts/build-mesa.sh first')
endif

foreach t : horizon_tests
  # Tests 12 and 13 additionally see Mesa's headers and archives; the
  # other eleven are built with no Mesa in sight, as before.
  t_deps = [idep_horizon_gpu]
  # Rebuilding Mesa must relink these two, or they would keep measuring
  # the archives of an older Mesa build. Meson already infers that:
  # guess_external_link_dependencies() (ninjabackend.py:3636 in meson
  # 1.11.2, the version toolchain/versions.env pins) records any link
  # argument that is an absolute path to an existing library file, and
  # the generated rule reads
  #   build t_threads.elf: c_LINKER ... | .../libmesa_util_c11.a ...
  # Declaring it as well costs one line and does not depend on that
  # inference staying a documented guess — nor on the archives existing
  # when this directory was configured, which is what the guess requires
  # and scripts/build-horizon.sh now detects separately.
  t_link_depends = []
  if mesa_libs_present and mesa_tests.contains(t)
    t_deps += idep_mesa_core
    t_link_depends = mesa_test_lib_paths
  endif

  # name_suffix 'elf': elf2nro takes an ELF, and naming it so keeps the
  # intermediate distinguishable from the .nro in the build directory.
  t_elf = executable(
    t,
    files('tests' / (t + '.c')),
    include_directories : [horizon_gpu_inc, tests_inc],
    link_with : testfw,
    dependencies : t_deps,
    link_depends : t_link_depends,
    name_suffix : 'elf',
    install : false,
  )

  # Metadata matches the Makefile's nacptool invocation exactly, so the
  # two build paths produce .nro files that differ only where the
  # toolchain makes them differ.
  t_nacp = custom_target(
    t + '.nacp',
    output : t + '.nacp',
    command : [
      nacptool, '--create', t, 'mesa-nvk-horizon', 'phase1', '@OUTPUT@',
    ],
  )

  test_nros += custom_target(
    t + '.nro',
    input : [t_elf, t_nacp],
    output : t + '.nro',
    command : [elf2nro, '@INPUT0@', '@OUTPUT@', '--nacp=@INPUT1@'],
    build_by_default : true,
  )
endforeach

# No meson `test()` entries: these binaries run on a Nintendo Switch, not
# here. needs_exe_wrapper is true in the cross file for the same reason.
# Their results are recorded in STATUS.md as hardware (HW) runs.

summary({
  'toolchain' : meson.get_external_property('devkitpro_prefix', 'unknown'),
  'libnx' : meson.get_external_property('libnx_prefix', 'unknown'),
  'tests' : horizon_tests.length(),
}, section : 'mesa-nvk-horizon')
