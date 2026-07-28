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
      for r in /tmp/horizon-ctx/sysroot-target/${RUST_TARGET}/release/deps/*.rlib; do \
        case "$(basename "$r")" in libhorizon_sysroot_driver-*) continue ;; esac; \
        cp "$r" "$libdir/"; \
      done; \
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
