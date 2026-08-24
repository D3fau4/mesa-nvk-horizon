# Derived toolchain image: the environment's nx-dev plus the three
# things Mesa's Rust half needs and that image does not carry.
#
# Build it with scripts/build-toolchain-image.sh — never with a bare
# `docker build`. The context it needs is assembled there from files
# fetched on the *host*, because containers here have no network
# (CLAUDE.md), and that includes the build container: a plain
# `docker build` of this file fails at the first RUN with
#
#   ERROR: process "/bin/sh -c ..." did not complete successfully:
#          network bridge not found
#
# which is dockerd running --bridge=none. `--network=none` makes it
# work, and the script passes it.
#
# WHAT THIS ADDS, and why each one cannot simply be installed:
#
#   libclang   bindgen dlopens it at run time. It has to be a Debian
#              bookworm build, because that is what the base image is
#              (glibc 2.36); the host here is Ubuntu 24.04 (glibc 2.39)
#              and its libclang cannot load in the container.
#   bindgen    Mesa generates NAK's and NIL's bindings with it. Not in
#              the base image, not in Debian's archive at the version
#              Mesa requires, and a host `cargo install` produces a
#              binary the container cannot execute.
#   cbindgen   Same, for NIL's public nil.h.
#   Rust       aarch64-nintendo-switch-freestanding is tier 3, so
#   sysroot    rustup ships no prebuilt core for it, and Meson drives
#              rustc directly — there is no -Zbuild-std in a Meson
#              build. core, alloc and compiler_builtins are compiled
#              once here instead.
#
# WHAT THIS DELIBERATELY DOES NOT ADD: Meson and Mesa's Python
# generators. Those work identically in both toolchain modes — a local
# devkitA64 install and this image — so they stay in the tree where one
# mechanism serves both. The line is: the image carries what the
# container cannot otherwise obtain or run.
#
# The base image is a floating tag by policy (toolchain/versions.env:
# the Switch toolchain belongs to the environment). This layer does not
# change that; it only adds to whatever that tag currently resolves to,
# and scripts/package-horizon.sh records the digest that was used.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT

ARG BASE_IMAGE=ghcr.io/d3fau4/nx-dev:latest
FROM ${BASE_IMAGE}

ARG RUST_TARGET
ARG BINDGEN_CLI_VERSION
ARG CBINDGEN_VERSION

# --- 1. libclang ------------------------------------------------------
#
# dpkg -i rather than dropping the .so somewhere and pointing
# $LIBCLANG_PATH at it: installed properly, clang-sys finds it in the
# standard search path with no environment variable at all, and the
# loader resolves libLLVM-16's own dependencies (libedit, libz3,
# libxml2, libzstd, ...) from the base image without an
# LD_LIBRARY_PATH.
COPY debs/ /tmp/horizon-ctx/debs/
RUN dpkg -i /tmp/horizon-ctx/debs/*.deb && rm -rf /tmp/horizon-ctx/debs

# --- 2. bindgen and cbindgen -----------------------------------------
#
# --offline --locked: each .crate ships the Cargo.lock it was published
# with, and the vendor directory was filled from exactly that on the
# host. Letting cargo re-resolve would ask for crates nobody fetched,
# and there is no network here to fetch them from.
COPY rust-tools-src/ /tmp/horizon-ctx/rust-tools-src/
COPY rust-tools-vendor/ /tmp/horizon-ctx/rust-tools-vendor/
COPY cargo-config-tools.toml /tmp/horizon-ctx/cargo-home/config.toml
RUN CARGO_HOME=/tmp/horizon-ctx/cargo-home \
    PATH=/opt/cargo/bin:$PATH \
    sh -c 'set -eu; \
      for p in bindgen-cli-${BINDGEN_CLI_VERSION} cbindgen-${CBINDGEN_VERSION}; do \
        cargo install --offline --locked \
              --path /tmp/horizon-ctx/rust-tools-src/$p \
              --root /opt/rust-tools \
              --target-dir /tmp/horizon-ctx/target; \
      done'

# --- 3. core, alloc and compiler_builtins for the Switch target -------
#
# --release, not the default dev profile: an unoptimised core is both
# enormous and slow, and Mesa is configured --buildtype=plain, so
# nothing downstream ever asks for a debug standard library.
#
# The driver package exists only so cargo has something to be invoked
# on; -Zbuild-std compiles core and alloc as its dependencies, and
# those artefacts are what gets installed. The driver's own rlib is
# dropped.
#
# THE ARTEFACTS ARE FOUND, NOT GLOBBED OUT OF deps/, because where cargo
# leaves them is not a stable interface. This read
# `.../release/deps/*.rlib` until 2026-08-24, when the base image's
# rolling nightly (rustc 1.100.0-nightly, c54751567 2026-08-22) moved to
# a layout with no deps/ directory at all:
#
#   cp: cannot stat '.../release/deps/*.rlib': No such file or directory
#
# and the same build now leaves them one level down, per crate:
#
#   .../release/build/core/25fdf654ef0abfef/out/libcore-25fdf654ef0abfef.rlib
#   .../release/build/alloc/8421d24a80ac7ee8/out/liballoc-8421d24a80ac7ee8.rlib
#   .../release/build/compiler_builtins/.../libcompiler_builtins-....rlib
#
# THE .rmeta GOES WITH IT, and that is not tidiness. In this layout the
# rlib carries a metadata stub and the full metadata is a sibling file —
# core is 655 KB of rlib beside 66 MB of rmeta — so a sysroot holding
# only the rlibs builds and then fails the moment anything reads it:
#
#   error: only metadata stub found for `rlib` dependency `core`
#          please provide path to the corresponding .rmeta file
#
# which is what § 4b's staticlib probe reported once the copy above
# started finding anything at all.
#
# A find over the profile directory collects them wherever cargo put
# them, which covers both layouts and the next one. The exclusion is by
# name rather than by directory for the same reason: the driver's own
# rlib is emitted both as `libhorizon_sysroot_driver-<hash>.rlib` and,
# in the new layout, as a bare `libhorizon_sysroot_driver.rlib` beside
# the profile root. The three-crate check below is what turns a layout
# this stops matching into a build failure rather than a sysroot that is
# quietly missing core.
#
# `-exec cp -t "$libdir" {} +`, and the terminator is the reason. This
# RUN is a `sh -c '...'` inside the Dockerfile's own `/bin/sh -c`, so a
# quoted `';'` closes that string early: find never receives its
# terminator and the rest of the recipe is handed to the outer shell as
# a command name.
#
#   find: missing argument to `-exec'
#   /bin/sh: 1: ;       for want in core alloc ...: not found
#
# `+` needs no quote and no backslash at either level, and runs one cp
# instead of one per file.
COPY rust-vendor/ /tmp/horizon-ctx/rust-vendor/
COPY cargo-config-std.toml /tmp/horizon-ctx/cargo-home-std/config.toml
COPY sysroot-driver/ /tmp/horizon-ctx/sysroot-driver/
RUN CARGO_HOME=/tmp/horizon-ctx/cargo-home-std \
    PATH=/opt/cargo/bin:$PATH \
    sh -c 'set -eu; \
      cargo build --offline --release \
            --manifest-path /tmp/horizon-ctx/sysroot-driver/Cargo.toml \
            --target-dir /tmp/horizon-ctx/sysroot-target \
            -Z build-std=core,alloc \
            --target "${RUST_TARGET}"; \
      libdir=/opt/rust-sysroot/lib/rustlib/${RUST_TARGET}/lib; \
      mkdir -p "$libdir"; \
      find "/tmp/horizon-ctx/sysroot-target/${RUST_TARGET}/release" \
           \( -name "*.rlib" -o -name "*.rmeta" \) \
           ! -name "libhorizon_sysroot_driver*" \
           -exec cp -t "$libdir" {} +; \
      for want in core alloc compiler_builtins; do \
        ls "$libdir"/lib${want}-*.rlib >/dev/null || \
          { echo "error: $want missing from the built sysroot" >&2; exit 1; }; \
      done; \
      host=$(rustc -vV | sed -n "s/^host: //p"); \
      ln -sfn "$(rustc --print sysroot)/lib/rustlib/$host" \
              "/opt/rust-sysroot/lib/rustlib/$host"'

# --- 4. the OpenCL-C compiler's dependencies --------------------------
#
# NVK compiles two .cl files into SPIR-V at build time, so Mesa needs
# mesa_clc — a *build-machine* tool that wants LLVM, clang,
# SPIRV-LLVM-Translator, SPIRV-Tools and libclc.
# scripts/fetch-clc-deps.sh explains why LLVM 15 and why SPIRV-Tools is
# the one piece built from source.
#
# The .deb closure was resolved on the host against what this image
# already has, so `dpkg -i` here installs a complete set and needs no
# network — which is just as well, since there is none.
COPY clc-debs/ /tmp/horizon-ctx/clc-debs/
RUN dpkg -i /tmp/horizon-ctx/clc-debs/*.deb && rm -rf /tmp/horizon-ctx/clc-debs

# SPIRV-Tools >= 2024.1; bookworm ships 2023.1. Built shared, so
# Mesa's `dependency('SPIRV-Tools')` finds a .pc file and mesa_clc
# links the same library everything else does.
#
# -DSPIRV-Headers_SOURCE_DIR is how SPIRV-Tools is told where its
# headers dependency lives without the gclient/DEPS machinery it
# normally uses to fetch it.
COPY spirv-src/ /tmp/horizon-ctx/spirv-src/
RUN cmake -S /tmp/horizon-ctx/spirv-src/SPIRV-Tools \
          -B /tmp/horizon-ctx/spirv-build -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DBUILD_SHARED_LIBS=ON \
          -DSPIRV_SKIP_TESTS=ON \
          -DSPIRV_SKIP_EXECUTABLES=OFF \
          -DSPIRV-Headers_SOURCE_DIR=/tmp/horizon-ctx/spirv-src/SPIRV-Headers \
 && cmake --build /tmp/horizon-ctx/spirv-build --parallel \
 && cmake --install /tmp/horizon-ctx/spirv-build \
 && ldconfig \
 && rm -rf /tmp/horizon-ctx/spirv-src /tmp/horizon-ctx/spirv-build

# --- 4b. prove the image can do the work, here rather than after ------
#
# These two probes used to run after the build, as `docker run -v
# "$PWD":"$PWD"`. That cannot work when the job itself is inside a
# container: $PWD is the job container's path, the daemon resolves the
# bind against the *host*, finds nothing, and mounts an empty directory
# — so bindgen reported `No such file or directory (os error 2)` about a
# header that was right there. Measured on Forgejo's act_runner, task
# 1274.
#
# As RUN steps they need no mount at all, and a failure means no image
# rather than an image that has to be un-tagged afterwards.
COPY probe/ /tmp/horizon-ctx/probe/

# bindgen with a real header. `bindgen --version` proves nothing:
# clang-sys loads libclang lazily, so the version prints happily on an
# image with no libclang at all — which is exactly how the first attempt
# at this passed while being broken.
RUN PATH=/opt/rust-tools/bin:/opt/cargo/bin:$PATH \
    bindgen /tmp/horizon-ctx/probe/probe.h -o /tmp/horizon-ctx/probe/probe.rs \
 && grep -q 'struct probe_s' /tmp/horizon-ctx/probe/probe.rs \
 && PATH=/opt/rust-tools/bin:/opt/cargo/bin:$PATH cbindgen --version

# And that the sysroot is usable by a bare rustc, which is how Meson
# drives it — Meson never calls cargo. A *staticlib*, because that is
# what Mesa builds NAK and NIL as (rust_abi : 'c'), and a staticlib is
# where the two items only a whole program can supply become mandatory:
#   error: no global memory allocator found but one is required
#   error: `#[panic_handler]` function required, but not found
# Both were emitted by the first version of this check, which had
# neither. Supplying them in probe/s.rs keeps the check about the
# sysroot.
RUN PATH=/opt/cargo/bin:$PATH \
    rustc --target "${RUST_TARGET}" --sysroot /opt/rust-sysroot \
          --crate-type staticlib --edition 2021 \
          -o /tmp/horizon-ctx/probe/s.a /tmp/horizon-ctx/probe/s.rs \
 && test -s /tmp/horizon-ctx/probe/s.a

# --- 4c. the image says what it is ------------------------------------
#
# WHY A FILE AND NOT ONLY A LABEL. Inside a job running *in* this image
# there is no docker to ask, and scripts/toolchain-env.sh is then in
# local mode — where scripts/package-horizon.sh used to record
# `image: local` and lose every trace of what produced the binaries.
# Attribution is that manifest's entire purpose, so the image carries
# the answer with it.
#
# The identity, not a registry digest: a digest says where an image was
# pushed, this says how it was built — the base image and ID, the pinned
# bindgen/cbindgen/LLVM versions, the resolved .deb closure and this
# Dockerfile's own hash. It is the same string the label carries and
# what build-toolchain-image.sh compares to decide on a rebuild.
ARG IMAGE_IDENTITY=unknown
# Re-declared: an ARG before the first FROM is in scope for FROM and
# nothing after it, so the first version of this wrote an empty `base=`.
# Caught by reading the file the image had just written rather than by
# assuming it said what it was told to.
ARG BASE_IMAGE
RUN printf 'identity=%s\nbase=%s\ntarget=%s\n' \
        "${IMAGE_IDENTITY}" "${BASE_IMAGE}" "${RUST_TARGET}" \
        > /etc/mesa-nvk-horizon-toolchain

# --- 5. leave nothing behind ------------------------------------------
RUN rm -rf /tmp/horizon-ctx

# The base image keeps rustup outside the default PATH and every script
# that runs here prepends what it needs anyway; this is for a developer
# who opens a shell in the image by hand.
ENV PATH=/opt/rust-tools/bin:/opt/cargo/bin:${PATH}

# Read by scripts/toolchain-env.sh to tell a derived image from the
# base one, so a stale or missing layer is a clear error rather than a
# confusing "bindgen not found" three scripts later.
LABEL org.mesa-nvk-horizon.toolchain="rust-tools+libclang+sysroot"
