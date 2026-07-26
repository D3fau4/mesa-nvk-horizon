#!/usr/bin/env bash
# Builds the configured Mesa tree.
#
#   scripts/build-mesa.sh [meson compile args / target names...]
#
# With no arguments this builds Mesa's non-driver core explicitly, by
# name. It has to: configured with no drivers at all, almost nothing is
# build_by_default, so a bare `ninja` builds one generated header and
# reports success. Phase 3's exit criterion is that the non-driver core
# *builds*, so the libraries are named here rather than inferred from a
# default target that does not cover them.
#
# Everything goes through `meson compile` rather than ninja directly:
# Mesa's generators are Python and need mako on PYTHONPATH, which
# horizon_meson supplies and a bare ninja does not.
#
# Idempotent in the way a build is: re-running builds only what changed.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

MESA_BUILD_DIR="${MESA_BUILD_DIR:-build/mesa-probe}"

# src/meson.build builds these subdirectories unconditionally — every
# driver is behind a with_* condition — so this list *is* the non-driver
# core, not a selection from it. Meson target names, as reported by
# `meson introspect --targets`, not ninja output paths.
MESA_CORE_TARGETS=(
    mesa_util_c11
    mesa_util
    mesa_util_simd
    blake3
    parson
    xmlconfig
    compiler
    nir
    vtn
    isaspec
)

# gtest is deliberately NOT in that list, and not because it fails to
# build. src/gtest is declared build_by_default : false and every
# idep_gtest consumer is inside `if with_tests`; the build-tests option
# defaults to false, so nothing in this configuration links it. It is
# Mesa's unit-test framework, not part of the non-driver core, and its
# tests could not run here anyway — needs_exe_wrapper is true and there
# is no emulator, which makes running them a hardware-class question by
# this project's own rules. Forcing it to build measured this, recorded
# so it is not rediscovered: bundled googletest does not compile against
# newlib under Mesa's strict -std=c++17 ('fileno', 'strdup', 'fdopen',
# '::mkstemp' not declared in gtest-port.h / gtest-port.cc). Anyone
# turning on -Dbuild-tests=true will meet that first.

if [ ! -f "$MESA_BUILD_DIR/build.ninja" ]; then
    echo "no configured build in $MESA_BUILD_DIR; configuring first"
    scripts/configure-mesa.sh
fi

if [ "$#" -eq 0 ]; then
    set -- "${MESA_CORE_TARGETS[@]}"
fi

horizon_meson compile -C "$MESA_BUILD_DIR" "$@"
