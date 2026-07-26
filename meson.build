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
]

testfw = static_library(
  'testfw',
  files('tests/common/testfw.c'),
  include_directories : [horizon_gpu_inc, tests_inc],
  install : false,
)

test_nros = []

foreach t : horizon_tests
  # name_suffix 'elf': elf2nro takes an ELF, and naming it so keeps the
  # intermediate distinguishable from the .nro in the build directory.
  t_elf = executable(
    t,
    files('tests' / (t + '.c')),
    include_directories : [horizon_gpu_inc, tests_inc],
    link_with : testfw,
    dependencies : idep_horizon_gpu,
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
