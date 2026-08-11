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
  # Phase 4: does a GPU write to memory become visible to the CPU? Every
  # observation below t_vulkan is made through a syncpoint, which is a
  # host1x counter and not memory, so nothing had ever asked this.
  't_gpuwrite',
  't_teardown',
  # Phase 4, decision D14: the UNCACHED cache policy. Uses no Mesa, so it
  # lives with the horizon_gpu tests rather than the NVK ones.
  't_uncached',
  # Phase 3 item 6: compat/sysconf.c against the real kernel. Uses no
  # horizon_gpu — the C library and svcGetInfo only — but it is built
  # and packaged with the others because it is a hardware measurement,
  # and hardware measurements travel together.
  't_sysinfo',
  # Phase 5: what the allocator does around the shader local/shared
  # memory window. NVK reaches that aperture the moment a shader runs,
  # and blocking it off broke vkCreateDevice once (patches 0039/0040)
  # for a reason nobody measured. Uses no Mesa, on purpose: the question
  # is about the address space, not about the driver.
  't_va_window',
  # A deliberate MMU fault, and the wait that must not report success
  # for it. The fence/notifier fix was written because of a real fault
  # and has never had one since — a check that has never fired is not a
  # check. Destroys its own channel on purpose, which is also the only
  # place teardown-after-a-fault is exercised.
  't_fault',
  # Decision D15: how large a single GPFIFO entry this hardware will
  # actually execute. nxvk ramps the size at every channel creation to
  # pin a ring-size fault; this measures the number once instead, so
  # horizon_gpu_submit can bound the span rather than let a channel find
  # the limit.
  't_pbsize',
  # Phase 6 groundwork: the first test that owns the display and runs
  # with no console, so the reporting path a swapchain test will depend
  # on is measured before anything depends on it.
  't_display',
  # Phase 6, and it comes before the WSI rather than after it: our own
  # scanout buffers on the default nwindow, driven one level below
  # NWindow, so the number of slots that can be held at once is measured
  # instead of assumed. Uses no Mesa — the question is about the
  # compositor, not about Vulkan.
  't_nwindow',
]

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
)

test_nros = []

# ---------------------------------------------------------------------
# Tests 12 and 13 — Mesa's own code, measured on hardware.
# ---------------------------------------------------------------------
# Phase 3 items 4 (threads) and 5 (timers/clocks). They link the archives
# Mesa's build produced, not sources recompiled with flags of our own:
# the object under test has to be the object Mesa builds, or the
# measurement describes a different build. -DHAVE_PTHREAD,
# -DHAVE_STRUCT_TIMESPEC and -DENABLE_SHADER_CACHE are Mesa's own
# configure results here, copied so
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
# libmesa_util.a reaches blake3 through mesa-blake3.h's inline wrappers —
# disk_cache_compute_key() is one — so libblake3.a has to be on the link
# line of anything that calls into the shader cache. It was not needed
# while t_threads and t_ostime were the only two tests here, because
# neither of them hashes anything.
mesa_test_libs = [
  mesa_build_dir / 'src/c11/impl/libmesa_util_c11.a',
  mesa_build_dir / 'src/util/libmesa_util.a',
  mesa_build_dir / 'src/util/blake3/libblake3.a',
]
mesa_tests = ['t_threads', 't_ostime', 't_shader_cache']

# ---------------------------------------------------------------------
# Test 14 — the mandatory Vulkan sequence (Phase 4 exit criterion).
# ---------------------------------------------------------------------
# It links the whole nouveau Vulkan driver, so it needs a different Mesa
# build from tests 12 and 13: scripts/configure-mesa-nvk.sh's, which has
# -Dvulkan-drivers=nouveau. Skipped with a message when that build is
# not there, exactly as those two are — a bare clone must still produce
# the tests that need only the toolchain.
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
nvk_tests = [
  't_vulkan',
  # Phase 5 item 1. Everything later in the phase reads its result back
  # through a copy, so this is the one that has nothing behind it.
  't_vk_transfer',
  # Phase 7: the only test that asks what the shader cache is FOR — is a
  # shader compiled on one launch still compiled on the next. Needs two
  # launches of the same build to mean anything; the first is cold and
  # says so.
  't_vk_cache',
  # Phase 5 items 3 and 4. One test because an off-screen image is only
  # observable once something renders to it, and the smallest thing that
  # does is a clear — which NVK implements as a render pass, not a blit.
  't_vk_image',
  # Phase 5 item 2 — the first shader this project runs. NAK and NIL
  # have been linked since Phase 4 and have never produced machine code
  # that executed.
  't_vk_compute',
  # Phase 5 item 5 — the first draw call. Vertex processing,
  # rasterisation and the ROP, none of which any earlier item reaches.
  't_vk_triangle',
  # Phase 5 item 6 — sampling: descriptors, samplers and mip levels.
  't_vk_texture',
  # Phase 5 item 7 — the depth test, and the depth buffer read back as
  # a value rather than trusted because the colours came out right.
  't_vk_depth',
  # Phase 5 item 8 — twelve colour formats, each checked against the
  # bytes its encoding says it must hold.
  't_vk_format',
  # Phase 5 item 9 — several submits in flight at once, demonstrated by
  # an unsignalled fence and by wall clock, not by absence of errors.
  't_vk_submits',
  # Not an item: the two things this project's own patches changed and
  # nothing had ever measured — patch 0046's extension gating, checked
  # in both directions, and patch 0045's alloc_tiled_mem, reached
  # through the one path that can reach it.
  't_vk_caps',
  # Phase 6: the swapchain itself. It owns the display, so it runs with
  # no console and reports through its log alone — and every one of the
  # phase's four exit criteria is a section of it.
  't_vk_swapchain',
  # Phase 6, the part t_vk_swapchain does not reach: the swapchain while
  # something else legitimately happens at the same time, and over a
  # length that a slot leaked once per generation would show up in.
  # STATUS.md listed "anything multi-threaded" as never verified.
  't_vk_wsi_mt',
  # Phase 6: VK_SUBOPTIMAL_KHR, and the line between it and
  # VK_ERROR_OUT_OF_DATE_KHR. STATUS.md listed it as one of the things
  # that "never had a test".
  't_vk_suboptimal',
  # Phase 6: VK_PRESENT_MODE_IMMEDIATE_KHR, which STATUS.md listed
  # beside VK_SUBOPTIMAL_KHR as advertised and never measured through
  # Vulkan. It compares the two present modes against each other in one
  # run, so the verdict is a ratio on one console rather than a number
  # carried between runs.
  't_vk_immediate',
]

# Shaders each NVK test needs, by test name. An entry is the stem of
# tests/shaders/<stem>.spvasm; scripts/spv-embed.py assembles it with
# spirv-as, validates it with spirv-val and writes <stem>.spv.h into the
# build directory, which the test includes.
#
# Assembly rather than GLSL because the container has no network and the
# base image has no glslang, while SPIRV-Tools is already in the derived
# image for mesa_clc. See scripts/spv-embed.py for the trade.
nvk_test_shaders = {
  't_vulkan' : [],
  't_vk_transfer' : [],
  # None on purpose: t_vk_image must reach its first render pass without
  # anything having been uploaded to the shader heap, or it stops being
  # able to catch the regression patch 0048 fixed.
  't_vk_image' : [],
  't_vk_compute' : ['comp_write_id'],
  # The same shader t_vk_compute uses, on purpose: a failure in t_vk_cache
  # should be about the cache and not about a shader nothing else runs.
  't_vk_cache' : ['comp_write_id'],
  # Two pipelines: A builds its positions from gl_VertexIndex and writes
  # a constant colour, B fetches position and colour from a vertex
  # buffer and interpolates. Four shaders so that a failure in B alone
  # names attribute fetch or interpolation.
  't_vk_triangle' : ['tri_vert_index', 'tri_frag_const',
                     'tri_vert_attrib', 'tri_frag_interp'],
  # One vertex shader for all three cases; two fragment shaders because
  # implicit and explicit LOD are different instructions.
  't_vk_texture' : ['tex_vert_fullscreen', 'tex_frag_sample',
                    'tex_frag_lod1'],
  # One pair for all four draws: what differs between them is twenty
  # bytes of push constant and a scissor rectangle.
  't_vk_depth' : ['depth_vert_z', 'depth_frag_pc'],
  # Three fragment shaders because a fragment output's numeric type has
  # to match its attachment's.
  't_vk_format' : ['fmt_vert_fullscreen', 'fmt_frag_float',
                   'fmt_frag_uint', 'fmt_frag_sint'],
  # A compute job whose duration the test controls, so it can still be
  # running when the next submit is issued.
  't_vk_submits' : ['comp_spin'],
  # Shared with t_vk_format: the same full-target triangle and the same
  # push-constant colour, because what t_vk_caps measures is where the
  # pixels land, not what draws them.
  't_vk_caps' : ['fmt_vert_fullscreen', 'fmt_frag_float'],
  # None: a presented frame here is a vkCmdClearColorImage, so nothing
  # about the swapchain depends on a shader compiling.
  't_vk_swapchain' : [],
  # None, for the same reason.
  't_vk_wsi_mt' : [],
  # None, for the same reason.
  't_vk_suboptimal' : [],
  # None, for the same reason.
  't_vk_immediate' : [],
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
  horizon_tests += mesa_tests
else
  message('skipping ' + ' '.join(mesa_tests) + ': no Mesa archives in ' +
          mesa_build_dir + '; run scripts/configure-mesa.sh and ' +
          'scripts/build-mesa.sh first')
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
    # tests that do not present nothing but a few declarations.
    compile_args : ['-DVK_USE_PLATFORM_VI_NN'],
  )
  horizon_tests += nvk_tests
else
  message('skipping ' + ' '.join(nvk_tests) + ': no NVK archives in ' +
          nvk_build_dir + '; run scripts/build-mesa-nvk.sh first')
endif

# Shader assembly. Gated on the same condition as the tests that use the
# headers: spirv-as and spirv-val live in the derived toolchain image,
# which is also the only thing that can have produced the NVK archives,
# so a tree with the archives has the tools and a tree without them
# needs neither.
nvk_shader_headers = {}
if nvk_libs_present
  spirv_as = find_program('spirv-as')
  spirv_val = find_program('spirv-val')
  spv_embed = find_program('scripts/spv-embed.py')

  # Each shader is assembled ONCE, however many tests include it: two
  # custom_targets writing the same output is a configure error, and
  # t_vk_image and t_vk_compute already share one.
  shader_header_by_name = {}
  foreach t : nvk_tests
    foreach s : nvk_test_shaders.get(t, [])
      if not shader_header_by_name.has_key(s)
        shader_header_by_name += { s : custom_target(
          s + '.spv.h',
          input : files('tests/shaders' / (s + '.spvasm')),
          output : s + '.spv.h',
          # --target-env vulkan1.1 is SPIR-V 1.3, which is where
          # StorageBuffer becomes a core storage class. Higher would work
          # on this device and buy nothing the tests need.
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

  foreach t : nvk_tests
    t_headers = []
    foreach s : nvk_test_shaders.get(t, [])
      t_headers += shader_header_by_name[s]
    endforeach
    nvk_shader_headers += { t : t_headers }
  endforeach
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
  t_sources = [files('tests' / (t + '.c'))]
  if mesa_libs_present and mesa_tests.contains(t)
    t_deps += idep_mesa_core
    t_link_depends = mesa_test_lib_paths
  endif
  if nvk_libs_present and nvk_tests.contains(t)
    t_deps += idep_nvk_driver
    t_link_depends = nvk_test_lib_paths
    # The Vulkan fixture, compiled into each test that has a driver to
    # talk to rather than built as a library: it needs Mesa's Vulkan
    # headers, which only these tests have on their include path, and a
    # per-test object keeps the tests that predate it unchanged.
    #
    # Its generated shader headers are listed as sources so ninja
    # produces them before the test compiles; nothing links from them.
    t_sources += files('tests/common/vkfw.c')
    t_sources += nvk_shader_headers.get(t, [])
  endif

  # name_suffix 'elf': elf2nro takes an ELF, and naming it so keeps the
  # intermediate distinguishable from the .nro in the build directory.
  t_elf = executable(
    t,
    t_sources,
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
