# mesa-nvk-horizon — Meson build for libhorizon_gpu.a and the test
# suites.
#
# This exists to exercise toolchain/horizon-aarch64.cross (Phase 2:
# "Phase 1 tests cross-compile with the new cross file"), and to be the
# build system the Mesa work in Phase 3 plugs into.
#
# It does NOT replace the Makefile. The Makefile produced the .nro files
# that were verified on real hardware, so it stays as the reference
# path; both are expected to emit the same flags and comparable
# artefacts. Host-side unit tests remain separate
# (scripts/run-host-tests.sh) — they are built with the *host* compiler
# and sanitizers, which is a different job from this file's.
#
# ONE .nro PER SUITE. A suite is tests/<suite>/: suite.c names its cases
# and <case>.c is one of them. The three dictionaries below —
# horizon_suites, mesa_suites, nvk_suites — are the suites each
# dependency tier can build, and a case is added to one of them rather
# than by adding a .nro. See tests/README.md and CLAUDE.md.
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
  'horizon/cache/crc32.c',
  'horizon/cache/blob_cache.c',
  'horizon/debug/log.c',
  'horizon/debug/status.c',
  'horizon/device/device.c',
  'horizon/memory/mem.c',
  'horizon/vm/va_space.c',
  'horizon/vm/vm.c',
  'horizon/channel/channel.c',
  'horizon/submit/cmds.c',
  'horizon/submit/submit.c',
  'horizon/surface/surface.c',
  'horizon/sync/syncpt.c',
)

# horizon/include/ is the only public surface of the layer; the
# implementation directories are not on the include path.
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

# Same name as the Makefile's `lib` target: `meson compile -C <dir> lib`
# builds only libhorizon_gpu.a, skipping every test .nro below.
alias_target('lib', libhorizon_gpu)

# ---------------------------------------------------------------------
# Test suites — one .nro per suite, several cases per suite.
# ---------------------------------------------------------------------
# A suite is a directory: tests/<suite>/suite.c is the table of cases and
# tests/<suite>/<case>.c is one case. The case's own header comment is
# where its rationale lives — this file states only what the *build*
# needs to know, so that a fact about a test is not restated in two
# places that can drift.
#
# Packaging tools come from the cross file's [binaries]; they run on the
# build machine even though they are declared for the host toolchain,
# which is what custom_target commands do.
elf2nro = find_program('elf2nro')
nacptool = find_program('nacptool')

# Test-framework header lives in tests/common/, included as "common/…".
tests_inc = include_directories('tests')

# The suites that need nothing but the toolchain. The Makefile builds
# exactly these and scripts/check-mesa-test-parity.sh fails if the two
# lists ever disagree.
horizon_suites = {
  'platform' : ['nv_bringup', 'teardown', 'sysconf', 'crc32'],
  'gpu_memory' : ['alloc', 'nvmap', 'va_reserve', 'va_map', 'uncached',
                  'shader_window', 'borrowed_pages', 'borrowed_retry'],
  'gpu_submit' : ['channel', 'submit', 'syncpt_per_submit',
                  'syncpt_cpu_incr', 'fence_wait', 'fence_wait_many',
                  'gpu_write', 'pushbuf_size'],
  # Both cases fault the GPU on purpose and lose a channel doing it, so
  # they are one .nro that is run last rather than cases inside the two
  # suites above. tests/gpu_fault/suite.c says it in full.
  'gpu_fault' : ['sparse', 'mmu_fault'],
  # Owns the display: no console.
  'display' : ['console_handoff', 'nwindow'],
  # Needs a console *and* an operator, which is why it is neither in
  # `display` nor in `platform`.
  'dock' : ['mode_change'],
}

# The build stamp every test prints in its first lines. Regenerated on
# every build (build_always_stale), which is the whole point: a .nro on
# an SD card looks exactly like the one it replaced, and on 2026-08-05
# a hardware run was spent on the previous build's binaries without
# anything in the log saying so. See scripts/gen-build-id.sh.
build_id_h = custom_target(
  'horizon_build_id.h',
  output : 'horizon_build_id.h',
  command : [find_program('scripts/gen-build-id.sh')],
  capture : true,
  build_always_stale : true,
)

testfw = static_library(
  'testfw',
  [files('tests/common/testfw.c'), build_id_h],
  # include_directories('.') covers this build directory as well as the
  # source one, which is where the generated header lands.
  include_directories : [horizon_gpu_inc, tests_inc,
                         include_directories('.')],
  install : false,
  # Only tests need this; it must not be part of the `lib`-only default.
  build_by_default : false,
)

test_nros = []

# ---------------------------------------------------------------------
# mesa_runtime — Mesa's own code, measured on hardware.
# ---------------------------------------------------------------------
# Phase 3 items 4 (threads) and 5 (timers/clocks), plus the shader disk
# cache. They link the archives Mesa's build produced, not sources
# recompiled with flags of our own: the object under test has to be the
# object Mesa builds, or the measurement describes a different build.
# -DHAVE_PTHREAD, -DHAVE_STRUCT_TIMESPEC and -DENABLE_SHADER_CACHE are
# Mesa's own configure results here, copied so its headers declare the
# same types; both appear in build/mesa-probe/build.ninja.
#
# Skipped, with a message, when Mesa has not been built — a bare clone
# must still produce the six suites that need only the toolchain.
#
# The Makefile states the same four facts — MESA_SUITES, MESA_LIBS and
# MESA_CFLAGS' defines and include paths — and each build system is meant
# to stay readable on its own, so they are duplicated rather than shared.
# scripts/check-mesa-test-parity.sh is what enforces that: a comment
# asking for them to be kept in step is not enforcement, and the .nro
# size comparison would not notice a define present here and missing
# there.
fs = import('fs')

# Not hardcoded: scripts/configure-mesa.sh and scripts/build-mesa.sh both
# honour $MESA_BUILD_DIR, and looking somewhere else than the directory
# the caller actually built in would skip these tests while claiming
# Mesa had not been built. scripts/configure-horizon.sh passes the same
# value through (see meson.options for why it is an option and not an
# environment variable).
mesa_build_dir = get_option('mesa_build_dir')
# libmesa_util.a reaches blake3 through mesa-blake3.h's inline wrappers —
# disk_cache_compute_key() is one — so libblake3.a has to be on the link
# line of anything that calls into the shader cache. It was not needed
# while c11_threads and os_time were the only two cases here, because
# neither of them hashes anything.
mesa_test_libs = [
  mesa_build_dir / 'src/c11/impl/libmesa_util_c11.a',
  mesa_build_dir / 'src/util/libmesa_util.a',
  mesa_build_dir / 'src/util/blake3/libblake3.a',
]
mesa_suites = {
  'mesa_runtime' : ['disk_cache', 'c11_threads', 'os_time'],
}

# ---------------------------------------------------------------------
# The Vulkan suites — the whole nouveau driver.
# ---------------------------------------------------------------------
# They link the whole nouveau Vulkan driver, so they need a different
# Mesa build from mesa_runtime: scripts/configure-mesa-nvk.sh's, which
# has -Dvulkan-drivers=nouveau. Skipped with a message when that build is
# not there, exactly as that one is — a bare clone must still produce the
# suites that need only the toolchain.
#
# The order matters here and nowhere else in this file: these are static
# archives with genuine cycles between them (nvk needs vulkan_runtime,
# which needs nir, which needs mesa_util, and nvk needs all three), so
# Meson's --start-group/--end-group around a dependency's link_args is
# what makes the link resolve rather than left-to-right order.
nvk_build_dir = get_option('nvk_build_dir')
# libnvk.a IS PULLED WHOLE, and nothing else is.
#
# Upstream's own reason for the flag:
#
#   # Instruct users of this library to link with --whole-archive.
#   # Otherwise, our weak function overloads may not resolve properly.
#       -- mesa/src/vulkan/runtime/meson.build:250, :327
#
# A Vulkan driver reaches its entry points through dispatch tables
# generated with --weak, so a driver may leave one unimplemented and
# the linker resolves the gap to zero rather than reporting it. An
# archive member that nothing but a weak reference points at is
# therefore never pulled in, and what reaches the binary is a NULL
# table entry and a `nm -u` that says nothing is wrong.
#
# Measured on hardware, at the cost of a console round trip. The first
# run that reached the driver died at
#
#   ok   vkEnumeratePhysicalDevices(list) -> 0
#   <nothing further, no RESULT line>
#
# because vk_common_GetPhysicalDeviceProperties jumps through
# dispatch_table.GetPhysicalDeviceProperties2, and the binary contained
# no GetPhysicalDeviceProperties2 at all.
#
# Only libnvk.a, because it already contains the rest: upstream builds
# it with dependencies that link_whole the Vulkan runtime, and Meson
# propagates that into the static archive — 178 members, including
# vk_physical_device_properties.c.o and two copies of vk_instance.c.o.
# Listing libvulkan_runtime.a, libvulkan_lite_runtime.a or
# libvulkan_instance.a beside it links those objects a second time:
#
#   ld: .../libvulkan_instance.a.p/vk_instance.c.o: multiple definition
#       of `vk_instance_end_renderdoc_capture';
#       .../src/nouveau/vulkan/../../vulkan/runtime/libvulkan_instance.a.p/
#       vk_instance.c.o: first defined here
#
# — the same object reached by two paths. So they are not in the list
# at all, and the one archive that carries them is pulled whole.
nvk_whole_libs = [
  'src/nouveau/vulkan/libnvk.a',
]

# The rest are ordinary archives: everything needed in them is needed
# by a strong reference.
nvk_test_libs = [
  'src/nouveau/rust_runtime/libnouveau_rust_runtime.a',
  'src/nouveau/compiler/libnak.a',
  'src/nouveau/nil/liblibnil_format_table.a',
  'src/nouveau/mme/libnouveau_mme.a',
  'src/nouveau/headers/libnvidia_headers_c.a',
  'src/vulkan/util/libvulkan_util.a',
  'src/vulkan/wsi/libvulkan_wsi.a',
  'src/compiler/spirv/libvtn.a',
  'src/compiler/nir/libnir.a',
  'src/compiler/libcompiler.a',
  'src/compiler/rust/libcompiler_c_helpers.a',
  'src/util/libxmlconfig.a',
  'src/util/libmesa_util.a',
  'src/util/libmesa_util_simd.a',
  'src/util/blake3/libblake3.a',
  'src/c11/impl/libmesa_util_c11.a',
]

# Each suite's cases run in the order listed: a case reads its result
# back through machinery the case before it established, which used to
# be a run order in tests/README.md and is now a property of the build.
nvk_suites = {
  'vk_core' : ['bringup', 'transfer', 'upload_chunks', 'device_memory',
               'image_clear',
               'capabilities', 'timestamps', 'concurrent_submits',
               'submit_batching', 'sparse_binding'],
  'vk_shaders' : ['compute_dispatch', 'dynamic_loop', 'packed_bound_loop',
                  'nested_control_flow', 'nested_control_flow_frag',
                  'crs_matrix',
                  'fragment_kill', 'descriptor_set1', 'priv_reg'],
  'vk_render' : ['triangle', 'texture', 'depth', 'formats', 'multi_target',
                 'zcull', 'draw_volume'],
  # Alone, because what it reports is a compile time and a measurement
  # that runs behind other work is a different measurement.
  'vk_pipelines' : ['pipeline_volume'],
  # Alone, because a cold run deletes the driver's shader cache and it
  # needs two launches of one build to mean anything.
  'vk_cache' : ['shader_reuse'],
  # Owns the display.
  'vk_wsi' : ['swapchain', 'suboptimal', 'concurrency'],
  # Owns the display. Kept apart from vk_wsi: both of its cases report a
  # number rather than a return code.
  'vk_present' : ['drawn_frame', 'present_modes'],
}

# Shaders each case needs, by "<suite>/<case>". An entry is the stem of
# tests/shaders/<stem>.spvasm; scripts/spv-embed.py assembles it with
# spirv-as, validates it with spirv-val and writes <stem>.spv.h into the
# build directory, which the case includes.
#
# PER CASE AND NOT PER SUITE, although the build only needs the union.
# scripts/check-mesa-test-parity.sh compares each entry against the
# #include lines of that one file, and an empty list is the value worth
# guarding: vk_core/image_clear must reach its first render pass with
# nothing uploaded to NVK's shader heap or it stops being the regression
# test for patch 0030. Unioning first would let a shader another case in
# the same suite needs answer for it.
#
# Assembly rather than GLSL because the container has no network and the
# base image has no glslang, while SPIRV-Tools is already in the derived
# image for mesa_clc. See scripts/spv-embed.py for the trade.
nvk_case_shaders = {
  'vk_core/bringup' : [],
  'vk_core/transfer' : [],
  'vk_core/upload_chunks' : [],
  'vk_core/device_memory' : [],
  # None on purpose: see the note above.
  'vk_core/image_clear' : [],
  # Shared with vk_render/formats: the same full-target triangle and the
  # same push-constant colour, because what this case measures is where
  # the pixels land, not what draws them.
  'vk_core/capabilities' : ['fmt_vert_fullscreen', 'fmt_frag_float'],
  'vk_core/timestamps' : [],
  # A compute job whose duration the case controls, so it can still be
  # running when the next submit is issued.
  'vk_core/concurrent_submits' : ['comp_spin'],
  # Created and never dispatched: what the case needs is the shader-heap
  # upload that vkCreateComputePipelines performs on the upload queue's
  # channel, so that the submission after it takes a cross-channel wait.
  'vk_core/submit_batching' : ['comp_write_id'],
  'vk_core/sparse_binding' : [],

  'vk_shaders/compute_dispatch' : ['comp_write_id'],
  'vk_shaders/dynamic_loop' : ['comp_loop'],
  'vk_shaders/packed_bound_loop' : ['comp_loop2'],
  'vk_shaders/nested_control_flow' : ['comp_crs'],
  'vk_shaders/nested_control_flow_frag' : ['fmt_vert_fullscreen',
                                           'crs_frag_deep'],
  # Four fragment shaders from one template, differing in the nesting
  # depth and in how many values stay live across it — see
  # scripts/gen-crs-matrix.py, which generates them.
  'vk_shaders/crs_matrix' : ['fmt_vert_fullscreen', 'crs_mx_a', 'crs_mx_b',
                             'crs_mx_c', 'crs_mx_d', 'crs_mx_e',
                             'crs_mx_f', 'crs_mx_g', 'crs_mx_h'],
  'vk_shaders/fragment_kill' : ['fmt_vert_fullscreen', 'kill_frag_loop'],
  'vk_shaders/descriptor_set1' : ['fmt_vert_fullscreen', 'set1_frag'],
  # crs_mx_g again, because the question is whether an ordinary draw
  # changes when the queue context asks for the two privileged register
  # writes, and a shader the suite already renders is the control.
  'vk_shaders/priv_reg' : ['fmt_vert_fullscreen', 'crs_mx_g'],

  # Two pipelines: A builds its positions from gl_VertexIndex and writes
  # a constant colour, B fetches position and colour from a vertex
  # buffer and interpolates. Four shaders so that a failure in B alone
  # names attribute fetch or interpolation.
  'vk_render/triangle' : ['tri_vert_index', 'tri_frag_const',
                          'tri_vert_attrib', 'tri_frag_interp'],
  # One vertex shader for all three cases; two fragment shaders because
  # implicit and explicit LOD are different instructions.
  'vk_render/texture' : ['tex_vert_fullscreen', 'tex_frag_sample',
                         'tex_frag_lod1'],
  # One pair for all four draws: what differs between them is twenty
  # bytes of push constant and a scissor rectangle.
  'vk_render/depth' : ['depth_vert_z', 'depth_frag_pc'],
  # Three fragment shaders because a fragment output's numeric type has
  # to match its attachment's.
  'vk_render/formats' : ['fmt_vert_fullscreen', 'fmt_frag_float',
                         'fmt_frag_uint', 'fmt_frag_sint'],
  # kill_frag_loop with its kill turned off: already proven right on this
  # chip, so the only variable across the cases is the target count.
  'vk_render/multi_target' : ['fmt_vert_fullscreen', 'kill_frag_loop'],
  # The depth pair vk_render/depth already uses, on purpose: what zcull
  # measures is which fragments survive, so the draws should be ones
  # whose behaviour is already established. comp_write_id is never
  # dispatched — the pipeline is built and destroyed so that NVK uploads
  # a shader through the upload queue, which is a different channel.
  'vk_render/zcull' : ['depth_vert_z', 'depth_frag_pc', 'comp_write_id'],
  # The same depth pair again. What this case varies is the number of
  # draws, the pipeline between them and the blend state — none of which
  # is a property of the shaders.
  'vk_render/draw_volume' : ['depth_vert_z', 'depth_frag_pc'],

  # One shader, specialized many times. comp_write_id would not do: it
  # is four instructions and identical every time, so a hundred
  # pipelines built from it are one compile and one heap allocation.
  'vk_pipelines/pipeline_volume' : ['comp_chain'],

  # The same shader vk_shaders/compute_dispatch uses, on purpose: a
  # failure here should be about the cache and not about a shader
  # nothing else runs.
  'vk_cache/shader_reuse' : ['comp_write_id'],

  # None: a presented frame in these three is a vkCmdClearColorImage, so
  # nothing about the swapchain depends on a shader compiling.
  'vk_wsi/swapchain' : [],
  'vk_wsi/suboptimal' : [],
  'vk_wsi/concurrency' : [],

  # Shared with vk_render/texture, and shared on purpose: the frame this
  # case presents has to be a fullscreen textured blit that already
  # works, or a slow frame here would be one more thing to rule out
  # rather than a measurement. Same triangle, same sampler, a
  # surface-sized texture.
  'vk_present/drawn_frame' : ['tex_vert_fullscreen', 'tex_frag_sample'],
  # None, for the same reason as the vk_wsi three.
  'vk_present/present_modes' : [],
}

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
    compile_args : ['-DHAVE_PTHREAD', '-DHAVE_STRUCT_TIMESPEC',
                    '-DENABLE_SHADER_CACHE'],
    include_directories : include_directories('mesa/src', 'mesa/include'),
    # One list, not two: a second `link_args` key would silently replace
    # the first rather than add to it, and the archives are the half that
    # would have been lost.
    #
    # -lzstd -lz because enabling the shader cache put compress.c on the
    # reachable side of libmesa_util.a, and Mesa's own configure enabled
    # it through devkitPro's portlibs. libhorizon_gpu.a is already on the
    # line — every test links it — and the group makes the order between
    # them irrelevant. The Makefile names the same libraries and says why
    # order matters there instead.
    link_args : mesa_test_lib_paths + [
      '-L' + meson.get_external_property('devkitpro_prefix') /
             'portlibs/switch/lib',
      '-lzstd', '-lz',
    ],
  )
else
  message('skipping ' + ' '.join(mesa_suites.keys()) +
          ': no Mesa archives in ' + mesa_build_dir +
          '; run scripts/configure-mesa.sh and scripts/build-mesa.sh first')
endif

nvk_whole_lib_paths = []
foreach l : nvk_whole_libs
  nvk_whole_lib_paths += meson.project_source_root() / nvk_build_dir / l
endforeach

nvk_test_lib_paths = []
foreach l : nvk_test_libs
  nvk_test_lib_paths += meson.project_source_root() / nvk_build_dir / l
endforeach

nvk_libs_present = true
foreach p : nvk_whole_lib_paths + nvk_test_lib_paths
  if not fs.exists(p)
    nvk_libs_present = false
  endif
endforeach

if nvk_libs_present
  idep_nvk_driver = declare_dependency(
    include_directories : include_directories(
      'mesa/src', 'mesa/include', 'mesa/src/vulkan/util'),
    # -lstdc++: NVK compiles nvkcl.cpp (the C++ vtn_bindgen2 produces
    # from the OpenCL shaders), and its exception personality routine
    # lives there. One C++ file in an otherwise C driver.
    #
    # --whole-archive covers exactly the four archives listed above and
    # is turned off again immediately: whole-archiving the rest costs
    # size for nothing, and whole-archiving two archives built from the
    # same sources collides on every symbol in them.
    #
    # -lzstd -lz: enabling the shader cache put compress.c on the
    # reachable side of libmesa_util.a, and Mesa's own configure enabled
    # it through devkitPro's portlibs. Same libraries idep_mesa_core
    # names, for the same reason.
    link_args : ['-Wl,--whole-archive'] + nvk_whole_lib_paths +
                ['-Wl,--no-whole-archive'] + nvk_test_lib_paths +
                ['-lstdc++',
                 '-L' + meson.get_external_property('devkitpro_prefix') /
                        'portlibs/switch/lib',
                 '-lzstd', '-lz'],
    # The same platform define the driver was built with. Without it
    # <vulkan/vulkan.h> hides VK_NN_vi_surface — VkViSurfaceCreateInfoNN,
    # PFN_vkCreateViSurfaceNN, the extension's name macro — so a test
    # could not name the surface type the driver advertises. It costs the
    # suites that do not present nothing but a few declarations.
    compile_args : ['-DVK_USE_PLATFORM_VI_NN'],
  )
else
  message('skipping ' + ' '.join(nvk_suites.keys()) + ': no NVK archives in ' +
          nvk_build_dir + '; run scripts/build-mesa-nvk.sh first')
endif

# Shader assembly. Gated on the same condition as the suites that use the
# headers: spirv-as and spirv-val live in the derived toolchain image,
# which is also the only thing that can have produced the NVK archives,
# so a tree with the archives has the tools and a tree without them
# needs neither.
nvk_suite_shaders = {}
if nvk_libs_present
  spirv_as = find_program('spirv-as')
  spirv_val = find_program('spirv-val')
  spv_embed = find_program('scripts/spv-embed.py')

  # Each shader is assembled ONCE, however many cases include it: two
  # custom_targets writing the same output is a configure error, and
  # vk_shaders/compute_dispatch and vk_cache/shader_reuse already share
  # one.
  shader_header_by_name = {}
  foreach suite, cases : nvk_suites
    foreach c : cases
      foreach s : nvk_case_shaders.get(suite + '/' + c, [])
        if not shader_header_by_name.has_key(s)
          shader_header_by_name += { s : custom_target(
            s + '.spv.h',
            input : files('tests/shaders' / (s + '.spvasm')),
            output : s + '.spv.h',
            # --target-env vulkan1.1 is SPIR-V 1.3, which is where
            # StorageBuffer becomes a core storage class. Higher would
            # work on this device and buy nothing the tests need.
            command : [
              spv_embed,
              '--spirv-as', spirv_as.full_path(),
              '--spirv-val', spirv_val.full_path(),
              '--target-env', 'vulkan1.1',
              '-o', '@OUTPUT@', '@INPUT@',
            ],
          ) }
        endif
      endforeach
    endforeach
  endforeach

  # The union per suite, which is what a .nro actually compiles in. A
  # header listed twice would be a duplicate source, so the names are
  # collected before the targets are.
  foreach suite, cases : nvk_suites
    s_names = []
    foreach c : cases
      foreach s : nvk_case_shaders.get(suite + '/' + c, [])
        if not s_names.contains(s)
          s_names += s
        endif
      endforeach
    endforeach
    s_headers = []
    foreach s : s_names
      s_headers += shader_header_by_name[s]
    endforeach
    nvk_suite_shaders += { suite : s_headers }
  endforeach
endif

# One dictionary of everything this configuration can build, so the loop
# below has a single shape. The tier a suite belongs to still decides
# what it links, and that is read back from the three lists.
all_suites = horizon_suites
if mesa_libs_present
  all_suites += mesa_suites
endif
if nvk_libs_present
  all_suites += nvk_suites
endif

foreach suite, cases : all_suites
  # mesa_runtime additionally sees Mesa's headers and archives; the
  # Vulkan suites see the whole driver; the rest are built with no Mesa
  # in sight, as before.
  t_deps = [idep_horizon_gpu]
  # Rebuilding Mesa must relink these, or they would keep measuring the
  # archives of an older Mesa build. Meson already infers that:
  # guess_external_link_dependencies() (ninjabackend.py:3636 in meson
  # 1.11.2, the version toolchain/versions.env pins) records any link
  # argument that is an absolute path to an existing library file, and
  # the generated rule reads
  #   build mesa_runtime.elf: c_LINKER ... | .../libmesa_util_c11.a ...
  # Declaring it as well costs one line and does not depend on that
  # inference staying a documented guess — nor on the archives existing
  # when this directory was configured, which is what the guess requires
  # and scripts/build-horizon.sh now detects separately.
  t_link_depends = []

  # The suite's table first, then its cases in the order the suite lists
  # them. suite.c is what defines suite_name, test_uses_display and the
  # case table; a case named there and not written here is a link error.
  t_sources = [files('tests' / suite / 'suite.c')]
  foreach c : cases
    t_sources += files('tests' / suite / (c + '.c'))
  endforeach

  if mesa_libs_present and mesa_suites.has_key(suite)
    t_deps += idep_mesa_core
    t_link_depends = mesa_test_lib_paths
  endif
  if nvk_libs_present and nvk_suites.has_key(suite)
    t_deps += idep_nvk_driver
    t_link_depends = nvk_test_lib_paths
    # The Vulkan fixture, compiled into each suite that has a driver to
    # talk to rather than built as a library: it needs Mesa's Vulkan
    # headers, which only these suites have on their include path, and a
    # per-suite object keeps the suites that predate it unchanged.
    #
    # Its generated shader headers are listed as sources so ninja
    # produces them before the cases compile; nothing links from them.
    t_sources += files('tests/common/vkfw.c')
    t_sources += nvk_suite_shaders.get(suite, [])
  endif

  # name_suffix 'elf': elf2nro takes an ELF, and naming it so keeps the
  # intermediate distinguishable from the .nro in the build directory.
  t_elf = executable(
    suite,
    t_sources,
    include_directories : [horizon_gpu_inc, tests_inc],
    link_with : testfw,
    dependencies : t_deps,
    link_depends : t_link_depends,
    name_suffix : 'elf',
    install : false,
    build_by_default : false,
  )

  # Metadata matches the Makefile's nacptool invocation exactly, so the
  # two build paths produce .nro files that differ only where the
  # toolchain makes them differ.
  t_nacp = custom_target(
    suite + '.nacp',
    output : suite + '.nacp',
    command : [
      nacptool, '--create', suite, 'mesa-nvk-horizon', 'phase1', '@OUTPUT@',
    ],
  )

  test_nros += custom_target(
    suite + '.nro',
    input : [t_elf, t_nacp],
    output : suite + '.nro',
    command : [elf2nro, '@INPUT0@', '@OUTPUT@', '--nacp=@INPUT1@'],
    # Not the default build: see the `lib`/`test` alias_targets below.
    build_by_default : false,
  )
endforeach

# The Makefile's `lib`/`test` split, as far as Meson allows it: bare
# `meson compile` builds only libhorizon_gpu.a (the alias_target above),
# and this one adds every test .nro this configuration can produce.
#
# NOT called `test`. Meson reserves `all`, `clean`, `test`, `benchmark`,
# `install`, `dist`, `uninstall` and `reconfigure` for itself and refuses
# the whole configure with "Target name 'test' is reserved for Meson's
# internal use" — which is what it did, so no cross build could be
# configured at all. scripts/build-horizon.sh maps its documented `test`
# and `all` arguments onto this name, so its interface still matches the
# Makefile's.
alias_target('test-nros', test_nros)

# No meson `test()` entries: these binaries run on a Nintendo Switch, not
# here. needs_exe_wrapper is true in the cross file for the same reason.
# Their results are hardware (HW) runs, and are recorded as such
# together with the log that produced them.

n_cases = 0
foreach suite, cases : all_suites
  n_cases += cases.length()
endforeach

summary({
  'toolchain' : meson.get_external_property('devkitpro_prefix', 'unknown'),
  'libnx' : meson.get_external_property('libnx_prefix', 'unknown'),
  'suites' : all_suites.keys().length(),
  'cases' : n_cases,
}, section : 'mesa-nvk-horizon')
